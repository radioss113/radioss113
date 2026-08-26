#include "wifi_station.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dns_server.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "WIFI_STA";

#define AP_SSID               "Radio SS113 Stazione Ascolto"
#define WIFI_CONNECT_GRACE_MS 30000
#define WIFI_SCAN_RECONNECT_HOLD_MS 5000
#define WIFI_SCAN_DISCONNECT_SETTLE_MS 250
#define WIFI_SCAN_RECONNECT_DELAY_MS 750
#define MODE_RESET_HOLD_MS     5000
#define RESET_LED_BLINK_MS       80
#define RESET_LED_BLINK_COUNT    12
#define STATUS_LED_PULSE_MS     100
#define STATUS_LED_WIFI_SEARCH_PERIOD_MS 1000
#define STATUS_LED_AP_PERIOD_MS 2000
#define STATUS_LED_RESET_FAST_PERIOD_MS 160
#define STATUS_LED_GPIO          GPIO_NUM_22
#define STATUS_LED_ACTIVE_LOW        1
#define RESET_BUTTON_GPIO        GPIO_NUM_5
#define RESET_BUTTON_ACTIVE_LEVEL    0
#define RESET_BUTTON_POLL_MS        20

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_event_group;
static wifi_station_stats_t s_stats;
static runtime_config_t s_runtime_config;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_http_server;
static dns_server_handle_t s_dns_server;
static bool s_initialized;
static bool s_wifi_started;
static bool s_ap_active;
static TickType_t s_connect_attempt_start_tick;
static TickType_t s_last_disconnect_tick;
static TickType_t s_suspend_sta_connect_until_tick;
static TickType_t s_pending_sta_connect_tick;
static volatile bool s_reconfigure_requested;
static volatile bool s_factory_reset_requested;
static bool s_reset_button_task_started;
static bool s_status_led_task_started;
static char s_captive_uri[64];

typedef enum {
    STATUS_LED_OFF = 0,
    STATUS_LED_ON,
    STATUS_LED_WIFI_SEARCH,
    STATUS_LED_AP_OPEN,
    STATUS_LED_FACTORY_RESET,
} status_led_mode_t;

static volatile status_led_mode_t s_status_led_mode = STATUS_LED_OFF;

static void html_escape(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    if (dst_size == 0) {
        return;
    }
    for (size_t i = 0; src[i] != 0 && j + 1 < dst_size; ++i) {
        const char *rep = NULL;
        switch (src[i]) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default:
                dst[j++] = src[i];
                continue;
        }
        size_t rep_len = strlen(rep);
        if (j + rep_len >= dst_size) {
            break;
        }
        memcpy(dst + j, rep, rep_len);
        j += rep_len;
    }
    dst[j] = 0;
}

static void js_escape_double_quoted(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    if (dst_size == 0) {
        return;
    }
    for (size_t i = 0; src[i] != 0 && j + 1 < dst_size; ++i) {
        char c = src[i];
        if (c == '\\' || c == '"' || c == '\'') {
            if (j + 2 >= dst_size) {
                break;
            }
            dst[j++] = '\\';
            dst[j++] = c;
        } else if ((unsigned char)c >= 32) {
            dst[j++] = c;
        }
    }
    dst[j] = 0;
}

