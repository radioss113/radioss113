#include "runtime_config.h"

#include <ctype.h>
#include <string.h>

#include "esp_check.h"
#include "nvs.h"
#include "sdkconfig.h"

#define NVS_NS              "a1sstream"
#define NVS_KEY_SSID        "wifi_ssid"
#define NVS_KEY_PASS        "wifi_pass"
#define NVS_KEY_URI         "stream_uri"
#define NVS_KEY_STREAM_NAME "stream_name"
#define NVS_KEY_STREAM_PWD  "stream_pwd"

static const char *TAG = "RUNTIME_CFG";

static void trim_ascii_whitespace(char *s)
{
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = 0;

    if (start != s) {
        memmove(s, start, (size_t)(end - start) + 1);
    }
}

void load_default_runtime_config(runtime_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->stream_uri, CONFIG_HARBOR_URI, sizeof(cfg->stream_uri));
    strlcpy(cfg->stream_device_name, "test", sizeof(cfg->stream_device_name));
    strlcpy(cfg->stream_password, CONFIG_HARBOR_PASSWORD, sizeof(cfg->stream_password));
}

bool stream_uri_is_supported(const char *uri)
{
    return uri && strncmp(uri, "http://", 7) == 0;
}

void normalize_device_name(char *name, size_t size)
{
    if (!name || size == 0) {
        return;
    }

    trim_ascii_whitespace(name);
    size_t w = 0;
    bool last_dash = false;
    for (size_t i = 0; name[i] != 0 && w + 1 < size; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (isalnum(c)) {
            name[w++] = (char)tolower(c);
            last_dash = false;
        } else if (w > 0 && !last_dash) {
            name[w++] = '-';
            last_dash = true;
        }
    }
    while (w > 0 && name[w - 1] == '-') {
        w--;
    }
    name[w] = 0;
}

void normalize_stream_uri(char *uri, size_t uri_size)
{
    if (!uri || uri_size == 0) {
        return;
    }

    trim_ascii_whitespace(uri);
    if (uri[0] == 0) {
        strlcpy(uri, CONFIG_HARBOR_URI, uri_size);
        return;
    }

    if (strstr(uri, "://") == NULL) {
        char normalized[STREAM_URI_MAX_LEN];
        snprintf(normalized, sizeof(normalized), "http://%s", uri);
        strlcpy(uri, normalized, uri_size);
    }
}

const char *stream_uri_requirements_text(void)
{
    return "Questa build accetta solo stream URL http://";
}

esp_err_t save_runtime_config(const char *ssid,
                              const char *pass,
                              const char *stream_uri,
                              const char *stream_name,
                              const char *stream_password)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(ssid && ssid[0], ESP_ERR_INVALID_ARG, TAG, "ssid missing");
    ESP_RETURN_ON_FALSE(stream_uri && stream_uri[0], ESP_ERR_INVALID_ARG, TAG, "stream uri missing");
    ESP_RETURN_ON_FALSE(stream_name && stream_name[0], ESP_ERR_INVALID_ARG, TAG, "stream name missing");
    ESP_RETURN_ON_FALSE(stream_password && stream_password[0], ESP_ERR_INVALID_ARG, TAG, "stream password missing");

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &nvs), TAG, "nvs_open failed");
    ESP_GOTO_ON_ERROR(nvs_set_str(nvs, NVS_KEY_SSID, ssid), fail, TAG, "save ssid failed");
    ESP_GOTO_ON_ERROR(nvs_set_str(nvs, NVS_KEY_PASS, pass ? pass : ""), fail, TAG, "save pass failed");
    ESP_GOTO_ON_ERROR(nvs_set_str(nvs, NVS_KEY_URI, stream_uri), fail, TAG, "save uri failed");
    ESP_GOTO_ON_ERROR(nvs_set_str(nvs, NVS_KEY_STREAM_NAME, stream_name), fail, TAG, "save stream name failed");
    ESP_GOTO_ON_ERROR(nvs_set_str(nvs, NVS_KEY_STREAM_PWD, stream_password), fail, TAG, "save stream password failed");
    ESP_GOTO_ON_ERROR(nvs_commit(nvs), fail, TAG, "nvs commit failed");
    nvs_close(nvs);
    return ESP_OK;

