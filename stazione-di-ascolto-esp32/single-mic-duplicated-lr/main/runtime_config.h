#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define STREAM_URI_MAX_LEN           192
#define STREAM_DEVICE_NAME_MAX_LEN    64
#define STREAM_PASSWORD_MAX_LEN       64

typedef struct {
    bool have_wifi_credentials;
    char saved_ssid[33];
    char saved_pass[65];
    char stream_uri[STREAM_URI_MAX_LEN];
    char stream_device_name[STREAM_DEVICE_NAME_MAX_LEN];
    char stream_password[STREAM_PASSWORD_MAX_LEN];
} runtime_config_t;

void load_default_runtime_config(runtime_config_t *cfg);
esp_err_t save_runtime_config(const char *ssid,
                              const char *pass,
                              const char *stream_uri,
                              const char *stream_name,
                              const char *stream_password);
bool load_runtime_config(runtime_config_t *cfg);
void erase_runtime_config(void);

void normalize_device_name(char *name, size_t size);
void normalize_stream_uri(char *uri, size_t uri_size);
bool stream_uri_is_supported(const char *uri);
const char *stream_uri_requirements_text(void);

const char *active_stream_uri(const runtime_config_t *cfg, char *resolved_uri, size_t resolved_uri_size);
const char *active_stream_password(const runtime_config_t *cfg);