static void html_appendf(char *dst, size_t dst_size, size_t *offset, const char *fmt, ...)
{
    if (*offset >= dst_size) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(dst + *offset, dst_size - *offset, fmt, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    size_t advance = (size_t)written;
    if (advance >= dst_size - *offset) {
        *offset = dst_size - 1;
    } else {
        *offset += advance;
    }
}

static void set_led_raw(bool on)
{
    int level = on ? 1 : 0;
    if (STATUS_LED_ACTIVE_LOW) {
        level = on ? 0 : 1;
    }
    gpio_set_level(STATUS_LED_GPIO, level);
}

static void configure_status_led_output(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    set_led_raw(false);
}

static void status_led_set_mode(status_led_mode_t mode)
{
    s_status_led_mode = mode;
}

static void status_led_task(void *arg)
{
    (void)arg;

    bool led_on = false;
    TickType_t last_toggle = xTaskGetTickCount();

    while (true) {
        status_led_mode_t mode = s_status_led_mode;
        uint32_t period_ms = 0;
        uint32_t on_ms = STATUS_LED_PULSE_MS;

        switch (mode) {
            case STATUS_LED_ON:
                if (!led_on) {
                    led_on = true;
                    set_led_raw(true);
                }
                last_toggle = xTaskGetTickCount();
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            case STATUS_LED_WIFI_SEARCH:
                period_ms = STATUS_LED_WIFI_SEARCH_PERIOD_MS;
                break;
            case STATUS_LED_AP_OPEN:
                period_ms = STATUS_LED_AP_PERIOD_MS;
                break;
            case STATUS_LED_FACTORY_RESET:
                period_ms = STATUS_LED_RESET_FAST_PERIOD_MS;
                on_ms = STATUS_LED_RESET_FAST_PERIOD_MS / 2;
                break;
            case STATUS_LED_OFF:
            default:
                if (led_on) {
                    led_on = false;
                    set_led_raw(false);
                }
                last_toggle = xTaskGetTickCount();
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
        }

        TickType_t now = xTaskGetTickCount();
        uint32_t elapsed_ms = pdTICKS_TO_MS(now - last_toggle);
        uint32_t target_ms = led_on ? on_ms : (period_ms > on_ms ? period_ms - on_ms : on_ms);
        if (elapsed_ms >= target_ms) {
            led_on = !led_on;
            set_led_raw(led_on);
            last_toggle = now;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static esp_err_t init_status_led_service(void)
{
    configure_status_led_output();
    if (!s_status_led_task_started) {
        BaseType_t ok = xTaskCreatePinnedToCore(status_led_task, "status_led", 2048, NULL, 2, NULL, 0);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "status led task create failed");
        s_status_led_task_started = true;
    }
    return ESP_OK;
}

static void blink_reset_led(void)
{
    for (int i = 0; i < RESET_LED_BLINK_COUNT; ++i) {
        set_led_raw(true);
        vTaskDelay(pdMS_TO_TICKS(RESET_LED_BLINK_MS));
        set_led_raw(false);
        vTaskDelay(pdMS_TO_TICKS(RESET_LED_BLINK_MS));
    }
}

static void url_decode(char *dst, const char *src)
{
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = 0;
}

static bool form_get_value(const char *body, const char *key, char *out, size_t out_size)
{
    size_t key_len = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;
            const char *end = strchr(p, '&');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            char temp[256];
            if (len >= sizeof(temp)) {
                len = sizeof(temp) - 1;
            }
            memcpy(temp, p, len);
            temp[len] = 0;
            url_decode(out, temp);
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    if (out_size) {
        out[0] = 0;
    }
    return false;
}

static void start_config_server(void);
static void start_config_dns(void);
static void stop_config_dns(void);
static void start_config_ap(void);
static void stop_config_ap(void);

static bool tick_deadline_active(TickType_t now, TickType_t deadline)
{
    return deadline != 0 && (TickType_t)(deadline - now) < (TickType_t)(portMAX_DELAY / 2);
}

static bool tick_deadline_reached(TickType_t now, TickType_t deadline)
{
    return deadline != 0 && !tick_deadline_active(now, deadline);
}

static bool sta_connect_suspended(TickType_t now)
{
    return tick_deadline_active(now, s_suspend_sta_connect_until_tick);
}

static void schedule_sta_reconnect(uint32_t delay_ms)
{
    if (!s_runtime_config.have_wifi_credentials) {
        return;
    }
    s_pending_sta_connect_tick = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
}

static void apply_sta_config_and_connect(void)
{
    if (!s_runtime_config.have_wifi_credentials) {
        return;
    }

    wifi_mode_t target_mode = s_ap_active ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    ESP_ERROR_CHECK(esp_wifi_set_mode(target_mode));

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, s_runtime_config.saved_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, s_runtime_config.saved_pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply STA config: %s", esp_err_to_name(cfg_err));
        return;
    }
    ESP_LOGI(TAG, "Connecting to saved SSID: %s", s_runtime_config.saved_ssid);
    if (s_connect_attempt_start_tick == 0) {
        s_connect_attempt_start_tick = xTaskGetTickCount();
    }
    esp_wifi_connect();
}

static esp_err_t networks_get_handler(httpd_req_t *req)
{
    TickType_t now = xTaskGetTickCount();
    s_suspend_sta_connect_until_tick = now + pdMS_TO_TICKS(WIFI_SCAN_RECONNECT_HOLD_MS);

    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
    };
    uint16_t ap_count = 20;
    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_records) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    if (s_ap_active) {
        esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (mode_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to keep APSTA before scan: %s", esp_err_to_name(mode_err));
        }
    }
    if (s_runtime_config.have_wifi_credentials && !s_stats.connected) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(WIFI_SCAN_DISCONNECT_SETTLE_MS));
    }

    esp_err_t scan_err = esp_wifi_scan_start(&scan_cfg, true);
    if (scan_err == ESP_OK) {
        esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    } else {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(scan_err));
        ap_count = 0;
    }
    s_suspend_sta_connect_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(WIFI_SCAN_RECONNECT_DELAY_MS);
    if (s_runtime_config.have_wifi_credentials && !s_stats.connected) {
        schedule_sta_reconnect(WIFI_SCAN_RECONNECT_DELAY_MS);
    }

    char *html = calloc(1, 8192);
    if (!html) {
        free(ap_records);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    size_t off = 0;
    if (ap_count == 0) {
        html_appendf(html, 8192, &off,
                     "<div class='meta'>Nessuna rete visibile al momento. Puoi comunque inserire SSID e password a mano.</div>");
    } else {
        for (uint16_t i = 0; i < ap_count; ++i) {
            char ssid_html[128];
            char ssid_js[64];
            html_escape((const char *)ap_records[i].ssid, ssid_html, sizeof(ssid_html));
            js_escape_double_quoted((const char *)ap_records[i].ssid, ssid_js, sizeof(ssid_js));
            bool secured = ap_records[i].authmode != WIFI_AUTH_OPEN;
            html_appendf(html, 8192, &off,
                         "<button class='network' type='button' onclick='setSsid(\"%s\")'>%s"
                         "<div class='meta'>RSSI %d dBm%s</div></button>",
                         ssid_js, ssid_html, ap_records[i].rssi, secured ? " • protetta" : " • aperta");
        }
    }

    httpd_resp_set_type(req, "text/html");
    esp_err_t resp = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    free(ap_records);
    return resp;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char saved_ssid_html[128];
    char stream_uri_html[256];
    char stream_base_html[256];
    char stream_name_html[128];
    char resolved_uri[STREAM_URI_MAX_LEN + STREAM_DEVICE_NAME_MAX_LEN];
    html_escape(s_runtime_config.saved_ssid, saved_ssid_html, sizeof(saved_ssid_html));
    html_escape(active_stream_uri(&s_runtime_config, resolved_uri, sizeof(resolved_uri)), stream_uri_html, sizeof(stream_uri_html));
    html_escape(s_runtime_config.stream_uri[0] ? s_runtime_config.stream_uri : "", stream_base_html, sizeof(stream_base_html));
    html_escape(s_runtime_config.stream_device_name, stream_name_html, sizeof(stream_name_html));

    esp_netif_ip_info_t sta_ip = {0};
    char ip_buf[32] = "non disponibile";
    if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &sta_ip) == ESP_OK && sta_ip.ip.addr != 0) {
        snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&sta_ip.ip));
    }

    const size_t html_cap = 12288;
    char *html = calloc(1, html_cap);
    if (!html) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    size_t off = 0;
    html_appendf(html, html_cap, &off,
                 "<!doctype html><html><head><meta charset='utf-8'>"
                 "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                 "<title>RadioSS113 Setup</title>"
                 "<style>"
                 "body{font-family:Arial,sans-serif;margin:20px;max-width:760px;background:#111;color:#f4f4f4}"
                 "h1,h2{margin:0 0 12px 0}"
                 ".card{background:#1b1b1b;padding:16px;border-radius:12px;margin-bottom:18px}"
                 ".network{display:block;width:100%%;text-align:left;margin:8px 0;padding:10px 12px;"
                 "border:1px solid #333;border-radius:10px;background:#202020;color:#fff}"
                 "input{width:100%%;font-size:16px;padding:10px;box-sizing:border-box;border-radius:8px;border:1px solid #444;background:#0f0f0f;color:#fff}"
                 "button[type=submit]{font-size:16px;padding:12px 18px;margin-top:12px;border-radius:10px;border:0;background:#d83030;color:#fff}"
                 ".meta{opacity:.75;font-size:14px;line-height:1.45}"
                 "</style>"
                 "<script>"
                 "function setSsid(v){document.getElementById('ssid').value=v;document.getElementById('password').focus();}"
                 "async function loadNetworks(){"
                 "const box=document.getElementById('networks');"
                 "box.innerHTML='<div class=\"meta\">Scansione reti in corso...</div>';"
                 "try{const r=await fetch('/networks'); box.innerHTML=await r.text();}"
                 "catch(e){box.innerHTML='<div class=\"meta\">Impossibile leggere la lista reti.</div>';}"
                 "}"
                 "window.addEventListener('load', loadNetworks);"
                 "</script>"
                 "</head><body>"
                 "<h1>RadioSS113 Setup</h1>"
                 "<div class='card'>"
                 "<div class='meta'>Rete di configurazione: <strong>%s</strong></div>"
                 "<div class='meta'>SSID salvato: <strong>%s</strong></div>"
                 "<div class='meta'>Nome dispositivo: <strong>%s</strong></div>"
                 "<div class='meta'>Stream target: <strong>%s</strong></div>"
                 "<div class='meta'>IP STA: <strong>%s</strong></div>"
                 "<div class='meta'>%s</div>"
                 "<div class='meta'>Se l'URL termina con /, il nome dispositivo viene aggiunto automaticamente.</div>"
                 "</div>",
                 AP_SSID,
                 s_runtime_config.saved_ssid[0] ? saved_ssid_html : "(nessuno)",
                 s_runtime_config.stream_device_name[0] ? stream_name_html : "(nessuno)",
                 stream_uri_html,
                 ip_buf,
                 stream_uri_requirements_text());

    html_appendf(html, html_cap, &off,
                 "<div class='card'><h2>Reti Wi-Fi trovate</h2><div id='networks'><div class='meta'>Scansione reti in corso...</div></div></div>");

    html_appendf(html, html_cap, &off,
                 "<div class='card'><h2>Configura rete e stream</h2>"
                 "<form method='post' action='/save'>"
                 "<label>SSID<br><input id='ssid' name='ssid' maxlength='32' required value='%s'></label><br><br>"
                 "<label>Password<br><input id='password' name='password' type='password' maxlength='64'></label><br><br>"
                 "<label>Nome dispositivo<br><input id='device_name' name='device_name' maxlength='63' required value='%s'></label><br><br>"
                 "<label>Stream URL base<br><input id='stream_url' name='stream_url' maxlength='191' value='%s'></label><br><br>"
                 "<label>Password stream<br><input id='stream_password' name='stream_password' type='password' maxlength='63' value='%s'></label><br><br>"
                 "<button type='submit'>Salva e applica</button>"
                 "</form></div></body></html>",
                 s_runtime_config.saved_ssid[0] ? saved_ssid_html : "",
                 s_runtime_config.stream_device_name[0] ? stream_name_html : "",
                 stream_base_html,
                 active_stream_password(&s_runtime_config));

    httpd_resp_set_type(req, "text/html");
    esp_err_t resp = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return resp;
}

