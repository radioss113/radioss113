#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "runtime_config.h"

typedef struct {
    bool connected;
    bool portal_active;
    bool have_saved_credentials;
    uint32_t disconnect_count;
    int32_t last_disconnect_reason;
    int64_t last_connect_us;
    int64_t last_disconnect_us;
    char ip_addr[16];
} wifi_station_stats_t;

esp_err_t wifi_station_init(const char *ssid, const char *password);
bool wifi_station_is_connected(void);
void wifi_station_get_stats(wifi_station_stats_t *stats);
void wifi_station_get_runtime_config(runtime_config_t *config);
bool wifi_station_take_reconfigure_request(void);
bool wifi_station_take_factory_reset_request(void);
void wifi_station_perform_reconfigure_restart(void);
void wifi_station_perform_factory_reset_and_restart(void);
