#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    HARBOR_FAIL_NONE = 0,
    HARBOR_FAIL_CONNECT,
    HARBOR_FAIL_HEADER_SEND,
    HARBOR_FAIL_ACCEPT_TIMEOUT,
    HARBOR_FAIL_REJECTED,
    HARBOR_FAIL_MOUNT_TAKEN,
    HARBOR_FAIL_SEND,
    HARBOR_FAIL_PEER_CLOSED,
    HARBOR_FAIL_WIFI_DOWN,
} harbor_failure_reason_t;

typedef struct {
    const char *uri;
    const char *user;
    const char *password;
    const char *content_type;
    const char *ice_name;
    const char *ice_description;
    int sample_rate_hz;
    int channel_count;
    int bitrate_bps;
    bool use_source_method;
} harbor_client_config_t;

typedef struct {
    bool connected;
    bool accepted;
    uint32_t connection_generation;
    uint32_t connect_failures;
    uint32_t accept_failures;
    uint32_t mount_taken_failures;
    uint32_t send_failures;
    uint32_t partial_writes;
    uint64_t total_bytes_sent;
    uint64_t connection_bytes_sent;
    int last_errno;
    int64_t last_failure_us;
    int64_t next_retry_us;
    int64_t connected_since_us;
    int64_t accepted_since_us;
    int64_t last_send_ok_us;
    harbor_failure_reason_t last_failure;
    char last_response[160];
} harbor_client_stats_t;

typedef struct harbor_client {
    harbor_client_config_t cfg;
    harbor_client_stats_t stats;
    int sock;
    char host[96];
    char path[128];
    int port;
    char auth_header[160];
    bool logged_first_payload;
} harbor_client_t;

esp_err_t harbor_client_init(harbor_client_t *client, const harbor_client_config_t *config);
void harbor_client_deinit(harbor_client_t *client);
bool harbor_client_is_connected(const harbor_client_t *client);
esp_err_t harbor_client_open_if_due(harbor_client_t *client, int64_t now_us);
esp_err_t harbor_client_send(harbor_client_t *client, const uint8_t *data, size_t len);
void harbor_client_note_wifi_down(harbor_client_t *client);
void harbor_client_get_stats(const harbor_client_t *client, harbor_client_stats_t *stats);
const char *harbor_failure_reason_str(harbor_failure_reason_t reason);