static esp_err_t redirect_to_root_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[768];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    int offset = 0;
    while (offset < total) {
        int read = httpd_req_recv(req, body + offset, total - offset);
        if (read <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Read failed");
            return ESP_FAIL;
        }
        offset += read;
    }
    body[offset] = 0;

    char ssid[64] = {0};
    char pass[128] = {0};
    char stream_uri[STREAM_URI_MAX_LEN] = {0};
    char stream_name[STREAM_DEVICE_NAME_MAX_LEN] = {0};
    char stream_password[STREAM_PASSWORD_MAX_LEN] = {0};
    if (!form_get_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID missing");
        return ESP_FAIL;
    }
    form_get_value(body, "password", pass, sizeof(pass));
    if (!form_get_value(body, "device_name", stream_name, sizeof(stream_name)) || stream_name[0] == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Device name missing");
        return ESP_FAIL;
    }
    normalize_device_name(stream_name, sizeof(stream_name));
    if (stream_name[0] == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Device name invalid");
        return ESP_FAIL;
    }
    if (!form_get_value(body, "stream_url", stream_uri, sizeof(stream_uri)) || stream_uri[0] == 0) {
        strlcpy(stream_uri, CONFIG_HARBOR_URI, sizeof(stream_uri));
    }
    if (!form_get_value(body, "stream_password", stream_password, sizeof(stream_password)) || stream_password[0] == 0) {
        strlcpy(stream_password, CONFIG_HARBOR_PASSWORD, sizeof(stream_password));
    }
    normalize_stream_uri(stream_uri, sizeof(stream_uri));
    if (!stream_uri_is_supported(stream_uri)) {
        char resp[1024];
        snprintf(resp, sizeof(resp),
                 "<html><body><h1>URL non supportata</h1>"
                 "<p>Hai inserito: <strong>%s</strong></p>"
                 "<p>%s</p>"
                 "<p>Per esempio: <strong>%s</strong></p>"
                 "<p><a href='/'>Torna alla configurazione</a></p>"
                 "</body></html>",
                 stream_uri, stream_uri_requirements_text(), CONFIG_HARBOR_URI);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    }

    ESP_ERROR_CHECK(save_runtime_config(ssid, pass, stream_uri, stream_name, stream_password));
    strlcpy(s_runtime_config.saved_ssid, ssid, sizeof(s_runtime_config.saved_ssid));
    strlcpy(s_runtime_config.saved_pass, pass, sizeof(s_runtime_config.saved_pass));
    strlcpy(s_runtime_config.stream_uri, stream_uri, sizeof(s_runtime_config.stream_uri));
    strlcpy(s_runtime_config.stream_device_name, stream_name, sizeof(s_runtime_config.stream_device_name));
    strlcpy(s_runtime_config.stream_password, stream_password, sizeof(s_runtime_config.stream_password));
    s_runtime_config.have_wifi_credentials = true;
    s_stats.have_saved_credentials = true;
    s_last_disconnect_tick = xTaskGetTickCount();
    s_reconfigure_requested = true;

    const char *resp =
        "<html><body><h1>Saved</h1>"
        "<p>Configurazione salvata.</p>"
        "<p>Il dispositivo sta applicando le impostazioni e si riavviera' tra pochi secondi.</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static void start_config_server(void)
{
    if (s_http_server) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 12;
    if (httpd_start(&s_http_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start config portal HTTP server");
        return;
    }

    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL};
    httpd_uri_t wifi = {.uri = "/wifi", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post_handler, .user_ctx = NULL};
    httpd_uri_t networks = {.uri = "/networks", .method = HTTP_GET, .handler = networks_get_handler, .user_ctx = NULL};
    httpd_uri_t android_204 = {.uri = "/generate_204", .method = HTTP_GET, .handler = redirect_to_root_handler, .user_ctx = NULL};
    httpd_uri_t hotspot_detect = {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = redirect_to_root_handler, .user_ctx = NULL};
    httpd_uri_t connect_test = {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = redirect_to_root_handler, .user_ctx = NULL};
    httpd_uri_t ncsi = {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = redirect_to_root_handler, .user_ctx = NULL};
    httpd_uri_t wildcard = {.uri = "/*", .method = HTTP_GET, .handler = redirect_to_root_handler, .user_ctx = NULL};

    httpd_register_uri_handler(s_http_server, &root);
    httpd_register_uri_handler(s_http_server, &wifi);
    httpd_register_uri_handler(s_http_server, &save);
    httpd_register_uri_handler(s_http_server, &networks);
    httpd_register_uri_handler(s_http_server, &android_204);
    httpd_register_uri_handler(s_http_server, &hotspot_detect);
    httpd_register_uri_handler(s_http_server, &connect_test);
    httpd_register_uri_handler(s_http_server, &ncsi);
    httpd_register_uri_handler(s_http_server, &wildcard);
}