fail:
    nvs_close(nvs);
    return ret;
}

bool load_runtime_config(runtime_config_t *cfg)
{
    if (!cfg) {
        return false;
    }

    load_default_runtime_config(cfg);

    nvs_handle_t nvs;
    size_t ssid_len = sizeof(cfg->saved_ssid);
    size_t pass_len = sizeof(cfg->saved_pass);
    size_t uri_len = sizeof(cfg->stream_uri);
    size_t stream_name_len = sizeof(cfg->stream_device_name);
    size_t stream_pwd_len = sizeof(cfg->stream_password);

    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    if (nvs_get_str(nvs, NVS_KEY_SSID, cfg->saved_ssid, &ssid_len) != ESP_OK) {
        nvs_close(nvs);
        return false;
    }
    if (nvs_get_str(nvs, NVS_KEY_PASS, cfg->saved_pass, &pass_len) != ESP_OK) {
        cfg->saved_pass[0] = 0;
    }
    if (nvs_get_str(nvs, NVS_KEY_URI, cfg->stream_uri, &uri_len) != ESP_OK) {
        strlcpy(cfg->stream_uri, CONFIG_HARBOR_URI, sizeof(cfg->stream_uri));
    }
    if (nvs_get_str(nvs, NVS_KEY_STREAM_NAME, cfg->stream_device_name, &stream_name_len) != ESP_OK) {
        strlcpy(cfg->stream_device_name, "test", sizeof(cfg->stream_device_name));
    }
    if (nvs_get_str(nvs, NVS_KEY_STREAM_PWD, cfg->stream_password, &stream_pwd_len) != ESP_OK) {
        strlcpy(cfg->stream_password, CONFIG_HARBOR_PASSWORD, sizeof(cfg->stream_password));
    }
    nvs_close(nvs);

    normalize_stream_uri(cfg->stream_uri, sizeof(cfg->stream_uri));
    normalize_device_name(cfg->stream_device_name, sizeof(cfg->stream_device_name));
    if (!stream_uri_is_supported(cfg->stream_uri)) {
        strlcpy(cfg->stream_uri, CONFIG_HARBOR_URI, sizeof(cfg->stream_uri));
    }

    cfg->have_wifi_credentials = cfg->saved_ssid[0] != 0;
    return cfg->have_wifi_credentials;
}

void erase_runtime_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, NVS_KEY_SSID);
        nvs_erase_key(nvs, NVS_KEY_PASS);
        nvs_erase_key(nvs, NVS_KEY_URI);
        nvs_erase_key(nvs, NVS_KEY_STREAM_NAME);
        nvs_erase_key(nvs, NVS_KEY_STREAM_PWD);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

const char *active_stream_uri(const runtime_config_t *cfg, char *resolved_uri, size_t resolved_uri_size)
{
    if (!cfg) {
        return CONFIG_HARBOR_URI;
    }

    const char *base = cfg->stream_uri[0] ? cfg->stream_uri : CONFIG_HARBOR_URI;
    if (cfg->stream_device_name[0] == 0 || !resolved_uri || resolved_uri_size == 0) {
        return base;
    }

    size_t base_len = strlen(base);
    if (base_len > 0 && base[base_len - 1] == '/') {
        snprintf(resolved_uri, resolved_uri_size, "%s%s", base, cfg->stream_device_name);
        return resolved_uri;
    }

    return base;
}

const char *active_stream_password(const runtime_config_t *cfg)
{
    if (!cfg || cfg->stream_password[0] == 0) {
        return CONFIG_HARBOR_PASSWORD;
    }
    return cfg->stream_password;
}
