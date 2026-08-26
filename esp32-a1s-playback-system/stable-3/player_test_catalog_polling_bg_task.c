/* Variante di lavoro: parte dalla Stable 7 stabile e sperimenta il TTS MP3 live con prebuffer corto. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "board.h"
#include "audio_idf_version.h"
#include "esp_decoder.h"
#include "audio_hal.h"
#include "wav_head.h"
#include "dns_server.h"
#include "dns_server.c"

static const char *TAG = "PROVISION_TEST";
static const int VOLUME_STEP = 5;
static const bool LED_ACTIVE_LOW = false;
static const char *NVS_NS = "telefonoss113";
static const char *NVS_KEY_SSID = "wifi_ssid";
static const char *NVS_KEY_PASS = "wifi_pass";
static const char *NVS_KEY_STREAM1 = "stream1";
static const char *NVS_KEY_STREAM2 = "stream2";
static const char *NVS_KEY_STREAM3 = "stream3";
static const char *NVS_KEY_TTS_URL = "tts_url";
static const char *NVS_KEY_ANN_URL = "ann_url";
static const char *NVS_KEY_ANN_EN = "ann_en";
static const char *NVS_KEY_STARTUP_TEXT = "startup_txt";
static const char *NVS_KEY_BASE_HOST = "base_host";
static const char *NVS_KEY_FB1_URL = "fb1_url";
static const char *NVS_KEY_FB1_ST = "fb1_station";
static const char *NVS_KEY_FB1_MC = "fb1_mic";
static const char *NVS_KEY_FB2_URL = "fb2_url";
static const char *NVS_KEY_FB2_ST = "fb2_station";
static const char *NVS_KEY_FB2_MC = "fb2_mic";
static const char *NVS_KEY_FB3_URL = "fb3_url";
static const char *NVS_KEY_FB3_ST = "fb3_station";
static const char *NVS_KEY_FB3_MC = "fb3_mic";
static const char *AP_SSID = "RadioSS113 Setup";
static const char *DEFAULT_TTS_URL = "http://tts.example.invalid/stazione.mp3?engine=kokoro&voce=default_voice&speed=1.20";
static const char *DEFAULT_ANNOUNCE_URL = "http://tts.example.invalid/say.mp3?engine=kokoro&voce=default_voice&speed=1.20";
static const char *DEFAULT_STARTUP_TEXT = "Radio SS113!";
static const char *DEFAULT_BASE_HOST = "https://stream.example.invalid";
static const int WIFI_CONNECT_GRACE_MS = 30000;
static const int CATALOG_POLL_INTERVAL_MS = 30000;
static const int NETWORK_FAILURE_RECONNECT_THRESHOLD = 4;
static const int NETWORK_FAILURE_RECONNECT_COOLDOWN_MS = 5000;
static const int KEY6_FACTORY_RESET_HOLD_MS = 5000;
static const int MOUNT_BUTTON_FALLBACK_HOLD_MS = 5000;

#define STREAM_URL_MAX_LEN 192
#define STARTUP_TEXT_MAX_LEN 160
#define STATUS_LED_GPIO GPIO_NUM_23
#define TTS_BUTTON_GPIO GPIO_NUM_21
#define STATION_BUTTON_GPIO GPIO_NUM_19
#define MOUNT_BUTTON_GPIO GPIO_NUM_22
#define ICECAST_STATUS_PATH "/status-json.xsl"
#define MAX_STATIONS 12
#define MAX_MOUNTS_PER_STATION 12
#define STATION_NAME_MAX_LEN 32
#define MOUNT_NAME_MAX_LEN 32
#define SOURCE_NAME_MAX_LEN 64
#define FALLBACK_ENTRY_COUNT 3
#define ACTIVE_NAME_MAX_LEN 96
#define TTS_REQUEST_URL_MAX_LEN 512
#define TTS_LIVE_BUFFER_CAPACITY (512 * 1024)
#define TTS_LIVE_START_THRESHOLD (8 * 1024)

#define WIFI_CONNECTED_BIT BIT0

typedef struct {
    const char *name;
    const char *default_url;
    char url[192];
} stream_desc_t;

typedef enum {
    KEY_ACTION_STREAM = 0,
    KEY_ACTION_VOL_DOWN,
    KEY_ACTION_VOL_UP,
    KEY_ACTION_MUTE_TOGGLE,
    KEY_ACTION_PLAY_TTS,
    KEY_ACTION_STATION_NEXT,
    KEY_ACTION_MOUNT_NEXT,
} key_action_t;

typedef struct {
    const char *name;
    gpio_num_t gpio;
    bool has_internal_pullup;
    int last_level;
    key_action_t action;
    int stream_index;
} key_desc_t;

typedef struct {
    volatile bool stream_alive;
    volatile bool muted;
    volatile int edge_blink_count;
    volatile bool info_blink_active;
} led_state_t;

typedef struct {
    char url[STREAM_URL_MAX_LEN];
    char station_name[STATION_NAME_MAX_LEN];
    char source_name[SOURCE_NAME_MAX_LEN];
} fallback_entry_t;

typedef struct {
    char name[STATION_NAME_MAX_LEN];
    int mount_count;
    char mounts[MAX_MOUNTS_PER_STATION][MOUNT_NAME_MAX_LEN];
    char source_names[MAX_MOUNTS_PER_STATION][SOURCE_NAME_MAX_LEN];
} station_catalog_t;

typedef struct {
    char saved_ssid_html[128];
    char ssid_html[128];
    char tts_url_html[256];
    char announce_url_html[256];
    char startup_text_html[256];
    char base_host_html[256];
    char fb1_url_html[256];
    char fb1_station_html[64];
    char fb1_source_html[128];
    char fb2_url_html[256];
    char fb2_station_html[64];
    char fb2_source_html[128];
    char fb3_url_html[256];
    char fb3_station_html[64];
    char fb3_source_html[128];
    char catalog_html[1024];
} config_page_state_t;

typedef struct {
    char stream1[STREAM_URL_MAX_LEN];
    char stream2[STREAM_URL_MAX_LEN];
    char stream3[STREAM_URL_MAX_LEN];
    char tts_url[STREAM_URL_MAX_LEN];
    char announce_url[STREAM_URL_MAX_LEN];
    char startup_text[STARTUP_TEXT_MAX_LEN];
    char base_host[STREAM_URL_MAX_LEN];
    char fb1_url[STREAM_URL_MAX_LEN];
    char fb1_station[STATION_NAME_MAX_LEN];
    char fb1_source[SOURCE_NAME_MAX_LEN];
    char fb2_url[STREAM_URL_MAX_LEN];
    char fb2_station[STATION_NAME_MAX_LEN];
    char fb2_source[SOURCE_NAME_MAX_LEN];
    char fb3_url[STREAM_URL_MAX_LEN];
    char fb3_station[STATION_NAME_MAX_LEN];
    char fb3_source[SOURCE_NAME_MAX_LEN];
} config_post_values_t;

typedef enum {
    STREAM_MODE_PRESET = 0,
    STREAM_MODE_DYNAMIC,
    STREAM_MODE_FALLBACK,
} stream_mode_t;

static stream_desc_t kStreams[] = {
    { "aux_environment",      "https://catalog.example.invalid/aux_environment", "" },
    { "stereo_environment",   "https://catalog.example.invalid/stereo_environment", "" },
    { "radio",                "https://catalog.example.invalid/radio", "" },
};

static key_desc_t kKeys[] = {
    { "KEY1", GPIO_NUM_36, false, -1, KEY_ACTION_STREAM,       0 },
    { "KEY2", GPIO_NUM_13, true,  -1, KEY_ACTION_STREAM,       1 },
    { "KEY3", GPIO_NUM_19, true,  -1, KEY_ACTION_STATION_NEXT, -1 },
    { "KEY5", GPIO_NUM_18, true,  -1, KEY_ACTION_VOL_UP,      -1 },
    { "KEY6", GPIO_NUM_5,  true,  -1, KEY_ACTION_MUTE_TOGGLE, -1 },
    { "TTS",  TTS_BUTTON_GPIO, true, -1, KEY_ACTION_PLAY_TTS,  -1 },
    { "MOUNT", MOUNT_BUTTON_GPIO, true, -1, KEY_ACTION_MOUNT_NEXT, -1 },
};

static led_state_t g_led_state = {
    .stream_alive = false,
    .muted = false,
    .edge_blink_count = 0,
};

static EventGroupHandle_t g_wifi_event_group;
static httpd_handle_t g_http_server;
static esp_netif_t *g_sta_netif;
static esp_netif_t *g_ap_netif;
static bool g_wifi_connected;
static bool g_have_saved_creds;
static bool g_ap_active;
static bool g_wifi_started;
static TickType_t g_last_disconnect_tick;
static TickType_t g_last_forced_wifi_reconnect_at = 0;
static int g_network_failure_streak = 0;
static char g_saved_ssid[33];
static char g_saved_pass[65];
static char g_captive_uri[64];
static char g_tts_url[STREAM_URL_MAX_LEN];
static char g_announce_url[STREAM_URL_MAX_LEN];
static bool g_announce_enabled = true;
static char g_startup_message_text[STARTUP_TEXT_MAX_LEN];
static char g_base_host[STREAM_URL_MAX_LEN];
static fallback_entry_t g_fallback_entries[FALLBACK_ENTRY_COUNT];
static int g_current_fallback_index = 0;
static dns_server_handle_t g_dns_server;

static audio_pipeline_handle_t g_pipeline;
static audio_element_handle_t g_http_stream_reader;
static audio_element_handle_t g_i2s_stream_writer;
static audio_element_handle_t g_audio_decoder;
static audio_event_iface_handle_t g_evt;
static audio_board_handle_t g_board_handle;
static int g_current_stream = 0;
static int g_resume_stream_after_tts = 0;
static int g_current_volume = 100;
static char g_active_stream_url[STREAM_URL_MAX_LEN];
static char g_active_stream_name[ACTIVE_NAME_MAX_LEN];
static stream_mode_t g_stream_mode = STREAM_MODE_PRESET;
static int g_selected_preset_index = 0;
static bool g_catalog_available = false;
static bool g_catalog_refresh_pending = false;
static bool g_catalog_restart_requested = false;
static bool g_catalog_last_refresh_changed = false;
static station_catalog_t g_station_catalog[MAX_STATIONS];
static station_catalog_t g_previous_station_catalog[MAX_STATIONS];
static int g_previous_station_catalog_count = 0;
static bool g_previous_catalog_available = false;
static int g_station_catalog_count = 0;
static int g_current_station_index = 0;
static int g_current_mount_index = 0;
static int g_last_dynamic_station_index = 0;
static int g_last_dynamic_mount_index = 0;
static bool g_is_muted = false;
static bool g_tts_playing = false;
static bool g_tts_started = false;
static bool g_transition_mute_active = false;
static bool g_decoder_locked = false;
static bool g_pipeline_running = false;
static TaskHandle_t g_tts_task_handle = NULL;
static TaskHandle_t g_catalog_task_handle = NULL;
static volatile bool g_tts_stop_requested = false;
static bool g_tts_takeover_active = false;
static TickType_t g_ignore_restart_until = 0;
static bool g_last_logged_stream_alive = false;
static bool g_force_ap_due_to_missing_network = false;
static TickType_t g_key6_pressed_since = 0;
static bool g_key6_long_press_fired = false;
static TickType_t g_tts_pressed_since = 0;
static bool g_tts_long_press_fired = false;
static TickType_t g_mount_pressed_since = 0;
static bool g_mount_long_press_fired = false;
static bool g_pending_sta_connect = false;
static TickType_t g_pending_sta_connect_at = 0;
static TickType_t g_suspend_sta_connect_until = 0;
static bool g_stream_retry_pending = false;
static TickType_t g_stream_retry_at = 0;
static volatile bool g_stream_restart_in_progress = false;
static bool g_startup_announcement_pending = false;
static bool g_startup_announcement_played = false;
static TaskHandle_t g_catalog_poll_task_handle = NULL;

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t len;
    size_t pos;
    volatile bool download_complete;
    volatile bool playback_started;
    volatile bool download_failed;
} tts_memory_stream_t;

static tts_memory_stream_t g_tts_memory_stream = {0};

typedef struct {
    bool enabled;
    int input_channels;
    int bits;
    int bytes_per_sample;
    uint8_t *out_buf;
    size_t out_capacity;
} tts_upmix_t;

static esp_err_t tts_upmix_open(audio_element_handle_t self)
{
    tts_upmix_t *upmix = (tts_upmix_t *)audio_element_getdata(self);
    if (!upmix) {
        return ESP_FAIL;
    }
    upmix->enabled = false;
    upmix->input_channels = 2;
    upmix->bits = 16;
    upmix->bytes_per_sample = 2;
    return ESP_OK;
}

static esp_err_t tts_upmix_close(audio_element_handle_t self)
{
    (void)self;
    return ESP_OK;
}

static esp_err_t tts_upmix_destroy(audio_element_handle_t self)
{
    tts_upmix_t *upmix = (tts_upmix_t *)audio_element_getdata(self);
    if (upmix) {
        free(upmix->out_buf);
        free(upmix);
    }
    return ESP_OK;
}

static audio_element_err_t tts_upmix_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    tts_upmix_t *upmix = (tts_upmix_t *)audio_element_getdata(self);
    int r_size = audio_element_input(self, in_buffer, in_len);
    if (r_size <= 0) {
        return r_size;
    }
    if (!upmix || !upmix->enabled || upmix->input_channels != 1 || upmix->bytes_per_sample <= 0) {
        return audio_element_output(self, in_buffer, r_size);
    }

    int frame_bytes = upmix->bytes_per_sample * upmix->input_channels;
    if (frame_bytes <= 0 || (r_size % frame_bytes) != 0) {
        return audio_element_output(self, in_buffer, r_size);
    }

    size_t sample_count = (size_t)r_size / (size_t)frame_bytes;
    size_t out_size = sample_count * (size_t)upmix->bytes_per_sample * 2U;
    if (out_size > upmix->out_capacity) {
        uint8_t *new_buf = realloc(upmix->out_buf, out_size);
        if (!new_buf) {
            ESP_LOGE(TAG, "Cannot grow TTS upmix buffer to %u bytes", (unsigned)out_size);
            return AEL_IO_FAIL;
        }
        upmix->out_buf = new_buf;
        upmix->out_capacity = out_size;
    }

    for (size_t i = 0; i < sample_count; ++i) {
        uint8_t *src = (uint8_t *)in_buffer + (i * (size_t)upmix->bytes_per_sample);
        uint8_t *dst = upmix->out_buf + (i * (size_t)upmix->bytes_per_sample * 2U);
        memcpy(dst, src, (size_t)upmix->bytes_per_sample);
        memcpy(dst + upmix->bytes_per_sample, src, (size_t)upmix->bytes_per_sample);
    }

    return audio_element_output(self, (char *)upmix->out_buf, (int)out_size);
}

static audio_element_handle_t tts_upmix_init(void)
{
    tts_upmix_t *upmix = calloc(1, sizeof(tts_upmix_t));
    if (!upmix) {
        return NULL;
    }

    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open = tts_upmix_open;
    cfg.close = tts_upmix_close;
    cfg.process = tts_upmix_process;
    cfg.destroy = tts_upmix_destroy;
    cfg.buffer_len = 4 * 1024;
    cfg.out_rb_size = 16 * 1024;
    cfg.task_stack = 3 * 1024;
    cfg.tag = "tts_upmix";
    cfg.data = upmix;

    audio_element_handle_t el = audio_element_init(&cfg);
    if (!el) {
        free(upmix);
        return NULL;
    }
    return el;
}

static void configure_tts_upmix(audio_element_handle_t upmix_el, const audio_element_info_t *input_info)
{
    if (!upmix_el || !input_info) {
        return;
    }

    tts_upmix_t *upmix = (tts_upmix_t *)audio_element_getdata(upmix_el);
    if (!upmix) {
        return;
    }

    upmix->input_channels = input_info->channels;
    upmix->bits = input_info->bits;
    upmix->bytes_per_sample = input_info->bits / 8;
    upmix->enabled = (input_info->channels == 1 && upmix->bytes_per_sample > 0);

    audio_element_info_t out_info = *input_info;
    if (upmix->enabled) {
        out_info.channels = 2;
        out_info.bps = out_info.sample_rates * out_info.bits * out_info.channels;
        ESP_LOGI(TAG, "TTS mono upmix enabled: %d Hz, %d bit -> stereo", out_info.sample_rates, out_info.bits);
    }
    audio_element_setinfo(upmix_el, &out_info);
    audio_element_report_info(upmix_el);
}

static key_desc_t *find_key_by_action(key_action_t action)
{
    for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i) {
        if (kKeys[i].action == action) {
            return &kKeys[i];
        }
    }
    return NULL;
}

static void restart_current_stream(void);
static void start_audio_pipeline(void);
static void stop_audio_pipeline(void);
static void stop_config_dns(void);
static void factory_reset_settings(void);
static void schedule_stream_retry(uint32_t delay_ms, const char *reason);
static void reset_network_failure_streak(const char *reason);
static void note_network_failure_and_maybe_reconnect(const char *reason);
static void play_tts_url(void);
static void finish_tts_playback(const char *reason);
static void tts_wav_task(void *arg);
static void tts_live_task(void *arg);
static void tts_live_playback_task(void *arg);
static void set_stream_alive_state(bool alive, const char *reason);
static void update_active_stream_from_selection(void);
static bool refresh_stream_catalog(bool log_success);
static void switch_station_next(void);
static void switch_mount_next(void);
static void toggle_fallback_stream(void);
static void request_catalog_refresh(bool restart_stream);
static void catalog_refresh_task(void *arg);
static void catalog_poll_task(void *arg);
static bool fallback_entry_is_configured(const fallback_entry_t *entry);
static int find_first_fallback_index(void);
static bool has_any_fallback_stream(void);
static const fallback_entry_t *get_active_fallback_entry(void);
static int next_fallback_index(int start_index);
static const char *fallback_station_label(const fallback_entry_t *entry);
static const char *fallback_source_label(const fallback_entry_t *entry);
static bool build_tts_request_url(char *dst, size_t dst_size);
static bool build_announce_request_url(const char *text, char *dst, size_t dst_size);
static bool build_startup_message_request_url(char *dst, size_t dst_size);
static bool start_tts_request_url(const char *reason, const char *url);
static bool play_selection_announcement(bool station_change);
static bool play_startup_message(void);

#define RESTART_GUARD_MS 1500
#define STREAM_RETRY_MS 3000

static bool is_probably_wav_url(const char *url)
{
    if (!url || !url[0]) {
        return false;
    }
    const char *wav = strstr(url, ".wav");
    return wav && (wav[4] == 0 || wav[4] == '?' || wav[4] == '&');
}

static bool is_tts_busy(void)
{
    return g_tts_playing || g_tts_task_handle != NULL;
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static bool parse_wav_header_from_buffer(const uint8_t *buf, size_t len, wav_info_t *info)
{
    if (!buf || !info || len < 12) {
        return false;
    }
    if (read_le32(buf) != CHUNKID_RIFF || read_le32(buf + 8) != CHUNKID_WAVE) {
        return false;
    }

    bool fmt_found = false;
    bool data_found = false;
    size_t off = 12;
    memset(info, 0, sizeof(*info));

    while (off + 8 <= len) {
        uint32_t chunk_id = read_le32(buf + off);
        uint32_t chunk_size = read_le32(buf + off + 4);
        size_t payload_off = off + 8;

        if (chunk_id == CHUNKID_FMT) {
            if (payload_off + chunk_size > len) {
                return false;
            }
            if (chunk_size < 16) {
                return false;
            }
            info->audio_format = read_le16(buf + payload_off + 0);
            info->channels = read_le16(buf + payload_off + 2);
            info->samplerate = read_le32(buf + payload_off + 4);
            info->bitrate = read_le32(buf + payload_off + 8) * 8;
            info->block_align = read_le16(buf + payload_off + 12);
            info->bits = read_le16(buf + payload_off + 14);
            fmt_found = true;
        } else if (chunk_id == CHUNKID_DATA) {
            if (!fmt_found) {
                return false;
            }
            info->data_shift = payload_off;
            info->data_size = chunk_size;
            data_found = true;
            break;
        } else {
            if (payload_off + chunk_size > len) {
                return false;
            }
        }

        off = payload_off + chunk_size;
        if (chunk_size & 1U) {
            off++;
        }
    }

    return fmt_found && data_found;
}

static int tts_memory_read_cb(audio_element_handle_t el, char *buf, int len, TickType_t wait_time, void *ctx)
{
    (void)el;
    (void)wait_time;
    tts_memory_stream_t *stream = (tts_memory_stream_t *)ctx;
    if (!stream || !stream->data) {
        return AEL_IO_DONE;
    }
    while (stream->pos >= stream->len && !stream->download_complete && !g_tts_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (stream->pos >= stream->len) {
        if (stream->download_failed) {
            return AEL_IO_FAIL;
        }
        return AEL_IO_DONE;
    }
    size_t remaining = stream->len - stream->pos;
    if ((size_t)len > remaining) {
        len = (int)remaining;
    }
    memcpy(buf, stream->data + stream->pos, (size_t)len);
    stream->pos += (size_t)len;
    return len;
}

static void clear_tts_memory_stream(void)
{
    free(g_tts_memory_stream.data);
    memset(&g_tts_memory_stream, 0, sizeof(g_tts_memory_stream));
}

static bool play_tts_memory_buffer(void)
{
    if (!g_tts_memory_stream.data || g_tts_memory_stream.len == 0) {
        ESP_LOGE(TAG, "TTS memory playback requested without buffered audio");
        return false;
    }
    bool started = false;
    bool finished = false;
    bool local_decoder_locked = false;
    bool ok = false;
    audio_pipeline_handle_t pipeline = NULL;
    audio_element_handle_t decoder = NULL;
    audio_element_handle_t upmix = NULL;
    audio_element_handle_t i2s = NULL;
    audio_event_iface_handle_t evt = NULL;

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!pipeline) {
        ESP_LOGE(TAG, "Cannot create TTS pipeline");
        goto cleanup;
    }

    audio_decoder_t auto_decode[] = {
        DEFAULT_ESP_WAV_DECODER_CONFIG(),
        DEFAULT_ESP_MP3_DECODER_CONFIG(),
    };
    esp_decoder_cfg_t decoder_cfg = DEFAULT_ESP_DECODER_CONFIG();
    decoder_cfg.out_rb_size = 16 * 1024;
    decoder = esp_decoder_init(&decoder_cfg, auto_decode, sizeof(auto_decode) / sizeof(audio_decoder_t));
    if (!decoder) {
        ESP_LOGE(TAG, "Cannot create TTS decoder");
        goto cleanup;
    }
    audio_element_set_read_cb(decoder, tts_memory_read_cb, &g_tts_memory_stream);

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.out_rb_size = 16 * 1024;
    i2s = i2s_stream_init(&i2s_cfg);
    if (!i2s) {
        ESP_LOGE(TAG, "Cannot create TTS i2s writer");
        goto cleanup;
    }

    upmix = tts_upmix_init();
    if (!upmix) {
        ESP_LOGE(TAG, "Cannot create TTS upmix element");
        goto cleanup;
    }

    audio_pipeline_register(pipeline, decoder, "tts_dec");
    audio_pipeline_register(pipeline, upmix, "tts_upmix");
    audio_pipeline_register(pipeline, i2s, "tts_i2s");
    const char *link_tag[3] = {"tts_dec", "tts_upmix", "tts_i2s"};
    ESP_ERROR_CHECK(audio_pipeline_link(pipeline, &link_tag[0], 3));

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    evt = audio_event_iface_init(&evt_cfg);
    if (!evt) {
        ESP_LOGE(TAG, "Cannot create TTS event iface");
        goto cleanup;
    }
    audio_pipeline_set_listener(pipeline, evt);

    g_tts_memory_stream.pos = 0;
    g_tts_started = false;
    set_stream_alive_state(false, "tts pipeline starting");
    ESP_LOGI(TAG, "Starting buffered/live TTS playback (%u bytes ready)", (unsigned)g_tts_memory_stream.len);

    if (!g_is_muted) {
        ESP_ERROR_CHECK(audio_hal_set_mute(g_board_handle->audio_hal, true));
        g_transition_mute_active = true;
    }

    ESP_ERROR_CHECK(audio_pipeline_run(pipeline));
    started = true;

    while (!finished) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, pdMS_TO_TICKS(100));
        if (g_tts_stop_requested) {
            ESP_LOGI(TAG, "Stopping buffered TTS playback on request");
            audio_pipeline_stop(pipeline);
            audio_pipeline_wait_for_stop(pipeline);
            finished = true;
            break;
        }
        if (ret != ESP_OK) {
            continue;
        }
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
            && msg.source == (void *)decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(decoder, &music_info);
            configure_tts_upmix(upmix, &music_info);
            audio_element_info_t output_info = music_info;
            if (music_info.channels == 1) {
                output_info.channels = 2;
                output_info.bps = output_info.sample_rates * output_info.bits * output_info.channels;
            }
            i2s_stream_set_clk(i2s, output_info.sample_rates, output_info.bits, output_info.channels);
            local_decoder_locked = true;
            if (g_transition_mute_active && !g_is_muted) {
                ESP_ERROR_CHECK(audio_hal_set_mute(g_board_handle->audio_hal, false));
                g_transition_mute_active = false;
            }
            g_tts_started = true;
            set_stream_alive_state(true, "tts decoder locked");
        } else if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
                   && msg.source == (void *)i2s
                   && msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
            int status = (int)msg.data;
            if (status == AEL_STATUS_STATE_RUNNING) {
                if (g_transition_mute_active && local_decoder_locked && !g_is_muted) {
                    ESP_ERROR_CHECK(audio_hal_set_mute(g_board_handle->audio_hal, false));
                    g_transition_mute_active = false;
                }
                set_stream_alive_state(true, "tts i2s running");
            } else if (status == AEL_STATUS_STATE_STOPPED || status == AEL_STATUS_STATE_FINISHED) {
                set_stream_alive_state(false, "tts i2s stopped");
                finished = true;
                ok = !g_tts_stop_requested;
            }
        }
    }

cleanup:
    if (started) {
        audio_pipeline_stop(pipeline);
        audio_pipeline_wait_for_stop(pipeline);
    }
    if (pipeline && evt) {
        audio_pipeline_remove_listener(pipeline);
    }
    if (evt) {
        audio_event_iface_destroy(evt);
    }
    if (pipeline) {
        if (decoder) {
            audio_pipeline_unregister(pipeline, decoder);
        }
        if (upmix) {
            audio_pipeline_unregister(pipeline, upmix);
        }
        if (i2s) {
            audio_pipeline_unregister(pipeline, i2s);
        }
        audio_pipeline_deinit(pipeline);
    }
    if (decoder) {
        audio_element_deinit(decoder);
    }
    if (upmix) {
        audio_element_deinit(upmix);
    }
    if (i2s) {
        audio_element_deinit(i2s);
    }
    set_stream_alive_state(false, "tts pipeline stopped");
    return ok;
}

static void set_led_raw(bool on)
{
    int level = on ? 1 : 0;
    if (LED_ACTIVE_LOW) {
        level = on ? 0 : 1;
    }
    gpio_set_level(STATUS_LED_GPIO, level);
}

static void set_stream_alive_state(bool alive, const char *reason)
{
    g_led_state.stream_alive = alive;
    if (alive) {
        g_stream_retry_pending = false;
    }
    if (alive != g_last_logged_stream_alive) {
        if (reason && reason[0]) {
            ESP_LOGI(TAG, "Stream %s (%s)", alive ? "connected" : "disconnected", reason);
        } else {
            ESP_LOGI(TAG, "Stream %s", alive ? "connected" : "disconnected");
        }
        g_last_logged_stream_alive = alive;
    }
}

static void led_task(void *arg)
{
    (void)arg;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    bool blink_phase = false;
    while (1) {
        if (g_led_state.info_blink_active) {
            blink_phase = !blink_phase;
            set_led_raw(blink_phase);
            vTaskDelay(pdMS_TO_TICKS(110));
            continue;
        }

        if (g_led_state.edge_blink_count > 0) {
            set_led_raw(true);
            vTaskDelay(pdMS_TO_TICKS(70));
            set_led_raw(false);
            vTaskDelay(pdMS_TO_TICKS(70));
            g_led_state.edge_blink_count--;
            continue;
        }

        if (!g_led_state.stream_alive) {
            blink_phase = !blink_phase;
            set_led_raw(blink_phase);
            vTaskDelay(pdMS_TO_TICKS(300));
        } else if (g_led_state.stream_alive && !g_led_state.muted) {
            set_led_raw(true);
            vTaskDelay(pdMS_TO_TICKS(80));
        } else if (g_led_state.muted) {
            set_led_raw(false);
            vTaskDelay(pdMS_TO_TICKS(80));
        }
    }
}

static void init_key_gpio(key_desc_t *key)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << key->gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = key->has_internal_pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    key->last_level = gpio_get_level(key->gpio);
}

static void clear_info_button_feedback(void)
{
    g_led_state.info_blink_active = false;
}

static void url_decode(char *dst, const char *src)
{
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], 0 };
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
        } else if ((unsigned char)c < 32) {
            continue;
        } else {
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

static esp_err_t httpd_send_html_chunk(httpd_req_t *req, const char *html)
{
    if (!html) {
        return ESP_OK;
    }
    return httpd_resp_send_chunk(req, html, HTTPD_RESP_USE_STRLEN);
}

static bool has_dynamic_base_host(void)
{
    return g_base_host[0] != 0;
}

static void build_icecast_status_url(char *dst, size_t dst_size)
{
    size_t len = strnlen(g_base_host, sizeof(g_base_host));
    while (len > 0 && g_base_host[len - 1] == '/') {
        len--;
    }
    snprintf(dst, dst_size, "%.*s%s", (int)len, g_base_host, ICECAST_STATUS_PATH);
}

static bool build_dynamic_stream_url(int station_index, int mount_index, char *dst, size_t dst_size)
{
    if (station_index < 0 || station_index >= g_station_catalog_count) {
        return false;
    }
    station_catalog_t *station = &g_station_catalog[station_index];
    if (mount_index < 0 || mount_index >= station->mount_count) {
        return false;
    }
    size_t len = strnlen(g_base_host, sizeof(g_base_host));
    while (len > 0 && g_base_host[len - 1] == '/') {
        len--;
    }
    snprintf(dst, dst_size, "%.*s/%s/%s",
             (int)len, g_base_host,
             station->name,
             station->mounts[mount_index]);
    return true;
}

static void reset_catalog(void)
{
    memset(g_station_catalog, 0, sizeof(g_station_catalog));
    g_station_catalog_count = 0;
    g_catalog_available = false;
    g_current_station_index = 0;
    g_current_mount_index = 0;
}

static int find_station_index(const char *name)
{
    for (int i = 0; i < g_station_catalog_count; ++i) {
        if (strcmp(g_station_catalog[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_station_index_in_catalog(const station_catalog_t *catalog, int count, const char *name)
{
    if (!catalog || !name) {
        return -1;
    }
    for (int i = 0; i < count; ++i) {
        if (strcmp(catalog[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static bool station_has_mount(const station_catalog_t *station, const char *mount)
{
    if (!station || !mount) {
        return false;
    }
    for (int i = 0; i < station->mount_count; ++i) {
        if (strcmp(station->mounts[i], mount) == 0) {
            return true;
        }
    }
    return false;
}

static int find_mount_index_in_station(const station_catalog_t *station, const char *mount)
{
    if (!station || !mount) {
        return -1;
    }
    for (int i = 0; i < station->mount_count; ++i) {
        if (strcmp(station->mounts[i], mount) == 0) {
            return i;
        }
    }
    return -1;
}

static bool catalog_layout_equals(const station_catalog_t *a_catalog, int a_count, bool a_available,
                                  const station_catalog_t *b_catalog, int b_count, bool b_available)
{
    if (a_available != b_available) {
        return false;
    }
    if (!a_available && !b_available) {
        return true;
    }
    if (a_count != b_count) {
        return false;
    }
    for (int i = 0; i < a_count; ++i) {
        int b_index = find_station_index_in_catalog(b_catalog, b_count, a_catalog[i].name);
        if (b_index < 0) {
            return false;
        }
        const station_catalog_t *a_station = &a_catalog[i];
        const station_catalog_t *b_station = &b_catalog[b_index];
        if (a_station->mount_count != b_station->mount_count) {
            return false;
        }
        for (int j = 0; j < a_station->mount_count; ++j) {
            if (!station_has_mount(b_station, a_station->mounts[j])) {
                return false;
            }
        }
    }
    return true;
}

static void snapshot_current_catalog_state(void)
{
    memcpy(g_previous_station_catalog, g_station_catalog, sizeof(g_previous_station_catalog));
    g_previous_station_catalog_count = g_station_catalog_count;
    g_previous_catalog_available = g_catalog_available;
}

static bool parse_mount_number(const char *mount, int *number_out)
{
    if (!mount || !mount[0] || !number_out) {
        return false;
    }
    int value = 0;
    for (size_t i = 0; mount[i] != 0; ++i) {
        if (mount[i] < '0' || mount[i] > '9') {
            return false;
        }
        value = (value * 10) + (mount[i] - '0');
    }
    *number_out = value;
    return true;
}

static int preferred_mount_index_for_station(const station_catalog_t *station)
{
    if (!station || station->mount_count <= 0) {
        return 0;
    }

    for (int i = 0; i < station->mount_count; ++i) {
        if (strcmp(station->mounts[i], "comp") == 0) {
            return i;
        }
    }

    int best_index = 0;
    int best_number = 0;
    bool best_number_valid = false;

    for (int i = 0; i < station->mount_count; ++i) {
        int current_number = 0;
        if (!parse_mount_number(station->mounts[i], &current_number)) {
            continue;
        }
        if (!best_number_valid || current_number < best_number) {
            best_number = current_number;
            best_index = i;
            best_number_valid = true;
        }
    }

    return best_index;
}

static void copy_source_name_for_mount(station_catalog_t *station, const char *mount_name, const char *source_name)
{
    if (!station || !mount_name || !source_name || !source_name[0]) {
        return;
    }
    for (int i = 0; i < station->mount_count; ++i) {
        if (strcmp(station->mounts[i], mount_name) == 0) {
            strlcpy(station->source_names[i], source_name, sizeof(station->source_names[i]));
            return;
        }
    }
}

static bool add_catalog_entry(const char *station_name, const char *mount_name, const char *source_name)
{
    if (!station_name || !mount_name || !station_name[0] || !mount_name[0]) {
        return false;
    }
    int station_index = find_station_index(station_name);
    if (station_index < 0) {
        if (g_station_catalog_count >= MAX_STATIONS) {
            return false;
        }
        station_index = g_station_catalog_count++;
        strlcpy(g_station_catalog[station_index].name, station_name, sizeof(g_station_catalog[station_index].name));
    }
    station_catalog_t *station = &g_station_catalog[station_index];
    if (station_has_mount(station, mount_name)) {
        copy_source_name_for_mount(station, mount_name, source_name);
        return true;
    }
    if (station->mount_count >= MAX_MOUNTS_PER_STATION) {
        return false;
    }
    strlcpy(station->mounts[station->mount_count], mount_name, sizeof(station->mounts[station->mount_count]));
    if (source_name && source_name[0]) {
        strlcpy(station->source_names[station->mount_count], source_name, sizeof(station->source_names[station->mount_count]));
    }
    station->mount_count++;
    return true;
}

static bool parse_station_mount_from_path(const char *path, char *station_out, size_t station_out_size, char *mount_out, size_t mount_out_size)
{
    if (!path || path[0] == 0) {
        return false;
    }
    const char *p = strstr(path, "://");
    if (p) {
        p = strchr(p + 3, '/');
    } else {
        p = path;
    }
    if (!p || *p != '/') {
        return false;
    }
    p++;
    if (*p == 0) {
        return false;
    }
    const char *slash = strchr(p, '/');
    if (!slash || slash == p || slash[1] == 0) {
        return false;
    }
    const char *rest = slash + 1;
    if (strchr(rest, '/')) {
        return false;
    }
    size_t station_len = (size_t)(slash - p);
    size_t mount_len = strlen(rest);
    if (station_len >= station_out_size || mount_len >= mount_out_size) {
        return false;
    }
    memcpy(station_out, p, station_len);
    station_out[station_len] = 0;
    memcpy(mount_out, rest, mount_len + 1);
    return true;
}

static void log_catalog_summary(void)
{
    if (!g_catalog_available) {
        ESP_LOGW(TAG, "Icecast catalog not available");
        return;
    }
    for (int i = 0; i < g_station_catalog_count; ++i) {
        char mounts[192] = {0};
        for (int j = 0; j < g_station_catalog[i].mount_count; ++j) {
            if (j > 0) {
                strlcat(mounts, ",", sizeof(mounts));
            }
            strlcat(mounts, g_station_catalog[i].mounts[j], sizeof(mounts));
        }
        ESP_LOGI(TAG, "Catalog %s -> [%s]", g_station_catalog[i].name, mounts);
    }
}

static void build_catalog_summary_html(char *dst, size_t dst_size)
{
    size_t off = 0;
    dst[0] = 0;
    if (!has_dynamic_base_host()) {
        html_appendf(dst, dst_size, &off, "<div class='meta'>Host Icecast non configurato.</div>");
        return;
    }
    if (!g_catalog_available) {
        html_appendf(dst, dst_size, &off, "<div class='meta'>Catalogo Icecast non disponibile, verra' usato il fallback se impostato.</div>");
        return;
    }
    for (int i = 0; i < g_station_catalog_count; ++i) {
        html_appendf(dst, dst_size, &off, "<div class='meta'><strong>%s</strong>: ", g_station_catalog[i].name);
        for (int j = 0; j < g_station_catalog[i].mount_count; ++j) {
            html_appendf(dst, dst_size, &off, "%s%s", j ? ", " : "", g_station_catalog[i].mounts[j]);
        }
        html_appendf(dst, dst_size, &off, "</div>");
    }
}

static void url_encode_component(const char *src, char *dst, size_t dst_size)
{
    static const char *hex = "0123456789ABCDEF";
    size_t j = 0;
    if (!dst_size) {
        return;
    }
    for (size_t i = 0; src && src[i] != 0 && j + 1 < dst_size; ++i) {
        unsigned char c = (unsigned char)src[i];
        bool safe = (c >= 'a' && c <= 'z')
                 || (c >= 'A' && c <= 'Z')
                 || (c >= '0' && c <= '9')
                 || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            dst[j++] = (char)c;
        } else if (j + 3 < dst_size) {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0x0F];
        } else {
            break;
        }
    }
    dst[j] = 0;
}

static void url_encode_query_text(const char *src, char *dst, size_t dst_size)
{
    static const char *hex = "0123456789ABCDEF";
    size_t j = 0;
    if (!dst_size) {
        return;
    }
    for (size_t i = 0; src && src[i] != 0 && j + 1 < dst_size; ++i) {
        unsigned char c = (unsigned char)src[i];
        bool safe = (c >= 'a' && c <= 'z')
                 || (c >= 'A' && c <= 'Z')
                 || (c >= '0' && c <= '9')
                 || c == '-' || c == '_' || c == '.' || c == '~';
        if (c == ' ') {
            dst[j++] = '+';
        } else if (safe) {
            dst[j++] = (char)c;
        } else if (j + 3 < dst_size) {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0x0F];
        } else {
            break;
        }
    }
    dst[j] = 0;
}

static bool build_tts_request_url(char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0 || g_tts_url[0] == 0) {
        return false;
    }

    strlcpy(dst, g_tts_url, dst_size);

    const char *microfono_raw = NULL;
    const char *luogo_raw = NULL;

    if (g_stream_mode == STREAM_MODE_DYNAMIC
        && g_current_station_index >= 0
        && g_current_station_index < g_station_catalog_count) {
        station_catalog_t *station = &g_station_catalog[g_current_station_index];
        if (g_current_mount_index >= 0 && g_current_mount_index < station->mount_count) {
            microfono_raw = station->source_names[g_current_mount_index][0]
                ? station->source_names[g_current_mount_index]
                : station->mounts[g_current_mount_index];
            luogo_raw = station->name;
        }
    } else if (g_stream_mode == STREAM_MODE_FALLBACK) {
        const fallback_entry_t *entry = get_active_fallback_entry();
        if (entry) {
            microfono_raw = fallback_source_label(entry);
            luogo_raw = fallback_station_label(entry);
        }
    }

    if (!microfono_raw || !luogo_raw) {
        return true;
    }

    char microfono_enc[192];
    char luogo_enc[96];
    url_encode_component(microfono_raw, microfono_enc, sizeof(microfono_enc));
    url_encode_component(luogo_raw, luogo_enc, sizeof(luogo_enc));

    strlcat(dst, strchr(dst, '?') ? "&" : "?", dst_size);
    strlcat(dst, "microfono=", dst_size);
    strlcat(dst, microfono_enc, dst_size);
    strlcat(dst, "&luogo=", dst_size);
    strlcat(dst, luogo_enc, dst_size);
    return true;
}

static bool build_announce_request_url(const char *text, char *dst, size_t dst_size)
{
    char text_enc[192];

    if (!text || !text[0] || !dst || dst_size == 0 || g_announce_url[0] == 0) {
        return false;
    }

    url_encode_query_text(text, text_enc, sizeof(text_enc));
    strlcpy(dst, g_announce_url, dst_size);
    strlcat(dst, strchr(dst, '?') ? "&" : "?", dst_size);
    strlcat(dst, "testo=", dst_size);
    strlcat(dst, text_enc, dst_size);
    return true;
}

static bool build_startup_message_request_url(char *dst, size_t dst_size)
{
    char text_enc[256];

    if (!dst || dst_size == 0 || g_startup_message_text[0] == 0 || g_announce_url[0] == 0) {
        return false;
    }

    url_encode_query_text(g_startup_message_text, text_enc, sizeof(text_enc));
    strlcpy(dst, g_announce_url, dst_size);
    strlcat(dst, strchr(dst, '?') ? "&" : "?", dst_size);
    strlcat(dst, "testo=", dst_size);
    strlcat(dst, text_enc, dst_size);
    return true;
}

static const char *wifi_authmode_name(wifi_auth_mode_t authmode)
{
    switch (authmode) {
        case WIFI_AUTH_OPEN:
            return "aperta";
        case WIFI_AUTH_WEP:
            return "WEP";
        case WIFI_AUTH_WPA_PSK:
            return "WPA";
        case WIFI_AUTH_WPA2_PSK:
            return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE:
            return "WPA2-ENT";
        case WIFI_AUTH_WPA3_PSK:
            return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "WPA2/WPA3";
        default:
            return "protetta";
    }
}

static esp_err_t save_wifi_creds(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &nvs), TAG, "nvs_open failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_SSID, ssid), TAG, "save ssid failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_PASS, pass), TAG, "save pass failed");
    ESP_RETURN_ON_ERROR(nvs_commit(nvs), TAG, "nvs commit failed");
    nvs_close(nvs);
    return ESP_OK;
}

static void init_default_stream_urls(void)
{
    for (size_t i = 0; i < sizeof(kStreams) / sizeof(kStreams[0]); ++i) {
        strlcpy(kStreams[i].url, kStreams[i].default_url, sizeof(kStreams[i].url));
    }
    strlcpy(g_tts_url, DEFAULT_TTS_URL, sizeof(g_tts_url));
    strlcpy(g_announce_url, DEFAULT_ANNOUNCE_URL, sizeof(g_announce_url));
    g_announce_enabled = true;
    g_startup_message_text[0] = 0;
    strlcpy(g_base_host, DEFAULT_BASE_HOST, sizeof(g_base_host));
    memset(g_fallback_entries, 0, sizeof(g_fallback_entries));
    g_current_fallback_index = 0;
}

static bool fallback_entry_is_configured(const fallback_entry_t *entry)
{
    return entry && entry->url[0] != 0;
}

static int find_first_fallback_index(void)
{
    for (int i = 0; i < FALLBACK_ENTRY_COUNT; ++i) {
        if (fallback_entry_is_configured(&g_fallback_entries[i])) {
            return i;
        }
    }
    return -1;
}

static bool has_any_fallback_stream(void)
{
    return find_first_fallback_index() >= 0;
}

static const fallback_entry_t *get_active_fallback_entry(void)
{
    if (g_current_fallback_index < 0 || g_current_fallback_index >= FALLBACK_ENTRY_COUNT) {
        return NULL;
    }
    if (!fallback_entry_is_configured(&g_fallback_entries[g_current_fallback_index])) {
        return NULL;
    }
    return &g_fallback_entries[g_current_fallback_index];
}

static int next_fallback_index(int start_index)
{
    if (!has_any_fallback_stream()) {
        return -1;
    }
    for (int offset = 1; offset <= FALLBACK_ENTRY_COUNT; ++offset) {
        int idx = (start_index + offset + FALLBACK_ENTRY_COUNT) % FALLBACK_ENTRY_COUNT;
        if (fallback_entry_is_configured(&g_fallback_entries[idx])) {
            return idx;
        }
    }
    return find_first_fallback_index();
}

static const char *fallback_station_label(const fallback_entry_t *entry)
{
    return (entry && entry->station_name[0]) ? entry->station_name : "fallback";
}

static const char *fallback_source_label(const fallback_entry_t *entry)
{
    return (entry && entry->source_name[0]) ? entry->source_name : "emergenza";
}

static esp_err_t save_stream_urls(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &nvs), TAG, "nvs_open failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_STREAM1, kStreams[0].url), TAG, "save stream1 failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_STREAM2, kStreams[1].url), TAG, "save stream2 failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_STREAM3, kStreams[2].url), TAG, "save stream3 failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_TTS_URL, g_tts_url), TAG, "save tts url failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_ANN_URL, g_announce_url), TAG, "save announce url failed");
    ESP_RETURN_ON_ERROR(nvs_set_u8(nvs, NVS_KEY_ANN_EN, g_announce_enabled ? 1 : 0), TAG, "save announce enabled failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_STARTUP_TEXT, g_startup_message_text), TAG, "save startup text failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_BASE_HOST, g_base_host), TAG, "save base host failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_FB1_URL, g_fallback_entries[0].url), TAG, "save fallback1 url failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_FB1_ST, g_fallback_entries[0].station_name), TAG, "save fallback1 station failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_FB1_MC, g_fallback_entries[0].source_name), TAG, "save fallback1 mic failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_FB2_URL, g_fallback_entries[1].url), TAG, "save fallback2 url failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_FB2_ST, g_fallback_entries[1].station_name), TAG, "save fallback2 station failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_FB2_MC, g_fallback_entries[1].source_name), TAG, "save fallback2 mic failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_FB3_URL, g_fallback_entries[2].url), TAG, "save fallback3 url failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_FB3_ST, g_fallback_entries[2].station_name), TAG, "save fallback3 station failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, NVS_KEY_FB3_MC, g_fallback_entries[2].source_name), TAG, "save fallback3 mic failed");
    ESP_RETURN_ON_ERROR(nvs_commit(nvs), TAG, "nvs commit failed");
    nvs_close(nvs);
    return ESP_OK;
}

static bool load_wifi_creds(void)
{
    nvs_handle_t nvs;
    size_t ssid_len = sizeof(g_saved_ssid);
    size_t pass_len = sizeof(g_saved_pass);

    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    if (nvs_get_str(nvs, NVS_KEY_SSID, g_saved_ssid, &ssid_len) != ESP_OK) {
        nvs_close(nvs);
        return false;
    }
    if (nvs_get_str(nvs, NVS_KEY_PASS, g_saved_pass, &pass_len) != ESP_OK) {
        g_saved_pass[0] = 0;
    }
    nvs_close(nvs);
    return g_saved_ssid[0] != 0;
}

static void load_stream_urls(void)
{
    init_default_stream_urls();

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    size_t len1 = sizeof(kStreams[0].url);
    size_t len2 = sizeof(kStreams[1].url);
    size_t len3 = sizeof(kStreams[2].url);
    size_t tts_len = sizeof(g_tts_url);
    size_t ann_len = sizeof(g_announce_url);
    size_t startup_len = sizeof(g_startup_message_text);
    size_t base_host_len = sizeof(g_base_host);
    size_t fb1_url_len = sizeof(g_fallback_entries[0].url);
    size_t fb1_st_len = sizeof(g_fallback_entries[0].station_name);
    size_t fb1_mc_len = sizeof(g_fallback_entries[0].source_name);
    size_t fb2_url_len = sizeof(g_fallback_entries[1].url);
    size_t fb2_st_len = sizeof(g_fallback_entries[1].station_name);
    size_t fb2_mc_len = sizeof(g_fallback_entries[1].source_name);
    size_t fb3_url_len = sizeof(g_fallback_entries[2].url);
    size_t fb3_st_len = sizeof(g_fallback_entries[2].station_name);
    size_t fb3_mc_len = sizeof(g_fallback_entries[2].source_name);
    uint8_t ann_enabled = 1;

    if (nvs_get_str(nvs, NVS_KEY_STREAM1, kStreams[0].url, &len1) != ESP_OK) {
        strlcpy(kStreams[0].url, kStreams[0].default_url, sizeof(kStreams[0].url));
    }
    if (nvs_get_str(nvs, NVS_KEY_STREAM2, kStreams[1].url, &len2) != ESP_OK) {
        strlcpy(kStreams[1].url, kStreams[1].default_url, sizeof(kStreams[1].url));
    }
    if (nvs_get_str(nvs, NVS_KEY_STREAM3, kStreams[2].url, &len3) != ESP_OK) {
        strlcpy(kStreams[2].url, kStreams[2].default_url, sizeof(kStreams[2].url));
    }
    if (nvs_get_str(nvs, NVS_KEY_TTS_URL, g_tts_url, &tts_len) != ESP_OK) {
        strlcpy(g_tts_url, DEFAULT_TTS_URL, sizeof(g_tts_url));
    }
    if (nvs_get_str(nvs, NVS_KEY_ANN_URL, g_announce_url, &ann_len) != ESP_OK) {
        strlcpy(g_announce_url, DEFAULT_ANNOUNCE_URL, sizeof(g_announce_url));
    }
    if (nvs_get_u8(nvs, NVS_KEY_ANN_EN, &ann_enabled) != ESP_OK) {
        ann_enabled = 1;
    }
    g_announce_enabled = ann_enabled != 0;
    if (nvs_get_str(nvs, NVS_KEY_STARTUP_TEXT, g_startup_message_text, &startup_len) != ESP_OK) {
        strlcpy(g_startup_message_text, DEFAULT_STARTUP_TEXT, sizeof(g_startup_message_text));
    }
    if (nvs_get_str(nvs, NVS_KEY_BASE_HOST, g_base_host, &base_host_len) != ESP_OK) {
        strlcpy(g_base_host, DEFAULT_BASE_HOST, sizeof(g_base_host));
    }
    nvs_get_str(nvs, NVS_KEY_FB1_URL, g_fallback_entries[0].url, &fb1_url_len);
    nvs_get_str(nvs, NVS_KEY_FB1_ST, g_fallback_entries[0].station_name, &fb1_st_len);
    nvs_get_str(nvs, NVS_KEY_FB1_MC, g_fallback_entries[0].source_name, &fb1_mc_len);
    nvs_get_str(nvs, NVS_KEY_FB2_URL, g_fallback_entries[1].url, &fb2_url_len);
    nvs_get_str(nvs, NVS_KEY_FB2_ST, g_fallback_entries[1].station_name, &fb2_st_len);
    nvs_get_str(nvs, NVS_KEY_FB2_MC, g_fallback_entries[1].source_name, &fb2_mc_len);
    nvs_get_str(nvs, NVS_KEY_FB3_URL, g_fallback_entries[2].url, &fb3_url_len);
    nvs_get_str(nvs, NVS_KEY_FB3_ST, g_fallback_entries[2].station_name, &fb3_st_len);
    nvs_get_str(nvs, NVS_KEY_FB3_MC, g_fallback_entries[2].source_name, &fb3_mc_len);
    g_current_fallback_index = find_first_fallback_index();
    if (g_current_fallback_index < 0) {
        g_current_fallback_index = 0;
    }

    nvs_close(nvs);
}

static void update_active_stream_from_selection(void)
{
    g_active_stream_name[0] = 0;
    g_active_stream_url[0] = 0;

    if (g_stream_mode == STREAM_MODE_DYNAMIC) {
        if (!build_dynamic_stream_url(g_current_station_index, g_current_mount_index, g_active_stream_url, sizeof(g_active_stream_url))) {
            g_stream_mode = STREAM_MODE_FALLBACK;
        } else {
            snprintf(g_active_stream_name, sizeof(g_active_stream_name), "%s/%s",
                     g_station_catalog[g_current_station_index].name,
                     g_station_catalog[g_current_station_index].mounts[g_current_mount_index]);
            return;
        }
    }

    if (g_stream_mode == STREAM_MODE_FALLBACK) {
        const fallback_entry_t *entry = get_active_fallback_entry();
        if (entry) {
            strlcpy(g_active_stream_url, entry->url, sizeof(g_active_stream_url));
            snprintf(g_active_stream_name, sizeof(g_active_stream_name), "fallback %s/%s",
                     fallback_station_label(entry),
                     fallback_source_label(entry));
            return;
        }
        g_stream_mode = STREAM_MODE_PRESET;
    }

    if (g_selected_preset_index < 0 || g_selected_preset_index >= (int)(sizeof(kStreams) / sizeof(kStreams[0]))) {
        g_selected_preset_index = 0;
    }
    strlcpy(g_active_stream_url, kStreams[g_selected_preset_index].url, sizeof(g_active_stream_url));
    strlcpy(g_active_stream_name, kStreams[g_selected_preset_index].name, sizeof(g_active_stream_name));
}

static bool parse_icecast_sources_json(cJSON *source)
{
    if (!source) {
        return false;
    }
    bool found = false;
    if (cJSON_IsArray(source)) {
        int count = cJSON_GetArraySize(source);
        for (int i = 0; i < count; ++i) {
            cJSON *item = cJSON_GetArrayItem(source, i);
            cJSON *listenurl = cJSON_GetObjectItemCaseSensitive(item, "listenurl");
            cJSON *server_name = cJSON_GetObjectItemCaseSensitive(item, "server_name");
            if (!cJSON_IsString(listenurl) || !listenurl->valuestring) {
                continue;
            }
            char station[STATION_NAME_MAX_LEN];
            char mount[MOUNT_NAME_MAX_LEN];
            if (parse_station_mount_from_path(listenurl->valuestring, station, sizeof(station), mount, sizeof(mount))) {
                add_catalog_entry(station, mount,
                                  (cJSON_IsString(server_name) && server_name->valuestring) ? server_name->valuestring : mount);
                found = true;
            }
        }
    } else if (cJSON_IsObject(source)) {
        cJSON *listenurl = cJSON_GetObjectItemCaseSensitive(source, "listenurl");
        cJSON *server_name = cJSON_GetObjectItemCaseSensitive(source, "server_name");
        if (cJSON_IsString(listenurl) && listenurl->valuestring) {
            char station[STATION_NAME_MAX_LEN];
            char mount[MOUNT_NAME_MAX_LEN];
            if (parse_station_mount_from_path(listenurl->valuestring, station, sizeof(station), mount, sizeof(mount))) {
                add_catalog_entry(station, mount,
                                  (cJSON_IsString(server_name) && server_name->valuestring) ? server_name->valuestring : mount);
                found = true;
            }
        }
    }
    return found;
}

static bool refresh_stream_catalog(bool log_success)
{
    char desired_station[STATION_NAME_MAX_LEN] = {0};
    char desired_mount[MOUNT_NAME_MAX_LEN] = {0};
    snapshot_current_catalog_state();

    if (g_previous_catalog_available
        && g_last_dynamic_station_index >= 0
        && g_last_dynamic_station_index < g_previous_station_catalog_count) {
        const station_catalog_t *previous_station = &g_previous_station_catalog[g_last_dynamic_station_index];
        strlcpy(desired_station, previous_station->name, sizeof(desired_station));
        if (g_last_dynamic_mount_index >= 0 && g_last_dynamic_mount_index < previous_station->mount_count) {
            strlcpy(desired_mount, previous_station->mounts[g_last_dynamic_mount_index], sizeof(desired_mount));
        }
    }

    if (!g_wifi_connected || !has_dynamic_base_host()) {
        g_catalog_last_refresh_changed = g_previous_catalog_available;
        reset_catalog();
        return false;
    }

    char status_url[STREAM_URL_MAX_LEN + 32];
    build_icecast_status_url(status_url, sizeof(status_url));
    ESP_LOGI(TAG, "Refreshing Icecast catalog from %s", status_url);

    esp_http_client_config_t http_cfg = {
        .url = status_url,
        .timeout_ms = 8000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGW(TAG, "Cannot create Icecast status client");
        reset_catalog();
        return false;
    }

    char *json_buf = NULL;
    size_t json_len = 0;
    size_t json_cap = 8192;
    bool ok = false;
    bool catalog_changed = false;

    json_buf = malloc(json_cap);
    if (!json_buf) {
        goto cleanup;
    }

    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot open Icecast status endpoint");
        goto cleanup;
    }
    esp_http_client_fetch_headers(client);

    while (1) {
        int want = (int)(json_cap - json_len - 1);
        if (want < 1024) {
            size_t new_cap = json_cap * 2;
            char *grown = realloc(json_buf, new_cap);
            if (!grown) {
                ESP_LOGW(TAG, "Cannot grow Icecast status buffer");
                goto cleanup;
            }
            json_buf = grown;
            json_cap = new_cap;
            want = (int)(json_cap - json_len - 1);
        }
        int read_len = esp_http_client_read(client, json_buf + json_len, want);
        if (read_len < 0) {
            ESP_LOGW(TAG, "Icecast status read failed (%d)", read_len);
            goto cleanup;
        }
        if (read_len == 0) {
            break;
        }
        json_len += (size_t)read_len;
    }
    json_buf[json_len] = 0;

    cJSON *root = cJSON_Parse(json_buf);
    if (!root) {
        ESP_LOGW(TAG, "Icecast status JSON parse failed");
        goto cleanup;
    }
    cJSON *icestats = cJSON_GetObjectItemCaseSensitive(root, "icestats");
    cJSON *source = icestats ? cJSON_GetObjectItemCaseSensitive(icestats, "source") : NULL;
    reset_catalog();
    ok = parse_icecast_sources_json(source) && g_station_catalog_count > 0;
    g_catalog_available = ok;
    catalog_changed = !catalog_layout_equals(g_previous_station_catalog, g_previous_station_catalog_count, g_previous_catalog_available,
                                             g_station_catalog, g_station_catalog_count, g_catalog_available);
    g_catalog_last_refresh_changed = catalog_changed;
    if (ok) {
        reset_network_failure_streak("catalog ok");
        int desired_station_index = 0;
        if (desired_station[0]) {
            int found_station_index = find_station_index(desired_station);
            if (found_station_index >= 0) {
                desired_station_index = found_station_index;
            }
        }
        g_last_dynamic_station_index = desired_station_index;
        g_current_station_index = desired_station_index;
        station_catalog_t *station = &g_station_catalog[g_current_station_index];
        int desired_mount_index = -1;
        if (desired_mount[0]) {
            desired_mount_index = find_mount_index_in_station(station, desired_mount);
        }
        if (desired_mount_index < 0) {
            g_last_dynamic_mount_index = preferred_mount_index_for_station(station);
        } else {
            g_last_dynamic_mount_index = desired_mount_index;
        }
        g_current_mount_index = g_last_dynamic_mount_index;
        if (log_success) {
            log_catalog_summary();
        }
        if (catalog_changed) {
            ESP_LOGW(TAG, "Icecast catalog changed: station/mount layout updated");
        }
    } else {
        reset_catalog();
    }
    cJSON_Delete(root);

cleanup:
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(json_buf);
    if (!ok) {
        note_network_failure_and_maybe_reconnect("catalog refresh failed");
        g_catalog_last_refresh_changed = !catalog_layout_equals(g_previous_station_catalog, g_previous_station_catalog_count, g_previous_catalog_available,
                                                                g_station_catalog, g_station_catalog_count, g_catalog_available);
        ESP_LOGW(TAG, "Icecast catalog unavailable, fallback will be used");
    }
    return ok;
}

static void select_dynamic_stream_or_fallback(void)
{
    if (refresh_stream_catalog(true)) {
        g_stream_mode = STREAM_MODE_DYNAMIC;
        g_current_station_index = g_last_dynamic_station_index;
        station_catalog_t *station = &g_station_catalog[g_current_station_index];
        g_current_mount_index = preferred_mount_index_for_station(station);
        g_last_dynamic_mount_index = g_current_mount_index;
    } else if (has_any_fallback_stream()) {
        g_stream_mode = STREAM_MODE_FALLBACK;
        g_current_fallback_index = find_first_fallback_index();
    } else {
        g_stream_mode = STREAM_MODE_PRESET;
        g_selected_preset_index = 0;
    }
    update_active_stream_from_selection();
}

static void request_catalog_refresh(bool restart_stream)
{
    g_catalog_refresh_pending = true;
    if (restart_stream) {
        g_catalog_restart_requested = true;
    }
    if (g_catalog_task_handle || !g_wifi_connected) {
        return;
    }
    if (xTaskCreate(catalog_refresh_task, "catalog_refresh", 8192, NULL, 5, &g_catalog_task_handle) != pdPASS) {
        ESP_LOGW(TAG, "Unable to start catalog refresh task");
        g_catalog_task_handle = NULL;
    }
}

static void catalog_refresh_task(void *arg)
{
    (void)arg;
    bool restart_stream = g_catalog_restart_requested;
    bool catalog_changed = false;
    g_catalog_refresh_pending = false;
    g_catalog_restart_requested = false;

    select_dynamic_stream_or_fallback();
    catalog_changed = g_catalog_last_refresh_changed;

    if (g_wifi_connected && !g_tts_playing) {
        if ((restart_stream || catalog_changed) && g_pipeline_running) {
            restart_current_stream();
        } else if (!g_pipeline_running) {
            start_audio_pipeline();
        }
    }

    g_catalog_task_handle = NULL;
    vTaskDelete(NULL);
}

static void catalog_poll_task(void *arg)
{
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CATALOG_POLL_INTERVAL_MS));

        if (!g_wifi_connected || !has_dynamic_base_host()) {
            continue;
        }
        if (g_catalog_task_handle || g_tts_playing) {
            continue;
        }

        request_catalog_refresh(false);
    }
}

static void switch_station_next(void)
{
    if (g_stream_mode == STREAM_MODE_FALLBACK) {
        int next_index = next_fallback_index(g_current_fallback_index);
        if (next_index < 0) {
            ESP_LOGW(TAG, "Station button ignored: no fallback configured");
            return;
        }
        g_current_fallback_index = next_index;
        update_active_stream_from_selection();
        ESP_LOGI(TAG, "Switching fallback station to %s", g_active_stream_name);
        if (g_wifi_connected) {
            if (is_tts_busy()) {
                ESP_LOGI(TAG, "Fallback station changed during TTS, new stream will start after playback");
            } else if (!play_selection_announcement(true)) {
                restart_current_stream();
            }
        }
        return;
    }

    if (!g_catalog_available && !refresh_stream_catalog(true)) {
        toggle_fallback_stream();
        return;
    }
    if (g_station_catalog_count <= 0) {
        return;
    }
    g_current_station_index = (g_current_station_index + 1) % g_station_catalog_count;
    station_catalog_t *station = &g_station_catalog[g_current_station_index];
    g_current_mount_index = preferred_mount_index_for_station(station);
    g_last_dynamic_station_index = g_current_station_index;
    g_last_dynamic_mount_index = g_current_mount_index;
    update_active_stream_from_selection();
    ESP_LOGI(TAG, "Switching station to %s/%s",
             g_station_catalog[g_current_station_index].name,
             g_station_catalog[g_current_station_index].mounts[g_current_mount_index]);
    if (g_wifi_connected) {
        if (is_tts_busy()) {
            ESP_LOGI(TAG, "Station changed during TTS, new stream will start after playback");
        } else if (!play_selection_announcement(true)) {
            restart_current_stream();
        }
    }
}

static void switch_mount_next(void)
{
    if (g_stream_mode == STREAM_MODE_FALLBACK) {
        int next_index = next_fallback_index(g_current_fallback_index);
        if (next_index < 0) {
            ESP_LOGW(TAG, "Mount button ignored: no fallback configured");
            return;
        }
        g_current_fallback_index = next_index;
        update_active_stream_from_selection();
        ESP_LOGI(TAG, "Switching fallback mount to %s", g_active_stream_name);
        if (g_wifi_connected) {
            if (is_tts_busy()) {
                ESP_LOGI(TAG, "Fallback mount changed during TTS, new stream will start after playback");
            } else if (!play_selection_announcement(false)) {
                restart_current_stream();
            }
        }
        return;
    }

    if (!g_catalog_available && !refresh_stream_catalog(true)) {
        if (!has_any_fallback_stream()) {
            ESP_LOGW(TAG, "Mount button ignored: no catalog and no fallback");
            return;
        }
        g_stream_mode = STREAM_MODE_FALLBACK;
        g_current_fallback_index = find_first_fallback_index();
        update_active_stream_from_selection();
        if (g_wifi_connected) {
            restart_current_stream();
        }
        return;
    }
    station_catalog_t *station = &g_station_catalog[g_current_station_index];
    if (station->mount_count <= 0) {
        return;
    }
    g_current_mount_index = (g_current_mount_index + 1) % station->mount_count;
    g_last_dynamic_mount_index = g_current_mount_index;
    update_active_stream_from_selection();
    ESP_LOGI(TAG, "Switching mount to %s", g_active_stream_name);
    if (g_wifi_connected) {
        if (is_tts_busy()) {
            ESP_LOGI(TAG, "Mount changed during TTS, new stream will start after playback");
        } else if (!play_selection_announcement(false)) {
            restart_current_stream();
        }
    }
}

static void toggle_fallback_stream(void)
{
    if (g_stream_mode == STREAM_MODE_FALLBACK) {
        if (refresh_stream_catalog(true)) {
            g_stream_mode = STREAM_MODE_DYNAMIC;
            g_current_station_index = g_last_dynamic_station_index;
            g_current_mount_index = g_last_dynamic_mount_index;
        } else if (has_any_fallback_stream()) {
            g_stream_mode = STREAM_MODE_FALLBACK;
            g_current_fallback_index = find_first_fallback_index();
        } else {
            g_stream_mode = STREAM_MODE_PRESET;
        }
    } else {
        g_last_dynamic_station_index = g_current_station_index;
        g_last_dynamic_mount_index = g_current_mount_index;
        if (has_any_fallback_stream()) {
            g_stream_mode = STREAM_MODE_FALLBACK;
            if (!fallback_entry_is_configured(&g_fallback_entries[g_current_fallback_index])) {
                g_current_fallback_index = find_first_fallback_index();
            }
        } else if (!refresh_stream_catalog(false)) {
            g_stream_mode = STREAM_MODE_PRESET;
        } else {
            g_stream_mode = STREAM_MODE_DYNAMIC;
        }
    }
    update_active_stream_from_selection();
    ESP_LOGI(TAG, "Long press mount -> active stream %s", g_active_stream_name);
    if (g_wifi_connected) {
        if (g_stream_mode == STREAM_MODE_FALLBACK) {
            char fallback_url[TTS_REQUEST_URL_MAX_LEN];
            if (build_announce_request_url("fallback", fallback_url, sizeof(fallback_url))
                && start_tts_request_url("Fallback announcement", fallback_url)) {
                return;
            }
        }
        restart_current_stream();
    }
}

static void factory_reset_settings(void)
{
    ESP_LOGW(TAG, "Factory reset requested: erasing saved Wi-Fi credentials and stream URLs");

    clear_info_button_feedback();
    g_led_state.edge_blink_count = 5;
    vTaskDelay(pdMS_TO_TICKS(800));

    stop_audio_pipeline();
    stop_config_dns();

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        ESP_ERROR_CHECK(nvs_erase_all(nvs));
        ESP_ERROR_CHECK(nvs_commit(nvs));
        nvs_close(nvs);
    }

    g_saved_ssid[0] = 0;
    g_saved_pass[0] = 0;
    g_have_saved_creds = false;
    g_wifi_connected = false;
    g_force_ap_due_to_missing_network = false;
    g_current_stream = 0;
    g_selected_preset_index = 0;
    g_resume_stream_after_tts = 0;
    g_tts_playing = false;
    g_startup_announcement_pending = false;
    g_startup_announcement_played = false;
    init_default_stream_urls();
    reset_catalog();
    g_stream_mode = STREAM_MODE_PRESET;
    update_active_stream_from_selection();

    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_LOGW(TAG, "Factory reset complete, rebooting");
    esp_restart();
}

static esp_err_t networks_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Scanning nearby Wi-Fi networks for captive portal");
    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
    };
    uint16_t ap_count = 40;
    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_records) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    g_suspend_sta_connect_until = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    g_pending_sta_connect = false;
    if (!g_wifi_connected && g_have_saved_creds) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    esp_err_t scan_err = esp_wifi_scan_start(&scan_cfg, true);
    if (scan_err == ESP_OK) {
        esp_wifi_scan_get_ap_records(&ap_count, ap_records);
        ESP_LOGI(TAG, "Wi-Fi scan complete: %u network(s) found", ap_count);
    } else {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(scan_err));
        ap_count = 0;
    }

    if (!g_wifi_connected && g_have_saved_creds) {
        g_pending_sta_connect = true;
        g_pending_sta_connect_at = xTaskGetTickCount() + pdMS_TO_TICKS(1200);
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
            html_appendf(html, 8192, &off,
                "<button class='network' type='button' onclick='setSsid(\"%s\")'>%s"
                "<div class='meta'>RSSI %d dBm • canale %u • %s</div></button>",
                ssid_js, ssid_html, ap_records[i].rssi, ap_records[i].primary, wifi_authmode_name(ap_records[i].authmode));
        }
    }

    httpd_resp_set_type(req, "text/html");
    esp_err_t resp = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    free(ap_records);
    return resp;
}

static esp_err_t streams_get_handler(httpd_req_t *req)
{
    config_page_state_t *page = calloc(1, sizeof(*page));
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    const char *announce_checked = g_announce_enabled ? " checked" : "";

    html_escape(g_tts_url, page->tts_url_html, sizeof(page->tts_url_html));
    html_escape(g_announce_url, page->announce_url_html, sizeof(page->announce_url_html));
    html_escape(g_startup_message_text, page->startup_text_html, sizeof(page->startup_text_html));
    html_escape(g_base_host, page->base_host_html, sizeof(page->base_host_html));
    html_escape(g_fallback_entries[0].url, page->fb1_url_html, sizeof(page->fb1_url_html));
    html_escape(g_fallback_entries[0].station_name, page->fb1_station_html, sizeof(page->fb1_station_html));
    html_escape(g_fallback_entries[0].source_name, page->fb1_source_html, sizeof(page->fb1_source_html));
    html_escape(g_fallback_entries[1].url, page->fb2_url_html, sizeof(page->fb2_url_html));
    html_escape(g_fallback_entries[1].station_name, page->fb2_station_html, sizeof(page->fb2_station_html));
    html_escape(g_fallback_entries[1].source_name, page->fb2_source_html, sizeof(page->fb2_source_html));
    html_escape(g_fallback_entries[2].url, page->fb3_url_html, sizeof(page->fb3_url_html));
    html_escape(g_fallback_entries[2].station_name, page->fb3_station_html, sizeof(page->fb3_station_html));
    html_escape(g_fallback_entries[2].source_name, page->fb3_source_html, sizeof(page->fb3_source_html));
    html_escape(g_saved_ssid, page->ssid_html, sizeof(page->ssid_html));
    build_catalog_summary_html(page->catalog_html, sizeof(page->catalog_html));

    esp_netif_ip_info_t ip = {0};
    char ip_buf[32] = "non disponibile";
    if (g_sta_netif && esp_netif_get_ip_info(g_sta_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&ip.ip));
    }
    char *html = malloc(8192);
    if (!html) {
        free(page);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    size_t off = 0;

    httpd_resp_set_type(req, "text/html");

    html[0] = 0;
    html_appendf(html, 8192, &off,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>RadioSS113 Streams</title>"
        "<style>"
        "body{font-family:Arial,sans-serif;margin:20px;max-width:820px;background:#111;color:#f4f4f4}"
        "h1,h2{margin:0 0 12px 0}"
        ".card{background:#1b1b1b;padding:16px;border-radius:12px;margin-bottom:18px}"
        "input{width:100%%;font-size:15px;padding:10px;box-sizing:border-box;border-radius:8px;border:1px solid #444;background:#0f0f0f;color:#fff}"
        "button{font-size:16px;padding:12px 18px;margin-top:12px;border-radius:10px;border:0;background:#d83030;color:#fff}"
        ".meta{opacity:.75;font-size:14px;line-height:1.45}"
        "code{background:#202020;padding:2px 6px;border-radius:6px}"
        "</style></head><body>");
    if (httpd_send_html_chunk(req, html) != ESP_OK) {
        free(html);
        free(page);
        return ESP_FAIL;
    }

    off = 0;
    html[0] = 0;
    html_appendf(html, 8192, &off,
        "<h1>RadioSS113 Stream Control</h1>"
        "<div class='card'>"
        "<div class='meta'>Wi-Fi: <strong>%s</strong></div>"
        "<div class='meta'>IP: <code>%s</code></div>"
        "<div class='meta'>Stream attuale: <strong>%s</strong></div>"
        "<div class='meta'>Modalita': <strong>%s</strong></div>"
        "<div class='meta'>Volume: <strong>%d%%</strong> • Mute: <strong>%s</strong></div>"
        "</div>",
        g_saved_ssid[0] ? page->ssid_html : "(nessuno)",
        ip_buf,
        g_active_stream_name[0] ? g_active_stream_name : kStreams[g_current_stream].name,
        g_stream_mode == STREAM_MODE_DYNAMIC ? "catalogo Icecast" :
        (g_stream_mode == STREAM_MODE_FALLBACK ? "fallback" : "preset"),
        g_current_volume,
        g_is_muted ? "on" : "off");
    if (httpd_send_html_chunk(req, html) != ESP_OK) {
        free(html);
        free(page);
        return ESP_FAIL;
    }

    off = 0;
    html[0] = 0;
    html_appendf(html, 8192, &off,
        "<div class='card'><form method='post' action='/save_streams'>"
        "<h2>Catalogo Icecast</h2>"
        "<label>Base host stream<br><input name='base_host' maxlength='191' value='%s'></label><br><br>"
        "<h2>Fallback emergenza</h2>"
        "<div class='meta'>In modalita' fallback, i pulsanti stazione e microfono ruotano tra questi tre profili.</div><br>"
        "<label>Fallback 1 URL<br><input name='fb1_url' maxlength='191' value='%s'></label><br><br>"
        "<label>Fallback 1 stazione<br><input name='fb1_station' maxlength='31' value='%s'></label><br><br>"
        "<label>Fallback 1 microfono<br><input name='fb1_source' maxlength='63' value='%s'></label><br><br>"
        "<label>Fallback 2 URL<br><input name='fb2_url' maxlength='191' value='%s'></label><br><br>"
        "<label>Fallback 2 stazione<br><input name='fb2_station' maxlength='31' value='%s'></label><br><br>"
        "<label>Fallback 2 microfono<br><input name='fb2_source' maxlength='63' value='%s'></label><br><br>"
        "<label>Fallback 3 URL<br><input name='fb3_url' maxlength='191' value='%s'></label><br><br>"
        "<label>Fallback 3 stazione<br><input name='fb3_station' maxlength='31' value='%s'></label><br><br>"
        "<label>Fallback 3 microfono<br><input name='fb3_source' maxlength='63' value='%s'></label><br><br>"
        "%s"
        "<h2>Audio pulsante INFO</h2>"
        "<label>INFO / GPIO%d<br><input name='tts_url' maxlength='191' value='%s'></label><br><br>"
        "<h2>Voce all'avvio</h2>"
        "<label>Testo dopo aggancio Wi-Fi<br><input name='startup_text' maxlength='159' value='%s'></label><br><br>"
        "<h2>Annunci cambio stazione/microfono</h2>"
        "<label>Base URL annuncio<br><input name='announce_url' maxlength='191' value='%s'></label><br><br>"
        "<label><input type='checkbox' name='announce_enabled' value='1'%s> Abilita annuncio voce su cambio stazione e microfono</label><br><br>"
        "<button type='submit'>Salva stream</button>"
        "</form></div>"
        "</body></html>",
        page->base_host_html,
        page->fb1_url_html, page->fb1_station_html, page->fb1_source_html,
        page->fb2_url_html, page->fb2_station_html, page->fb2_source_html,
        page->fb3_url_html, page->fb3_station_html, page->fb3_source_html,
        page->catalog_html,
        (int)TTS_BUTTON_GPIO, page->tts_url_html,
        page->startup_text_html,
        page->announce_url_html, announce_checked);
    if (httpd_send_html_chunk(req, html) != ESP_OK) {
        free(html);
        free(page);
        return ESP_FAIL;
    }
    free(html);
    free(page);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t save_streams_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }
    char *body = malloc((size_t)total + 1);
    config_post_values_t *values = calloc(1, sizeof(*values));
    if (!body || !values) {
        free(body);
        free(values);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int offset = 0;
    while (offset < total) {
        int read = httpd_req_recv(req, body + offset, total - offset);
        if (read <= 0) {
            free(body);
            free(values);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Read failed");
            return ESP_FAIL;
        }
        offset += read;
    }
    body[offset] = 0;

    bool announce_enabled = false;
    bool should_restart_stream = false;

    form_get_value(body, "stream1", values->stream1, sizeof(values->stream1));
    form_get_value(body, "stream2", values->stream2, sizeof(values->stream2));
    form_get_value(body, "stream3", values->stream3, sizeof(values->stream3));
    form_get_value(body, "tts_url", values->tts_url, sizeof(values->tts_url));
    form_get_value(body, "announce_url", values->announce_url, sizeof(values->announce_url));
    form_get_value(body, "startup_text", values->startup_text, sizeof(values->startup_text));
    form_get_value(body, "base_host", values->base_host, sizeof(values->base_host));
    form_get_value(body, "fb1_url", values->fb1_url, sizeof(values->fb1_url));
    form_get_value(body, "fb1_station", values->fb1_station, sizeof(values->fb1_station));
    form_get_value(body, "fb1_source", values->fb1_source, sizeof(values->fb1_source));
    form_get_value(body, "fb2_url", values->fb2_url, sizeof(values->fb2_url));
    form_get_value(body, "fb2_station", values->fb2_station, sizeof(values->fb2_station));
    form_get_value(body, "fb2_source", values->fb2_source, sizeof(values->fb2_source));
    form_get_value(body, "fb3_url", values->fb3_url, sizeof(values->fb3_url));
    form_get_value(body, "fb3_station", values->fb3_station, sizeof(values->fb3_station));
    form_get_value(body, "fb3_source", values->fb3_source, sizeof(values->fb3_source));
    announce_enabled = strstr(body, "announce_enabled=1") != NULL;

    if (values->stream1[0]) {
        should_restart_stream = should_restart_stream || strcmp(kStreams[0].url, values->stream1) != 0;
        strlcpy(kStreams[0].url, values->stream1, sizeof(kStreams[0].url));
    }
    if (values->stream2[0]) {
        should_restart_stream = should_restart_stream || strcmp(kStreams[1].url, values->stream2) != 0;
        strlcpy(kStreams[1].url, values->stream2, sizeof(kStreams[1].url));
    }
    if (values->stream3[0]) {
        should_restart_stream = should_restart_stream || strcmp(kStreams[2].url, values->stream3) != 0;
        strlcpy(kStreams[2].url, values->stream3, sizeof(kStreams[2].url));
    }
    if (values->tts_url[0]) {
        strlcpy(g_tts_url, values->tts_url, sizeof(g_tts_url));
    }
    if (values->announce_url[0]) {
        strlcpy(g_announce_url, values->announce_url, sizeof(g_announce_url));
    }
    g_announce_enabled = announce_enabled;
    strlcpy(g_startup_message_text, values->startup_text, sizeof(g_startup_message_text));
    if (values->base_host[0]) {
        should_restart_stream = should_restart_stream || strcmp(g_base_host, values->base_host) != 0;
        strlcpy(g_base_host, values->base_host, sizeof(g_base_host));
    }
    should_restart_stream = should_restart_stream || strcmp(g_fallback_entries[0].url, values->fb1_url) != 0;
    should_restart_stream = should_restart_stream || strcmp(g_fallback_entries[1].url, values->fb2_url) != 0;
    should_restart_stream = should_restart_stream || strcmp(g_fallback_entries[2].url, values->fb3_url) != 0;
    strlcpy(g_fallback_entries[0].url, values->fb1_url, sizeof(g_fallback_entries[0].url));
    strlcpy(g_fallback_entries[0].station_name, values->fb1_station, sizeof(g_fallback_entries[0].station_name));
    strlcpy(g_fallback_entries[0].source_name, values->fb1_source, sizeof(g_fallback_entries[0].source_name));
    strlcpy(g_fallback_entries[1].url, values->fb2_url, sizeof(g_fallback_entries[1].url));
    strlcpy(g_fallback_entries[1].station_name, values->fb2_station, sizeof(g_fallback_entries[1].station_name));
    strlcpy(g_fallback_entries[1].source_name, values->fb2_source, sizeof(g_fallback_entries[1].source_name));
    strlcpy(g_fallback_entries[2].url, values->fb3_url, sizeof(g_fallback_entries[2].url));
    strlcpy(g_fallback_entries[2].station_name, values->fb3_station, sizeof(g_fallback_entries[2].station_name));
    strlcpy(g_fallback_entries[2].source_name, values->fb3_source, sizeof(g_fallback_entries[2].source_name));
    if (!fallback_entry_is_configured(&g_fallback_entries[g_current_fallback_index])) {
        int first_fallback = find_first_fallback_index();
        g_current_fallback_index = first_fallback >= 0 ? first_fallback : 0;
    }

    ESP_ERROR_CHECK(save_stream_urls());
    ESP_LOGI(TAG, "Stream URLs updated from web interface");
    ESP_LOGI(TAG, "KEY1 -> %s", kStreams[0].url);
    ESP_LOGI(TAG, "KEY2 -> %s", kStreams[1].url);
    ESP_LOGI(TAG, "KEY3 -> %s", kStreams[2].url);
    ESP_LOGI(TAG, "TTS -> %s", g_tts_url);
    ESP_LOGI(TAG, "STARTUP TEXT -> %s", g_startup_message_text[0] ? g_startup_message_text : "(none)");
    ESP_LOGI(TAG, "ANNOUNCE -> %s (%s)", g_announce_url, g_announce_enabled ? "enabled" : "disabled");
    ESP_LOGI(TAG, "BASE HOST -> %s", g_base_host);
    ESP_LOGI(TAG, "FALLBACK1 -> %s | %s | %s",
             g_fallback_entries[0].url[0] ? g_fallback_entries[0].url : "(none)",
             g_fallback_entries[0].station_name[0] ? g_fallback_entries[0].station_name : "(station)",
             g_fallback_entries[0].source_name[0] ? g_fallback_entries[0].source_name : "(mic)");
    ESP_LOGI(TAG, "FALLBACK2 -> %s | %s | %s",
             g_fallback_entries[1].url[0] ? g_fallback_entries[1].url : "(none)",
             g_fallback_entries[1].station_name[0] ? g_fallback_entries[1].station_name : "(station)",
             g_fallback_entries[1].source_name[0] ? g_fallback_entries[1].source_name : "(mic)");
    ESP_LOGI(TAG, "FALLBACK3 -> %s | %s | %s",
             g_fallback_entries[2].url[0] ? g_fallback_entries[2].url : "(none)",
             g_fallback_entries[2].station_name[0] ? g_fallback_entries[2].station_name : "(station)",
             g_fallback_entries[2].source_name[0] ? g_fallback_entries[2].source_name : "(mic)");

    if (g_wifi_connected) {
        request_catalog_refresh(should_restart_stream);
    } else {
        g_catalog_refresh_pending = true;
        update_active_stream_from_selection();
    }

    const char *resp =
        "<html><body><h1>Saved</h1>"
        "<p>Nuovi stream salvati in memoria.</p>"
        "<p>Se il player era attivo, lo stream corrente e' stato riavviato.</p>"
        "<p><a href=\"/streams\">Torna alla pagina stream</a></p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    free(body);
    free(values);
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static void apply_sta_config_and_connect(void)
{
    if (!g_have_saved_creds) {
        return;
    }
    if (xTaskGetTickCount() < g_suspend_sta_connect_until) {
        ESP_LOGI(TAG, "STA connect temporarily suspended during Wi-Fi scan");
        g_pending_sta_connect = true;
        g_pending_sta_connect_at = g_suspend_sta_connect_until;
        return;
    }

    wifi_mode_t target_mode = g_ap_active ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    ESP_ERROR_CHECK(esp_wifi_set_mode(target_mode));

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, g_saved_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, g_saved_pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_LOGI(TAG, "Connecting to saved SSID: %s (password %s)",
             g_saved_ssid,
             g_saved_pass[0] ? "set" : "empty");
    esp_wifi_connect();
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (!g_ap_active && g_wifi_connected) {
        return streams_get_handler(req);
    }

    config_page_state_t *page = calloc(1, sizeof(*page));
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    const char *announce_checked = g_announce_enabled ? " checked" : "";
    html_escape(g_saved_ssid, page->saved_ssid_html, sizeof(page->saved_ssid_html));
    html_escape(g_tts_url, page->tts_url_html, sizeof(page->tts_url_html));
    html_escape(g_announce_url, page->announce_url_html, sizeof(page->announce_url_html));
    html_escape(g_startup_message_text, page->startup_text_html, sizeof(page->startup_text_html));
    html_escape(g_base_host, page->base_host_html, sizeof(page->base_host_html));
    html_escape(g_fallback_entries[0].url, page->fb1_url_html, sizeof(page->fb1_url_html));
    html_escape(g_fallback_entries[0].station_name, page->fb1_station_html, sizeof(page->fb1_station_html));
    html_escape(g_fallback_entries[0].source_name, page->fb1_source_html, sizeof(page->fb1_source_html));
    html_escape(g_fallback_entries[1].url, page->fb2_url_html, sizeof(page->fb2_url_html));
    html_escape(g_fallback_entries[1].station_name, page->fb2_station_html, sizeof(page->fb2_station_html));
    html_escape(g_fallback_entries[1].source_name, page->fb2_source_html, sizeof(page->fb2_source_html));
    html_escape(g_fallback_entries[2].url, page->fb3_url_html, sizeof(page->fb3_url_html));
    html_escape(g_fallback_entries[2].station_name, page->fb3_station_html, sizeof(page->fb3_station_html));
    html_escape(g_fallback_entries[2].source_name, page->fb3_source_html, sizeof(page->fb3_source_html));
    build_catalog_summary_html(page->catalog_html, sizeof(page->catalog_html));

    char *html = malloc(8192);
    if (!html) {
        free(page);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    size_t off = 0;
    httpd_resp_set_type(req, "text/html");

    html[0] = 0;
    html_appendf(html, 8192, &off,
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
        ".meta{opacity:.75;font-size:14px}"
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
        "<div class='card'><div class='meta'>Rete di configurazione: <strong>%s</strong></div>"
        "<div class='meta'>SSID salvato: <strong>%s</strong></div></div>",
        AP_SSID, g_saved_ssid[0] ? page->saved_ssid_html : "(nessuno)");
    if (httpd_send_html_chunk(req, html) != ESP_OK) {
        free(html);
        free(page);
        return ESP_FAIL;
    }

    off = 0;
    html[0] = 0;
    html_appendf(html, 8192, &off,
        "<div class='card'><h2>Reti Wi-Fi trovate</h2><div id='networks'><div class='meta'>Scansione reti in corso...</div></div></div>");
    if (httpd_send_html_chunk(req, html) != ESP_OK) {
        free(html);
        free(page);
        return ESP_FAIL;
    }

    off = 0;
    html[0] = 0;
    html_appendf(html, 8192, &off,
        "<div class='card'><h2>Configura rete</h2>"
        "<form method='post' action='/save'>"
        "<label>SSID<br><input id='ssid' name='ssid' maxlength='32' value='%s'></label><br><br>"
        "<label>Password<br><input id='password' name='password' type='password' maxlength='64'></label><br><br>"
        "<h2>Catalogo Icecast</h2>"
        "<label>Base host stream<br><input name='base_host' maxlength='191' value='%s'></label><br><br>"
        "<h2>Fallback emergenza</h2>"
        "<div class='meta'>In modalita' fallback, i pulsanti stazione e microfono ruotano tra questi tre profili.</div><br>"
        "<label>Fallback 1 URL<br><input name='fb1_url' maxlength='191' value='%s'></label><br><br>"
        "<label>Fallback 1 stazione<br><input name='fb1_station' maxlength='31' value='%s'></label><br><br>"
        "<label>Fallback 1 microfono<br><input name='fb1_source' maxlength='63' value='%s'></label><br><br>"
        "<label>Fallback 2 URL<br><input name='fb2_url' maxlength='191' value='%s'></label><br><br>"
        "<label>Fallback 2 stazione<br><input name='fb2_station' maxlength='31' value='%s'></label><br><br>"
        "<label>Fallback 2 microfono<br><input name='fb2_source' maxlength='63' value='%s'></label><br><br>"
        "<label>Fallback 3 URL<br><input name='fb3_url' maxlength='191' value='%s'></label><br><br>"
        "<label>Fallback 3 stazione<br><input name='fb3_station' maxlength='31' value='%s'></label><br><br>"
        "<label>Fallback 3 microfono<br><input name='fb3_source' maxlength='63' value='%s'></label><br><br>"
        "%s"
        "<h2>Audio pulsante INFO</h2>"
        "<label>INFO / GPIO%d<br><input name='tts_url' maxlength='191' value='%s'></label><br><br>"
        "<h2>Voce all'avvio</h2>"
        "<label>Testo dopo aggancio Wi-Fi<br><input name='startup_text' maxlength='159' value='%s'></label><br><br>"
        "<h2>Annunci cambio stazione/microfono</h2>"
        "<label>Base URL annuncio<br><input name='announce_url' maxlength='191' value='%s'></label><br><br>"
        "<label><input type='checkbox' name='announce_enabled' value='1'%s> Abilita annuncio voce su cambio stazione e microfono</label><br><br>"
        "<button type='submit'>Salva e collega</button>"
        "</form></div></body></html>",
        g_saved_ssid[0] ? page->saved_ssid_html : "",
        page->base_host_html,
        page->fb1_url_html, page->fb1_station_html, page->fb1_source_html,
        page->fb2_url_html, page->fb2_station_html, page->fb2_source_html,
        page->fb3_url_html, page->fb3_station_html, page->fb3_source_html,
        page->catalog_html,
        (int)TTS_BUTTON_GPIO, page->tts_url_html,
        page->startup_text_html,
        page->announce_url_html, announce_checked);
    if (httpd_send_html_chunk(req, html) != ESP_OK) {
        free(html);
        free(page);
        return ESP_FAIL;
    }
    free(html);
    free(page);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t redirect_to_root_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    return root_get_handler(req);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    char *body = malloc((size_t)total + 1);
    config_post_values_t *values = calloc(1, sizeof(*values));
    if (!body || !values) {
        free(body);
        free(values);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int offset = 0;
    while (offset < total) {
        int read = httpd_req_recv(req, body + offset, total - offset);
        if (read <= 0) {
            free(body);
            free(values);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Read failed");
            return ESP_FAIL;
        }
        offset += read;
    }
    body[offset] = 0;

    char ssid[64] = {0};
    char pass[128] = {0};
    bool announce_enabled = false;
    bool should_restart_stream = false;
    if (!form_get_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == 0) {
        free(body);
        free(values);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID missing");
        return ESP_FAIL;
    }
    form_get_value(body, "password", pass, sizeof(pass));
    if (pass[0] == 0) {
        free(body);
        free(values);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password missing");
        return ESP_FAIL;
    }
    form_get_value(body, "stream1", values->stream1, sizeof(values->stream1));
    form_get_value(body, "stream2", values->stream2, sizeof(values->stream2));
    form_get_value(body, "stream3", values->stream3, sizeof(values->stream3));
    form_get_value(body, "tts_url", values->tts_url, sizeof(values->tts_url));
    form_get_value(body, "announce_url", values->announce_url, sizeof(values->announce_url));
    form_get_value(body, "startup_text", values->startup_text, sizeof(values->startup_text));
    form_get_value(body, "base_host", values->base_host, sizeof(values->base_host));
    form_get_value(body, "fb1_url", values->fb1_url, sizeof(values->fb1_url));
    form_get_value(body, "fb1_station", values->fb1_station, sizeof(values->fb1_station));
    form_get_value(body, "fb1_source", values->fb1_source, sizeof(values->fb1_source));
    form_get_value(body, "fb2_url", values->fb2_url, sizeof(values->fb2_url));
    form_get_value(body, "fb2_station", values->fb2_station, sizeof(values->fb2_station));
    form_get_value(body, "fb2_source", values->fb2_source, sizeof(values->fb2_source));
    form_get_value(body, "fb3_url", values->fb3_url, sizeof(values->fb3_url));
    form_get_value(body, "fb3_station", values->fb3_station, sizeof(values->fb3_station));
    form_get_value(body, "fb3_source", values->fb3_source, sizeof(values->fb3_source));
    announce_enabled = strstr(body, "announce_enabled=1") != NULL;

    if (values->stream1[0]) {
        should_restart_stream = should_restart_stream || strcmp(kStreams[0].url, values->stream1) != 0;
        strlcpy(kStreams[0].url, values->stream1, sizeof(kStreams[0].url));
    }
    if (values->stream2[0]) {
        should_restart_stream = should_restart_stream || strcmp(kStreams[1].url, values->stream2) != 0;
        strlcpy(kStreams[1].url, values->stream2, sizeof(kStreams[1].url));
    }
    if (values->stream3[0]) {
        should_restart_stream = should_restart_stream || strcmp(kStreams[2].url, values->stream3) != 0;
        strlcpy(kStreams[2].url, values->stream3, sizeof(kStreams[2].url));
    }
    if (values->tts_url[0]) {
        strlcpy(g_tts_url, values->tts_url, sizeof(g_tts_url));
    }
    if (values->announce_url[0]) {
        strlcpy(g_announce_url, values->announce_url, sizeof(g_announce_url));
    }
    g_announce_enabled = announce_enabled;
    strlcpy(g_startup_message_text, values->startup_text, sizeof(g_startup_message_text));
    if (values->base_host[0]) {
        should_restart_stream = should_restart_stream || strcmp(g_base_host, values->base_host) != 0;
        strlcpy(g_base_host, values->base_host, sizeof(g_base_host));
    }
    should_restart_stream = should_restart_stream || strcmp(g_fallback_entries[0].url, values->fb1_url) != 0;
    should_restart_stream = should_restart_stream || strcmp(g_fallback_entries[1].url, values->fb2_url) != 0;
    should_restart_stream = should_restart_stream || strcmp(g_fallback_entries[2].url, values->fb3_url) != 0;
    strlcpy(g_fallback_entries[0].url, values->fb1_url, sizeof(g_fallback_entries[0].url));
    strlcpy(g_fallback_entries[0].station_name, values->fb1_station, sizeof(g_fallback_entries[0].station_name));
    strlcpy(g_fallback_entries[0].source_name, values->fb1_source, sizeof(g_fallback_entries[0].source_name));
    strlcpy(g_fallback_entries[1].url, values->fb2_url, sizeof(g_fallback_entries[1].url));
    strlcpy(g_fallback_entries[1].station_name, values->fb2_station, sizeof(g_fallback_entries[1].station_name));
    strlcpy(g_fallback_entries[1].source_name, values->fb2_source, sizeof(g_fallback_entries[1].source_name));
    strlcpy(g_fallback_entries[2].url, values->fb3_url, sizeof(g_fallback_entries[2].url));
    strlcpy(g_fallback_entries[2].station_name, values->fb3_station, sizeof(g_fallback_entries[2].station_name));
    strlcpy(g_fallback_entries[2].source_name, values->fb3_source, sizeof(g_fallback_entries[2].source_name));
    if (!fallback_entry_is_configured(&g_fallback_entries[g_current_fallback_index])) {
        int first_fallback = find_first_fallback_index();
        g_current_fallback_index = first_fallback >= 0 ? first_fallback : 0;
    }

    ESP_ERROR_CHECK(save_wifi_creds(ssid, pass));
    ESP_ERROR_CHECK(save_stream_urls());
    strlcpy(g_saved_ssid, ssid, sizeof(g_saved_ssid));
    strlcpy(g_saved_pass, pass, sizeof(g_saved_pass));
    g_have_saved_creds = true;
    g_last_disconnect_tick = xTaskGetTickCount();

    ESP_LOGI(TAG, "Saved new Wi-Fi credentials for SSID: %s", g_saved_ssid);
    ESP_LOGI(TAG, "Saved stream URLs from captive portal");
    ESP_LOGI(TAG, "KEY1 -> %s", kStreams[0].url);
    ESP_LOGI(TAG, "KEY2 -> %s", kStreams[1].url);
    ESP_LOGI(TAG, "KEY3 -> %s", kStreams[2].url);
    ESP_LOGI(TAG, "TTS -> %s", g_tts_url);
    ESP_LOGI(TAG, "STARTUP TEXT -> %s", g_startup_message_text[0] ? g_startup_message_text : "(none)");
    ESP_LOGI(TAG, "ANNOUNCE -> %s (%s)", g_announce_url, g_announce_enabled ? "enabled" : "disabled");
    ESP_LOGI(TAG, "BASE HOST -> %s", g_base_host);
    ESP_LOGI(TAG, "FALLBACK1 -> %s | %s | %s",
             g_fallback_entries[0].url[0] ? g_fallback_entries[0].url : "(none)",
             g_fallback_entries[0].station_name[0] ? g_fallback_entries[0].station_name : "(station)",
             g_fallback_entries[0].source_name[0] ? g_fallback_entries[0].source_name : "(mic)");
    ESP_LOGI(TAG, "FALLBACK2 -> %s | %s | %s",
             g_fallback_entries[1].url[0] ? g_fallback_entries[1].url : "(none)",
             g_fallback_entries[1].station_name[0] ? g_fallback_entries[1].station_name : "(station)",
             g_fallback_entries[1].source_name[0] ? g_fallback_entries[1].source_name : "(mic)");
    ESP_LOGI(TAG, "FALLBACK3 -> %s | %s | %s",
             g_fallback_entries[2].url[0] ? g_fallback_entries[2].url : "(none)",
             g_fallback_entries[2].station_name[0] ? g_fallback_entries[2].station_name : "(station)",
             g_fallback_entries[2].source_name[0] ? g_fallback_entries[2].source_name : "(mic)");
    if (g_wifi_connected) {
        request_catalog_refresh(should_restart_stream);
    } else {
        g_catalog_refresh_pending = true;
        update_active_stream_from_selection();
    }

    g_pending_sta_connect = true;
    g_pending_sta_connect_at = xTaskGetTickCount() + pdMS_TO_TICKS(1800);

    const char *resp =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>RadioSS113 Setup</title>"
        "<style>"
        "body{font-family:Arial,sans-serif;background:#111;color:#f4f4f4;margin:24px}"
        ".card{max-width:520px;background:#1b1b1b;padding:18px;border-radius:12px}"
        "h1{margin-top:0}"
        ".meta{opacity:.8}"
        "</style></head><body>"
        "<div class='card'>"
        "<h1>Impostazioni salvate</h1>"
        "<p>Wi-Fi e stream sono stati salvati in memoria.</p>"
        "<p class='meta'>La board prova ora a collegarsi alla rete configurata.</p>"
        "</div></body></html>";
    httpd_resp_set_type(req, "text/html");
    free(body);
    free(values);
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static void start_config_server(void)
{
    if (g_http_server) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 16;
    config.stack_size = 12288;
    config.max_resp_headers = 12;
    if (httpd_start(&g_http_server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Config portal HTTP server started");
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL,
        };
        httpd_uri_t save = {
            .uri = "/save",
            .method = HTTP_POST,
            .handler = save_post_handler,
            .user_ctx = NULL,
        };
        httpd_uri_t wifi = {
            .uri = "/wifi",
            .method = HTTP_GET,
            .handler = wifi_get_handler,
            .user_ctx = NULL,
        };
        httpd_uri_t networks = {
            .uri = "/networks",
            .method = HTTP_GET,
            .handler = networks_get_handler,
            .user_ctx = NULL,
        };
        httpd_uri_t streams = {
            .uri = "/streams",
            .method = HTTP_GET,
            .handler = streams_get_handler,
            .user_ctx = NULL,
        };
        httpd_uri_t save_streams = {
            .uri = "/save_streams",
            .method = HTTP_POST,
            .handler = save_streams_post_handler,
            .user_ctx = NULL,
        };
        httpd_uri_t android_204 = {
            .uri = "/generate_204",
            .method = HTTP_GET,
            .handler = redirect_to_root_handler,
            .user_ctx = NULL,
        };
        httpd_uri_t android_204_nounderscore = {
            .uri = "/generate204",
            .method = HTTP_GET,
            .handler = redirect_to_root_handler,
            .user_ctx = NULL,
        };
        httpd_uri_t hotspot_detect = {
            .uri = "/hotspot-detect.html",
            .method = HTTP_GET,
            .handler = redirect_to_root_handler,
            .user_ctx = NULL,
        };
        httpd_uri_t connect_test = {
            .uri = "/connecttest.txt",
            .method = HTTP_GET,
            .handler = redirect_to_root_handler,
            .user_ctx = NULL,
        };
        httpd_uri_t ncsi = {
            .uri = "/ncsi.txt",
            .method = HTTP_GET,
            .handler = redirect_to_root_handler,
            .user_ctx = NULL,
        };
        httpd_uri_t favicon = {
            .uri = "/favicon.ico",
            .method = HTTP_GET,
            .handler = redirect_to_root_handler,
            .user_ctx = NULL,
        };
        httpd_uri_t wildcard = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = redirect_to_root_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(g_http_server, &root);
        httpd_register_uri_handler(g_http_server, &wifi);
        httpd_register_uri_handler(g_http_server, &networks);
        httpd_register_uri_handler(g_http_server, &save);
        httpd_register_uri_handler(g_http_server, &streams);
        httpd_register_uri_handler(g_http_server, &save_streams);
        httpd_register_uri_handler(g_http_server, &android_204);
        httpd_register_uri_handler(g_http_server, &android_204_nounderscore);
        httpd_register_uri_handler(g_http_server, &hotspot_detect);
        httpd_register_uri_handler(g_http_server, &connect_test);
        httpd_register_uri_handler(g_http_server, &ncsi);
        httpd_register_uri_handler(g_http_server, &favicon);
        httpd_register_uri_handler(g_http_server, &wildcard);
    } else {
        ESP_LOGE(TAG, "Failed to start config portal HTTP server");
    }
}

static void stop_config_server(void)
{
    if (g_http_server) {
        ESP_LOGI(TAG, "Stopping config portal HTTP server");
        httpd_stop(g_http_server);
        g_http_server = NULL;
    }
}

static void start_config_dns(void)
{
    if (g_dns_server) {
        return;
    }
    dns_server_config_t config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    g_dns_server = start_dns_server(&config);
    if (g_dns_server) {
        ESP_LOGI(TAG, "Captive portal DNS redirect started");
    } else {
        ESP_LOGE(TAG, "Failed to start captive portal DNS redirect");
    }
}

static void stop_config_dns(void)
{
    if (g_dns_server) {
        ESP_LOGI(TAG, "Stopping captive portal DNS redirect");
        stop_dns_server(g_dns_server);
        g_dns_server = NULL;
    }
}

static void start_config_ap(void)
{
    if (g_ap_active) {
        return;
    }

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(AP_SSID);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_LOGW(TAG, "Starting config AP: %s", AP_SSID);
    // Keep STA enabled even during setup so Wi-Fi scans and background reconnects work.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    if (!g_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        g_wifi_started = true;
    }
    esp_netif_ip_info_t ap_ip = {0};
    if (esp_netif_get_ip_info(g_ap_netif, &ap_ip) == ESP_OK) {
        snprintf(g_captive_uri, sizeof(g_captive_uri), "http://" IPSTR "/", IP2STR(&ap_ip.ip));
        esp_netif_dhcps_option(
            g_ap_netif,
            ESP_NETIF_OP_SET,
            ESP_NETIF_CAPTIVEPORTAL_URI,
            g_captive_uri,
            strlen(g_captive_uri) + 1
        );
        ESP_LOGI(TAG, "Captive portal URI: %s", g_captive_uri);
    }
    start_config_server();
    start_config_dns();
    g_ap_active = true;
}

static void stop_config_ap(void)
{
    if (!g_ap_active) {
        return;
    }
    g_ap_active = false;
    ESP_LOGI(TAG, "Stopping config AP");
    stop_config_dns();
    if (g_have_saved_creds) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }
}

static void start_audio_url(const char *name, const char *url)
{
    if (g_pipeline_running || !g_wifi_connected) {
        return;
    }
    ESP_LOGI(TAG, "Starting stream: %s -> %s", name, url);
    set_stream_alive_state(false, "pipeline starting");
    g_decoder_locked = false;
    if (!g_is_muted) {
        ESP_ERROR_CHECK(audio_hal_set_mute(g_board_handle->audio_hal, true));
        g_transition_mute_active = true;
    }
    audio_element_set_uri(g_http_stream_reader, url);
    audio_pipeline_reset_ringbuffer(g_pipeline);
    audio_pipeline_reset_elements(g_pipeline);
    audio_pipeline_change_state(g_pipeline, AEL_STATE_INIT);
    audio_pipeline_run(g_pipeline);
    g_pipeline_running = true;
}

static void start_audio_pipeline(void)
{
    update_active_stream_from_selection();
    start_audio_url(g_active_stream_name, g_active_stream_url);
}

static void stop_audio_pipeline(void)
{
    if (!g_pipeline_running) {
        return;
    }
    ESP_LOGI(TAG, "Stopping current audio pipeline");
    audio_pipeline_stop(g_pipeline);
    audio_pipeline_wait_for_stop(g_pipeline);
    audio_pipeline_reset_ringbuffer(g_pipeline);
    audio_pipeline_reset_elements(g_pipeline);
    audio_pipeline_change_state(g_pipeline, AEL_STATE_INIT);
    g_pipeline_running = false;
    g_decoder_locked = false;
    if (g_transition_mute_active && !g_is_muted) {
        ESP_ERROR_CHECK(audio_hal_set_mute(g_board_handle->audio_hal, false));
        g_transition_mute_active = false;
    }
    set_stream_alive_state(false, "pipeline stopped");
}

static void restart_current_stream(void)
{
    if (g_stream_restart_in_progress) {
        ESP_LOGI(TAG, "Stream restart already in progress, skipping duplicate request");
        return;
    }
    if (g_tts_takeover_active) {
        ESP_LOGI(TAG, "Ignoring stream restart while TTS takeover is active");
        return;
    }
    if (g_tts_task_handle) {
        g_tts_stop_requested = true;
        ESP_LOGI(TAG, "Delaying stream restart until TTS task stops");
        return;
    }
    update_active_stream_from_selection();
    ESP_LOGW(TAG, "Restarting current stream: %s", g_active_stream_name);
    g_stream_restart_in_progress = true;
    g_tts_playing = false;
    g_tts_started = false;
    g_stream_retry_pending = false;
    g_ignore_restart_until = xTaskGetTickCount() + pdMS_TO_TICKS(RESTART_GUARD_MS);
    stop_audio_pipeline();
    start_audio_pipeline();
    g_stream_restart_in_progress = false;
}

static void schedule_stream_retry(uint32_t delay_ms, const char *reason)
{
    if (!g_wifi_connected || g_tts_playing) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    TickType_t due = now + pdMS_TO_TICKS(delay_ms);

    if (g_ignore_restart_until > due) {
        due = g_ignore_restart_until;
    }

    if (!g_stream_retry_pending || due < g_stream_retry_at) {
        g_stream_retry_pending = true;
        g_stream_retry_at = due;
        if (reason && reason[0]) {
            ESP_LOGW(TAG, "Scheduling stream retry in %u ms (%s)",
                     (unsigned)pdTICKS_TO_MS(due - now),
                     reason);
        } else {
            ESP_LOGW(TAG, "Scheduling stream retry in %u ms",
                     (unsigned)pdTICKS_TO_MS(due - now));
        }
    }
}

static void reset_network_failure_streak(const char *reason)
{
    if (g_network_failure_streak > 0) {
        if (reason && reason[0]) {
            ESP_LOGI(TAG, "Network failure streak reset (%s)", reason);
        } else {
            ESP_LOGI(TAG, "Network failure streak reset");
        }
    }
    g_network_failure_streak = 0;
}

static void note_network_failure_and_maybe_reconnect(const char *reason)
{
    if (!g_wifi_connected || !g_have_saved_creds || g_tts_playing) {
        return;
    }

    g_network_failure_streak++;
    if (reason && reason[0]) {
        ESP_LOGW(TAG, "Network failure streak: %d (%s)", g_network_failure_streak, reason);
    } else {
        ESP_LOGW(TAG, "Network failure streak: %d", g_network_failure_streak);
    }

    TickType_t now = xTaskGetTickCount();
    if (g_network_failure_streak < NETWORK_FAILURE_RECONNECT_THRESHOLD) {
        return;
    }
    if ((now - g_last_forced_wifi_reconnect_at) < pdMS_TO_TICKS(NETWORK_FAILURE_RECONNECT_COOLDOWN_MS)) {
        return;
    }

    ESP_LOGW(TAG, "Forcing Wi-Fi reconnect after %d consecutive network failures",
             g_network_failure_streak);
    g_last_forced_wifi_reconnect_at = now;
    g_network_failure_streak = 0;
    g_wifi_connected = false;
    xEventGroupClearBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
    g_last_disconnect_tick = now;
    set_stream_alive_state(false, "forcing Wi-Fi reconnect");
    esp_wifi_disconnect();
    if (xTaskGetTickCount() >= g_suspend_sta_connect_until) {
        esp_wifi_connect();
    }
}

static void play_tts_url(void)
{
    char tts_request_url[TTS_REQUEST_URL_MAX_LEN];

    if (!g_wifi_connected) {
        ESP_LOGW(TAG, "TTS button pressed but Wi-Fi is not connected");
        clear_info_button_feedback();
        return;
    }
    if (g_tts_url[0] == 0) {
        ESP_LOGW(TAG, "TTS button pressed but URL is empty");
        clear_info_button_feedback();
        return;
    }
    if (!build_tts_request_url(tts_request_url, sizeof(tts_request_url))) {
        ESP_LOGW(TAG, "TTS button pressed but request URL could not be built");
        clear_info_button_feedback();
        return;
    }
    if (!start_tts_request_url("TTS button pressed", tts_request_url)) {
        clear_info_button_feedback();
    }
}

static bool start_tts_request_url(const char *reason, const char *url)
{
    if (!url || !url[0]) {
        return false;
    }
    if (is_tts_busy()) {
        ESP_LOGI(TAG, "TTS already in progress");
        return false;
    }

    ESP_LOGI(TAG, "%s -> %s", reason ? reason : "TTS request", url);
    g_resume_stream_after_tts = g_current_stream;
    g_tts_playing = true;
    g_tts_started = false;
    g_tts_takeover_active = false;
    g_stream_retry_pending = false;
    g_ignore_restart_until = xTaskGetTickCount() + pdMS_TO_TICKS(RESTART_GUARD_MS);

    if (is_probably_wav_url(url)) {
        char *url_copy = strdup(url);
        if (url_copy) {
            g_tts_stop_requested = false;
            if (xTaskCreate(tts_wav_task, "tts_wav", 12288, url_copy, 5, &g_tts_task_handle) == pdPASS) {
                return true;
            }
            free(url_copy);
        }
        g_tts_task_handle = NULL;
        ESP_LOGW(TAG, "Unable to start WAV prebuffer task, using standard TTS path");
    } else {
        char *url_copy = strdup(url);
        if (url_copy) {
            g_tts_stop_requested = false;
            if (xTaskCreate(tts_live_task, "tts_live", 12288, url_copy, 5, &g_tts_task_handle) == pdPASS) {
                return true;
            }
            free(url_copy);
        }
        g_tts_task_handle = NULL;
        ESP_LOGW(TAG, "Unable to start MP3 live task, using standard TTS path");
    }

    stop_audio_pipeline();
    start_audio_url("tts", url);
    return true;
}

static bool play_selection_announcement(bool station_change)
{
    char announce_request_url[TTS_REQUEST_URL_MAX_LEN];
    char station_announcement_text[128];
    const char *text = NULL;

    if (!g_announce_enabled || !g_wifi_connected || g_announce_url[0] == 0) {
        return false;
    }
    if (g_stream_mode == STREAM_MODE_DYNAMIC
        && g_current_station_index >= 0
        && g_current_station_index < g_station_catalog_count) {
        station_catalog_t *station = &g_station_catalog[g_current_station_index];
        if (station_change) {
            snprintf(station_announcement_text, sizeof(station_announcement_text), "stazione di %s", station->name);
            text = station_announcement_text;
        } else if (g_current_mount_index >= 0 && g_current_mount_index < station->mount_count) {
            text = station->source_names[g_current_mount_index][0]
                ? station->source_names[g_current_mount_index]
                : station->mounts[g_current_mount_index];
        }
    } else if (g_stream_mode == STREAM_MODE_FALLBACK) {
        const fallback_entry_t *entry = get_active_fallback_entry();
        if (!entry) {
            return false;
        }
        if (station_change) {
            snprintf(station_announcement_text, sizeof(station_announcement_text), "stazione di %s",
                     fallback_station_label(entry));
            text = station_announcement_text;
        } else {
            text = fallback_source_label(entry);
        }
    }

    if (!text || !text[0]) {
        return false;
    }
    if (!build_announce_request_url(text, announce_request_url, sizeof(announce_request_url))) {
        return false;
    }
    return start_tts_request_url(station_change ? "Station announcement" : "Mount announcement",
                                 announce_request_url);
}

static bool play_startup_message(void)
{
    char startup_request_url[TTS_REQUEST_URL_MAX_LEN];

    if (!g_wifi_connected || g_startup_message_text[0] == 0) {
        return false;
    }
    if (!build_startup_message_request_url(startup_request_url, sizeof(startup_request_url))) {
        return false;
    }
    if (!start_tts_request_url("Startup message", startup_request_url)) {
        return false;
    }
    g_startup_announcement_pending = false;
    g_startup_announcement_played = true;
    return true;
}

static void finish_tts_playback(const char *reason)
{
    if (!g_tts_playing) {
        return;
    }

    ESP_LOGI(TAG, "TTS playback finished (%s), returning to stream: %s",
             reason ? reason : "done",
             g_active_stream_name[0] ? g_active_stream_name : kStreams[g_resume_stream_after_tts].name);
    g_tts_playing = false;
    g_tts_started = false;
    g_tts_takeover_active = false;
    clear_info_button_feedback();
    g_current_stream = g_resume_stream_after_tts;
    g_ignore_restart_until = xTaskGetTickCount() + pdMS_TO_TICKS(RESTART_GUARD_MS);
    stop_audio_pipeline();
    clear_tts_memory_stream();
    start_audio_pipeline();
}

static void tts_wav_task(void *arg)
{
    static const int HEADER_BUF_SIZE = 4096;
    static const int IO_BUF_SIZE = 4096;
    char *url = (char *)arg;
    esp_http_client_handle_t client = NULL;
    uint8_t *header_buf = NULL;
    uint8_t *io_buf = NULL;
    uint8_t *download_buf = NULL;
    wav_info_t wav_info = {0};
    int header_len = 0;
    size_t download_len = 0;
    size_t download_cap = 0;
    bool wav_ready = false;
    bool stream_interrupted = false;
    bool playback_ok = false;

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 12000,
        .buffer_size = IO_BUF_SIZE,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "TTS prebuffer: esp_http_client_init failed");
        goto cleanup;
    }
    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "TTS prebuffer: esp_http_client_open failed");
        goto cleanup;
    }
    esp_http_client_fetch_headers(client);

    header_buf = calloc(1, HEADER_BUF_SIZE);
    io_buf = malloc(IO_BUF_SIZE);
    if (!header_buf || !io_buf) {
        ESP_LOGE(TAG, "TTS prebuffer: out of memory");
        goto cleanup;
    }

    while (!wav_ready && header_len < HEADER_BUF_SIZE && !g_tts_stop_requested) {
        int read_len = esp_http_client_read(client, (char *)header_buf + header_len, HEADER_BUF_SIZE - header_len);
        if (read_len <= 0) {
            ESP_LOGE(TAG, "TTS prebuffer: header read failed (%d)", read_len);
            goto cleanup;
        }
        header_len += read_len;
        if (parse_wav_header_from_buffer(header_buf, (size_t)header_len, &wav_info)) {
            wav_ready = true;
        }
    }
    if (!wav_ready || g_tts_stop_requested) {
        ESP_LOGW(TAG, "TTS prebuffer: WAV header not ready after %d bytes", header_len);
        goto cleanup;
    }
    if (wav_info.audio_format != 1 || wav_info.data_shift >= (uint32_t)header_len) {
        ESP_LOGE(TAG, "TTS prebuffer: unsupported WAV format");
        goto cleanup;
    }

    download_cap = header_len > 65536 ? (size_t)header_len : 65536;
    download_buf = malloc(download_cap);
    if (!download_buf) {
        ESP_LOGE(TAG, "TTS prebuffer: cannot allocate download buffer");
        goto cleanup;
    }
    memcpy(download_buf, header_buf, (size_t)header_len);
    download_len = (size_t)header_len;

    while (!g_tts_stop_requested) {
        int read_len = esp_http_client_read(client, (char *)io_buf, IO_BUF_SIZE);
        if (read_len < 0) {
            ESP_LOGE(TAG, "TTS prebuffer: download read failed (%d)", read_len);
            goto cleanup;
        }
        if (read_len == 0) {
            break;
        }
        if (download_len + (size_t)read_len > download_cap) {
            size_t new_cap = download_cap;
            while (download_len + (size_t)read_len > new_cap) {
                new_cap *= 2;
            }
            uint8_t *grown = realloc(download_buf, new_cap);
            if (!grown) {
                ESP_LOGE(TAG, "TTS prebuffer: cannot grow download buffer to %u bytes", (unsigned)new_cap);
                goto cleanup;
            }
            download_buf = grown;
            download_cap = new_cap;
        }
        memcpy(download_buf + download_len, io_buf, (size_t)read_len);
        download_len += (size_t)read_len;
    }

cleanup:
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(io_buf);
    free(header_buf);
    free(url);
    g_tts_task_handle = NULL;

    if (g_tts_stop_requested) {
        g_tts_stop_requested = false;
        g_tts_playing = false;
        g_tts_started = false;
        g_tts_takeover_active = false;
        clear_info_button_feedback();
        free(download_buf);
        if (stream_interrupted && g_wifi_connected) {
            start_audio_pipeline();
        }
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TTS download ready: %u bytes, %u Hz, %u bit, %u ch",
             (unsigned)download_len,
             (unsigned)wav_info.samplerate,
             (unsigned)wav_info.bits,
             (unsigned)wav_info.channels);

    g_tts_takeover_active = true;
    stream_interrupted = g_pipeline_running;
    if (stream_interrupted) {
        stop_audio_pipeline();
    }

    clear_tts_memory_stream();
    g_tts_memory_stream.data = download_buf;
    g_tts_memory_stream.len = download_len;
    g_tts_memory_stream.pos = 0;
    download_buf = NULL;

    if (play_tts_memory_buffer()) {
        playback_ok = true;
    }

    if (playback_ok) {
        finish_tts_playback("tts buffered done");
        vTaskDelete(NULL);
        return;
    } else {
        ESP_LOGW(TAG, "TTS prebuffer failed, resuming current stream");
        g_tts_playing = false;
        g_tts_started = false;
        g_tts_takeover_active = false;
        clear_info_button_feedback();
        clear_tts_memory_stream();
        if (stream_interrupted && g_wifi_connected) {
            start_audio_pipeline();
        }
    }

    vTaskDelete(NULL);
}

static void tts_live_playback_task(void *arg)
{
    (void)arg;
    bool playback_ok = play_tts_memory_buffer();
    if (playback_ok) {
        finish_tts_playback("tts live done");
    } else {
        ESP_LOGW(TAG, "TTS live playback failed, resuming current stream");
        g_tts_playing = false;
        g_tts_started = false;
        g_tts_takeover_active = false;
        clear_info_button_feedback();
        clear_tts_memory_stream();
        if (g_wifi_connected) {
            start_audio_pipeline();
        }
    }
    vTaskDelete(NULL);
}

static void tts_live_task(void *arg)
{
    static const int IO_BUF_SIZE = 4096;
    char *url = (char *)arg;
    esp_http_client_handle_t client = NULL;
    uint8_t *io_buf = NULL;
    bool stream_interrupted = false;

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 12000,
        .buffer_size = IO_BUF_SIZE,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    clear_tts_memory_stream();
    g_tts_memory_stream.data = malloc(TTS_LIVE_BUFFER_CAPACITY);
    g_tts_memory_stream.capacity = g_tts_memory_stream.data ? TTS_LIVE_BUFFER_CAPACITY : 0;
    if (!g_tts_memory_stream.data) {
        ESP_LOGE(TAG, "TTS live: cannot allocate buffer");
        goto fail;
    }

    io_buf = malloc(IO_BUF_SIZE);
    if (!io_buf) {
        ESP_LOGE(TAG, "TTS live: cannot allocate IO buffer");
        goto fail;
    }

    client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "TTS live: esp_http_client_init failed");
        goto fail;
    }
    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "TTS live: esp_http_client_open failed");
        goto fail;
    }
    ESP_LOGI(TAG, "TTS live HTTP connected");
    esp_http_client_fetch_headers(client);

    while (!g_tts_stop_requested) {
        int read_len = esp_http_client_read(client, (char *)io_buf, IO_BUF_SIZE);
        if (read_len < 0) {
            ESP_LOGE(TAG, "TTS live: read failed (%d)", read_len);
            g_tts_memory_stream.download_failed = true;
            break;
        }
        if (read_len == 0) {
            g_tts_memory_stream.download_complete = true;
            break;
        }
        if (g_tts_memory_stream.len + (size_t)read_len > g_tts_memory_stream.capacity) {
            ESP_LOGW(TAG, "TTS live: buffer full at %u bytes", (unsigned)g_tts_memory_stream.len);
            g_tts_memory_stream.download_complete = true;
            break;
        }
        if (g_tts_memory_stream.len == 0) {
            ESP_LOGI(TAG, "TTS live first audio chunk: %d bytes", read_len);
        }
        memcpy(g_tts_memory_stream.data + g_tts_memory_stream.len, io_buf, (size_t)read_len);
        g_tts_memory_stream.len += (size_t)read_len;

        if (!g_tts_memory_stream.playback_started && g_tts_memory_stream.len >= TTS_LIVE_START_THRESHOLD) {
            ESP_LOGI(TAG, "TTS live prebuffer ready: %u bytes", (unsigned)g_tts_memory_stream.len);
            g_tts_takeover_active = true;
            stream_interrupted = g_pipeline_running;
            if (stream_interrupted) {
                stop_audio_pipeline();
            }
            g_tts_memory_stream.playback_started = true;
            if (xTaskCreate(tts_live_playback_task, "tts_live_play", 8192, NULL, 5, NULL) != pdPASS) {
                ESP_LOGE(TAG, "TTS live: cannot start playback task");
                g_tts_memory_stream.playback_started = false;
                goto fail;
            }
        }
    }

    if (!g_tts_memory_stream.playback_started && g_tts_memory_stream.len > 0 && !g_tts_stop_requested) {
        ESP_LOGI(TAG, "TTS live starting with short buffer: %u bytes", (unsigned)g_tts_memory_stream.len);
        g_tts_takeover_active = true;
        stream_interrupted = g_pipeline_running;
        if (stream_interrupted) {
            stop_audio_pipeline();
        }
        g_tts_memory_stream.playback_started = true;
        if (xTaskCreate(tts_live_playback_task, "tts_live_play", 8192, NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "TTS live: cannot start playback task");
            g_tts_memory_stream.playback_started = false;
            goto fail;
        }
    }

    if (g_tts_stop_requested) {
        g_tts_stop_requested = false;
        g_tts_memory_stream.download_complete = true;
        if (!g_tts_memory_stream.playback_started) {
            g_tts_playing = false;
            g_tts_started = false;
            g_tts_takeover_active = false;
            clear_info_button_feedback();
            clear_tts_memory_stream();
            if (stream_interrupted && g_wifi_connected) {
                start_audio_pipeline();
            }
        }
    }

    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(io_buf);
    free(url);
    g_tts_task_handle = NULL;
    vTaskDelete(NULL);
    return;

fail:
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(io_buf);
    free(url);
    g_tts_task_handle = NULL;
    g_tts_playing = false;
    g_tts_started = false;
    g_tts_takeover_active = false;
    clear_info_button_feedback();
    clear_tts_memory_stream();
    if (stream_interrupted && g_wifi_connected) {
        start_audio_pipeline();
    }
    vTaskDelete(NULL);
}

static void set_codec_volume(int *current_volume, int new_volume)
{
    bool limit_hit = false;
    if (new_volume < 0) {
        new_volume = 0;
        limit_hit = true;
    }
    if (new_volume > 100) {
        new_volume = 100;
        limit_hit = true;
    }
    if (new_volume == *current_volume) {
        if (new_volume == 0 || new_volume == 100) {
            g_led_state.edge_blink_count = 4;
        }
        ESP_LOGI(TAG, "Volume remains at %d%%", *current_volume);
        return;
    }
    ESP_ERROR_CHECK(audio_hal_set_volume(g_board_handle->audio_hal, new_volume));
    *current_volume = new_volume;
    ESP_LOGI(TAG, "Volume set to %d%%", *current_volume);
    g_led_state.edge_blink_count = 1;
    if (limit_hit || *current_volume == 0 || *current_volume == 100) {
        g_led_state.edge_blink_count = 4;
    }
}

static void toggle_mute(bool *is_muted)
{
    *is_muted = !*is_muted;
    ESP_ERROR_CHECK(audio_hal_set_mute(g_board_handle->audio_hal, *is_muted));
    g_transition_mute_active = false;
    g_led_state.muted = *is_muted;
    ESP_LOGI(TAG, "%s", *is_muted ? "Mute enabled" : "Mute disabled");
}

static void switch_stream(int next_stream)
{
    if (next_stream < 0 || next_stream >= (int)(sizeof(kStreams) / sizeof(kStreams[0]))) {
        return;
    }
    if (g_stream_mode == STREAM_MODE_PRESET && g_selected_preset_index == next_stream) {
        ESP_LOGI(TAG, "Stream key pressed, already on preset: %s", kStreams[next_stream].name);
        return;
    }
    ESP_LOGI(TAG, "Switching preset stream -> %s", kStreams[next_stream].name);
    g_tts_playing = false;
    g_stream_mode = STREAM_MODE_PRESET;
    g_selected_preset_index = next_stream;
    g_current_stream = next_stream;
    update_active_stream_from_selection();
    if (g_wifi_connected) {
        g_ignore_restart_until = xTaskGetTickCount() + pdMS_TO_TICKS(RESTART_GUARD_MS);
        restart_current_stream();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            g_last_disconnect_tick = xTaskGetTickCount();
        }
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            g_wifi_connected = false;
            xEventGroupClearBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
            g_last_disconnect_tick = xTaskGetTickCount();
            reset_network_failure_streak("wifi disconnected");
            set_stream_alive_state(false, "Wi-Fi disconnected");
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retrying saved SSID (reason=%d)", disc ? disc->reason : -1);
            if (disc && disc->reason == WIFI_REASON_NO_AP_FOUND) {
                g_force_ap_due_to_missing_network = true;
            }
            if (g_have_saved_creds && xTaskGetTickCount() >= g_suspend_sta_connect_until) {
                esp_wifi_connect();
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        g_wifi_connected = true;
        g_force_ap_due_to_missing_network = false;
        xEventGroupSetBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
        reset_network_failure_streak("got ip");
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (g_ap_active) {
            stop_config_ap();
        }
        if (!g_startup_announcement_played && g_startup_message_text[0] != 0) {
            g_startup_announcement_pending = true;
        }
        request_catalog_refresh(true);
    }
}

void app_main(void)
{
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    g_wifi_event_group = xEventGroupCreate();
    g_sta_netif = esp_netif_create_default_wifi_sta();
    g_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    xTaskCreate(led_task, "led_task", 2048, NULL, 4, NULL);
    xTaskCreate(catalog_poll_task, "catalog_poll", 4096, NULL, 3, &g_catalog_poll_task_handle);

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i) {
        init_key_gpio(&kKeys[i]);
    }

    g_board_handle = audio_board_init();
    audio_hal_ctrl_codec(g_board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);
    ESP_ERROR_CHECK(audio_hal_set_volume(g_board_handle->audio_hal, g_current_volume));
    ESP_ERROR_CHECK(audio_hal_set_mute(g_board_handle->audio_hal, false));

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    g_pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(g_pipeline);

    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.out_rb_size = 1024 * 1024;
    http_cfg.request_size = 4096;
    g_http_stream_reader = http_stream_init(&http_cfg);

    audio_decoder_t auto_decode[] = {
        DEFAULT_ESP_MP3_DECODER_CONFIG(),
        DEFAULT_ESP_WAV_DECODER_CONFIG(),
    };
    esp_decoder_cfg_t decoder_cfg = DEFAULT_ESP_DECODER_CONFIG();
    decoder_cfg.out_rb_size = 16 * 1024;
    g_audio_decoder = esp_decoder_init(&decoder_cfg, auto_decode, sizeof(auto_decode) / sizeof(audio_decoder_t));

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.out_rb_size = 16 * 1024;
    g_i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    audio_pipeline_register(g_pipeline, g_http_stream_reader, "http");
    audio_pipeline_register(g_pipeline, g_audio_decoder, "dec");
    audio_pipeline_register(g_pipeline, g_i2s_stream_writer, "i2s");

    const char *link_tag[3] = {"http", "dec", "i2s"};
    audio_pipeline_link(g_pipeline, &link_tag[0], 3);

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    g_evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(g_pipeline, g_evt);

    load_stream_urls();
    update_active_stream_from_selection();
    g_have_saved_creds = load_wifi_creds();
    g_last_disconnect_tick = xTaskGetTickCount();
    start_config_server();

    if (g_have_saved_creds) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        g_wifi_started = true;
        apply_sta_config_and_connect();
    } else {
        start_config_ap();
    }

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(g_evt, &msg, pdMS_TO_TICKS(20));
        bool restart_guard_active = xTaskGetTickCount() < g_ignore_restart_until;

        if (ret == ESP_OK) {
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
                && msg.source == (void *) g_audio_decoder
                && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
                audio_element_info_t music_info = {0};
                audio_element_getinfo(g_audio_decoder, &music_info);
                i2s_stream_set_clk(g_i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
                g_decoder_locked = true;
                if (g_transition_mute_active && !g_is_muted) {
                    ESP_ERROR_CHECK(audio_hal_set_mute(g_board_handle->audio_hal, false));
                    g_transition_mute_active = false;
                }
                if (g_tts_playing) {
                    g_tts_started = true;
                }
                set_stream_alive_state(true, "decoder locked");
            } else if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
                       && msg.source == (void *) g_http_stream_reader
                       && msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
                int status = (int)msg.data;
                if (status >= AEL_STATUS_ERROR_OPEN && status <= AEL_STATUS_ERROR_UNKNOWN) {
                    set_stream_alive_state(false, "http error");
                    if (g_tts_playing) {
                        finish_tts_playback("http error");
                        continue;
                    }
                    note_network_failure_and_maybe_reconnect("stream http error");
                    schedule_stream_retry(STREAM_RETRY_MS, "http error");
                    if (g_wifi_connected && !restart_guard_active) {
                        restart_current_stream();
                    }
                } else if (status == AEL_STATUS_ERROR_TIMEOUT) {
                    set_stream_alive_state(false, "http timeout");
                    if (g_tts_playing) {
                        finish_tts_playback("http timeout");
                        continue;
                    }
                    note_network_failure_and_maybe_reconnect("stream http timeout");
                    schedule_stream_retry(STREAM_RETRY_MS, "http timeout");
                    if (g_wifi_connected && !restart_guard_active) {
                        restart_current_stream();
                    }
                } else if (status == AEL_STATUS_STATE_RUNNING) {
                    if (g_tts_playing) {
                        g_tts_started = true;
                    }
                    reset_network_failure_streak("stream http running");
                    set_stream_alive_state(true, "http running");
                }
            } else if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
                       && msg.source == (void *) g_i2s_stream_writer
                       && msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
                int status = (int)msg.data;
                if (status == AEL_STATUS_STATE_RUNNING) {
                    if (g_tts_playing) {
                        g_tts_started = true;
                    }
                    if (g_transition_mute_active && g_decoder_locked && !g_is_muted) {
                        ESP_ERROR_CHECK(audio_hal_set_mute(g_board_handle->audio_hal, false));
                        g_transition_mute_active = false;
                    }
                    set_stream_alive_state(true, "i2s running");
                } else if (status == AEL_STATUS_STATE_STOPPED || status == AEL_STATUS_STATE_FINISHED) {
                    set_stream_alive_state(false, "i2s stopped");
                    if (g_tts_takeover_active) {
                        ESP_LOGI(TAG, "Ignoring i2s stopped while TTS takeover is active");
                        continue;
                    }
                    if (g_tts_playing && g_tts_started) {
                        finish_tts_playback("i2s stopped");
                        continue;
                    }
                    schedule_stream_retry(STREAM_RETRY_MS, "i2s stopped");
                    if (g_wifi_connected && !restart_guard_active) {
                        restart_current_stream();
                    }
                }
            }
        }

        if (g_have_saved_creds && !g_wifi_connected) {
            TickType_t elapsed = xTaskGetTickCount() - g_last_disconnect_tick;
            if (!g_ap_active && (g_force_ap_due_to_missing_network || pdTICKS_TO_MS(elapsed) >= WIFI_CONNECT_GRACE_MS)) {
                start_config_ap();
            }
        }

        if (g_catalog_refresh_pending && g_wifi_connected && !g_catalog_task_handle) {
            request_catalog_refresh(false);
        }

        if (g_pending_sta_connect && xTaskGetTickCount() >= g_pending_sta_connect_at) {
            g_pending_sta_connect = false;
            apply_sta_config_and_connect();
        }

        if (g_stream_retry_pending && g_wifi_connected && xTaskGetTickCount() >= g_stream_retry_at) {
            restart_current_stream();
        }

        if (g_startup_announcement_pending && g_wifi_connected && !is_tts_busy()) {
            play_startup_message();
        }

        if (g_wifi_connected && g_ap_active) {
            stop_config_ap();
        }

        for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i) {
            int level = gpio_get_level(kKeys[i].gpio);
            if (level != kKeys[i].last_level) {
                kKeys[i].last_level = level;
                if (level == 0) {
                    g_led_state.edge_blink_count = 1;
                    switch (kKeys[i].action) {
                        case KEY_ACTION_STREAM:
                            switch_stream(kKeys[i].stream_index);
                            break;
                        case KEY_ACTION_VOL_DOWN:
                            set_codec_volume(&g_current_volume, g_current_volume - VOLUME_STEP);
                            break;
                        case KEY_ACTION_VOL_UP:
                            set_codec_volume(&g_current_volume, g_current_volume + VOLUME_STEP);
                            break;
                        case KEY_ACTION_MUTE_TOGGLE:
                            g_key6_pressed_since = xTaskGetTickCount();
                            g_key6_long_press_fired = false;
                            break;
                        case KEY_ACTION_PLAY_TTS:
                            g_led_state.info_blink_active = true;
                            g_tts_pressed_since = xTaskGetTickCount();
                            g_tts_long_press_fired = false;
                            break;
                        case KEY_ACTION_STATION_NEXT:
                            switch_station_next();
                            break;
                        case KEY_ACTION_MOUNT_NEXT:
                            g_mount_pressed_since = xTaskGetTickCount();
                            g_mount_long_press_fired = false;
                            break;
                    }
                } else if (kKeys[i].action == KEY_ACTION_MUTE_TOGGLE) {
                    toggle_mute(&g_is_muted);
                    g_key6_pressed_since = 0;
                    g_key6_long_press_fired = false;
                } else if (kKeys[i].action == KEY_ACTION_PLAY_TTS) {
                    if (!g_tts_long_press_fired) {
                        play_tts_url();
                    } else {
                        clear_info_button_feedback();
                    }
                    g_tts_pressed_since = 0;
                    g_tts_long_press_fired = false;
                } else if (kKeys[i].action == KEY_ACTION_MOUNT_NEXT) {
                    if (!g_mount_long_press_fired) {
                        switch_mount_next();
                    }
                    g_mount_pressed_since = 0;
                    g_mount_long_press_fired = false;
                }
            }
        }

        key_desc_t *mute_key = find_key_by_action(KEY_ACTION_MUTE_TOGGLE);
        if (mute_key && mute_key->last_level == 0 && !g_key6_long_press_fired && g_key6_pressed_since != 0) {
            TickType_t held = xTaskGetTickCount() - g_key6_pressed_since;
            if (pdTICKS_TO_MS(held) >= KEY6_FACTORY_RESET_HOLD_MS) {
                g_key6_long_press_fired = true;
            }
        }

        key_desc_t *tts_key = find_key_by_action(KEY_ACTION_PLAY_TTS);
        if (tts_key && tts_key->last_level == 0 && !g_tts_long_press_fired && g_tts_pressed_since != 0) {
            TickType_t held = xTaskGetTickCount() - g_tts_pressed_since;
            if (pdTICKS_TO_MS(held) >= MOUNT_BUTTON_FALLBACK_HOLD_MS) {
                g_tts_long_press_fired = true;
                clear_info_button_feedback();
                factory_reset_settings();
            }
        }

        key_desc_t *mount_key = find_key_by_action(KEY_ACTION_MOUNT_NEXT);
        if (mount_key && mount_key->last_level == 0 && !g_mount_long_press_fired && g_mount_pressed_since != 0) {
            TickType_t held = xTaskGetTickCount() - g_mount_pressed_since;
            if (pdTICKS_TO_MS(held) >= MOUNT_BUTTON_FALLBACK_HOLD_MS) {
                g_mount_long_press_fired = true;
                toggle_fallback_stream();
            }
        }
    }
}