static void start_config_dns(void)
{
    if (s_dns_server) {
        return;
    }
    dns_server_config_t config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns_server = start_dns_server(&config);
}

static void stop_config_dns(void)
{
    if (s_dns_server) {
        stop_dns_server(s_dns_server);
        s_dns_server = NULL;
    }
}

static void start_config_ap(void)
{
    if (s_ap_active) {
        return;
    }

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(AP_SSID);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_LOGW(TAG, "Starting config AP: %s", AP_SSID);
    status_led_set_mode(STATUS_LED_AP_OPEN);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    }

    esp_netif_ip_info_t ap_ip = {0};
    if (esp_netif_get_ip_info(s_ap_netif, &ap_ip) == ESP_OK) {
        snprintf(s_captive_uri, sizeof(s_captive_uri), "http://" IPSTR "/", IP2STR(&ap_ip.ip));
        esp_netif_dhcps_option(s_ap_netif,
                               ESP_NETIF_OP_SET,
                               ESP_NETIF_CAPTIVEPORTAL_URI,
                               s_captive_uri,
                               strlen(s_captive_uri) + 1);
        ESP_LOGI(TAG, "Captive portal URI: %s", s_captive_uri);
    }

    start_config_server();
    start_config_dns();
    s_ap_active = true;
    s_stats.portal_active = true;
}

static void stop_config_ap(void)
{
    if (!s_ap_active) {
        return;
    }

    s_ap_active = false;
    s_stats.portal_active = false;
    stop_config_dns();
    status_led_set_mode(s_stats.connected ? STATUS_LED_ON : STATUS_LED_WIFI_SEARCH);
    if (s_runtime_config.have_wifi_credentials) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }
}

static void reset_button_task(void *arg)
{
    (void)arg;

    TickType_t press_start = 0;
    bool latched = false;

    while (true) {
        bool pressed = gpio_get_level(RESET_BUTTON_GPIO) == RESET_BUTTON_ACTIVE_LEVEL;
        TickType_t now = xTaskGetTickCount();

        if (pressed) {
            if (press_start == 0) {
                press_start = now;
            } else if (!latched && pdTICKS_TO_MS(now - press_start) >= MODE_RESET_HOLD_MS) {
                latched = true;
                s_factory_reset_requested = true;
                status_led_set_mode(STATUS_LED_FACTORY_RESET);
                ESP_LOGW(TAG, "GPIO5 long press detected: factory reset requested");
            }
        } else {
            press_start = 0;
            latched = false;
        }

        vTaskDelay(pdMS_TO_TICKS(RESET_BUTTON_POLL_MS));
    }
}

static esp_err_t init_reset_button_service(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << RESET_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "reset button gpio config failed");

    if (!s_reset_button_task_started) {
        BaseType_t ok = xTaskCreatePinnedToCore(reset_button_task, "reset_button", 3072, NULL, 3, NULL, 0);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "reset button task create failed");
        s_reset_button_task_started = true;
    }
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            return;
        }

        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            s_stats.connected = false;
            s_stats.disconnect_count++;
            s_stats.last_disconnect_reason = disc ? disc->reason : 0;
            s_stats.last_disconnect_us = esp_timer_get_time();
            s_stats.ip_addr[0] = 0;
            s_last_disconnect_tick = xTaskGetTickCount();
            if (s_connect_attempt_start_tick == 0) {
                s_connect_attempt_start_tick = s_last_disconnect_tick;
            }
            if (s_event_group) {
                xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
            }
            status_led_set_mode(s_ap_active ? STATUS_LED_AP_OPEN : STATUS_LED_WIFI_SEARCH);
            ESP_LOGW(TAG, "Wi-Fi disconnected: reason=%ld count=%" PRIu32,
                     (long)s_stats.last_disconnect_reason,
                     s_stats.disconnect_count);
            if (s_runtime_config.have_wifi_credentials) {
                TickType_t now = xTaskGetTickCount();
                if (sta_connect_suspended(now)) {
                    schedule_sta_reconnect(WIFI_SCAN_RECONNECT_DELAY_MS);
                    ESP_LOGI(TAG, "STA reconnect deferred during Wi-Fi scan");
                } else {
                    esp_wifi_connect();
                }
            }
            return;
        }
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got_ip = (ip_event_got_ip_t *)event_data;
        s_stats.connected = true;
        s_stats.last_connect_us = esp_timer_get_time();
        if (got_ip) {
            snprintf(s_stats.ip_addr, sizeof(s_stats.ip_addr), IPSTR, IP2STR(&got_ip->ip_info.ip));
        } else {
            s_stats.ip_addr[0] = 0;
        }
        if (s_event_group) {
            xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        }
        s_connect_attempt_start_tick = 0;
        status_led_set_mode(STATUS_LED_ON);
        ESP_LOGI(TAG, "Wi-Fi connected: ip=%s", s_stats.ip_addr[0] ? s_stats.ip_addr : "unknown");
        if (s_ap_active) {
            stop_config_ap();
        }
    }
}

static void wifi_station_tick(void *arg)
{
    (void)arg;

    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (tick_deadline_reached(now, s_pending_sta_connect_tick) && !sta_connect_suspended(now)) {
            s_pending_sta_connect_tick = 0;
            if (s_runtime_config.have_wifi_credentials && !s_stats.connected) {
                apply_sta_config_and_connect();
            }
        }
        if (s_runtime_config.have_wifi_credentials && !s_stats.connected) {
            if (s_connect_attempt_start_tick == 0) {
                s_connect_attempt_start_tick = now;
            }
            TickType_t elapsed = now - s_connect_attempt_start_tick;
            if (!s_ap_active && pdTICKS_TO_MS(elapsed) >= WIFI_CONNECT_GRACE_MS) {
                start_config_ap();
            }
        }
        if (s_stats.connected && s_ap_active) {
            stop_config_ap();
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

esp_err_t wifi_station_init(const char *ssid, const char *password)
{
    (void)ssid;
    (void)password;

    if (s_initialized) {
        return ESP_OK;
    }

    memset(&s_stats, 0, sizeof(s_stats));
    load_default_runtime_config(&s_runtime_config);
    s_runtime_config.have_wifi_credentials = load_runtime_config(&s_runtime_config);
    s_stats.have_saved_credentials = s_runtime_config.have_wifi_credentials;

    s_event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_event_group != NULL, ESP_ERR_NO_MEM, TAG, "wifi event group alloc failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "esp_event_loop_create_default failed");
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
                        TAG, "wifi event register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL),
                        TAG, "ip event register failed");

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi set storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "wifi set ps failed");

    ESP_RETURN_ON_ERROR(init_status_led_service(), TAG, "status led init failed");
    ESP_RETURN_ON_ERROR(init_reset_button_service(), TAG, "reset button init failed");

    BaseType_t task_ok = xTaskCreatePinnedToCore(wifi_station_tick, "wifi_station_tick", 2048, NULL, 4, NULL, 0);
    ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, TAG, "wifi tick task create failed");

    start_config_server();

    s_last_disconnect_tick = xTaskGetTickCount();
    if (s_runtime_config.have_wifi_credentials) {
        status_led_set_mode(STATUS_LED_WIFI_SEARCH);
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi set mode failed");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
        s_wifi_started = true;
        apply_sta_config_and_connect();
    } else {
        status_led_set_mode(STATUS_LED_AP_OPEN);
        start_config_ap();
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Wi-Fi station manager started");
    return ESP_OK;
}

bool wifi_station_is_connected(void)
{
    return s_stats.connected;
}

void wifi_station_get_stats(wifi_station_stats_t *stats)
{
    if (!stats) {
        return;
    }
    *stats = s_stats;
}

void wifi_station_get_runtime_config(runtime_config_t *config)
{
    if (!config) {
        return;
    }
    *config = s_runtime_config;
}

bool wifi_station_take_reconfigure_request(void)
{
    bool requested = s_reconfigure_requested;
    s_reconfigure_requested = false;
    return requested;
}

bool wifi_station_take_factory_reset_request(void)
{
    bool requested = s_factory_reset_requested;
    s_factory_reset_requested = false;
    return requested;
}

void wifi_station_perform_reconfigure_restart(void)
{
    ESP_LOGI(TAG, "Applying saved configuration by restart");
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

void wifi_station_perform_factory_reset_and_restart(void)
{
    ESP_LOGW(TAG, "Factory reset requested from MODE button");
    status_led_set_mode(STATUS_LED_FACTORY_RESET);
    erase_runtime_config();
    blink_reset_led();
    esp_restart();
}
