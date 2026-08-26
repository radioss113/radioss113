#include "harbor_client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"

#define HARBOR_SEND_TIMEOUT_MS          12000
#define HARBOR_ACCEPT_TIMEOUT_MS         4000
#define HARBOR_SEND_CHUNK_BYTES          1024
#define HARBOR_EAGAIN_RETRIES               2
#define HARBOR_EAGAIN_RETRY_MS             20
#define HARBOR_RETRY_CONNECT_MS          3000
#define HARBOR_RETRY_SEND_FAIL_MS        3000
#define HARBOR_RETRY_STALE_SESSION_MS   35000
#define HARBOR_RETRY_MOUNT_TAKEN_MS     32000
#define HARBOR_POST_ACCEPT_GRACE_MS       150
#define HARBOR_STABLE_SESSION_BYTES    (64 * 1024)
#define HARBOR_STABLE_SESSION_MS         10000

static const char *TAG = "HARBOR_CLIENT";

static void harbor_client_schedule_retry(harbor_client_t *client, harbor_failure_reason_t reason, int delay_ms)
{
    client->stats.last_failure = reason;
    client->stats.last_failure_us = esp_timer_get_time();
    client->stats.next_retry_us = client->stats.last_failure_us + ((int64_t)delay_ms * 1000LL);
}

static void harbor_client_close_internal(harbor_client_t *client)
{
    if (client->sock >= 0) {
        shutdown(client->sock, SHUT_RDWR);
        close(client->sock);
        client->sock = -1;
    }
    client->stats.connected = false;
    client->stats.accepted = false;
    client->stats.connection_bytes_sent = 0;
    client->stats.connected_since_us = 0;
    client->stats.accepted_since_us = 0;
    client->stats.last_send_ok_us = 0;
    client->logged_first_payload = false;
}

static int harbor_send_failure_retry_delay_ms(const harbor_client_t *client)
{
    if (!client) {
        return HARBOR_RETRY_SEND_FAIL_MS;
    }

    int64_t accepted_age_ms = 0;
    if (client->stats.accepted_since_us > 0) {
        int64_t now_us = esp_timer_get_time();
        if (now_us > client->stats.accepted_since_us) {
            accepted_age_ms = (now_us - client->stats.accepted_since_us) / 1000LL;
        }
    }

    if (client->stats.connection_bytes_sent >= HARBOR_STABLE_SESSION_BYTES ||
        accepted_age_ms >= HARBOR_STABLE_SESSION_MS) {
        return HARBOR_RETRY_STALE_SESSION_MS;
    }

    return HARBOR_RETRY_SEND_FAIL_MS;
}

static bool harbor_response_is_success(const char *response)
{
    if (!response) {
        return false;
    }

    return strstr(response, " 200 ") != NULL ||
           strstr(response, " 200\r") != NULL ||
           strstr(response, " 200\n") != NULL;
}

static esp_err_t build_basic_auth_header(harbor_client_t *client)
{
    char plain[128];
    unsigned char encoded[128];
    size_t encoded_len = 0;

    int plain_len = snprintf(plain, sizeof(plain), "%s:%s", client->cfg.user, client->cfg.password);
    if (plain_len <= 0 || plain_len >= (int)sizeof(plain)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int ret = mbedtls_base64_encode(encoded, sizeof(encoded), &encoded_len,
                                    (const unsigned char *)plain, (size_t)plain_len);
    memset(plain, 0, sizeof(plain));
    if (ret != 0 || encoded_len >= sizeof(encoded)) {
        memset(encoded, 0, sizeof(encoded));
        return ESP_FAIL;
    }
    encoded[encoded_len] = 0;

    int header_len = snprintf(client->auth_header, sizeof(client->auth_header), "Basic %s", encoded);
    memset(encoded, 0, sizeof(encoded));
    if (header_len <= 0 || header_len >= (int)sizeof(client->auth_header)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t parse_http_uri(const char *uri, char *host, size_t host_size, int *port, char *path, size_t path_size)
{
    const char *prefix = "http://";
    if (!uri || strncmp(uri, prefix, strlen(prefix)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *authority = uri + strlen(prefix);
    const char *slash = strchr(authority, '/');
    if (!slash) {
        slash = authority + strlen(authority);
    }

    const char *colon = memchr(authority, ':', (size_t)(slash - authority));
    size_t host_len = colon ? (size_t)(colon - authority) : (size_t)(slash - authority);
    if (host_len == 0 || host_len >= host_size) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(host, authority, host_len);
    host[host_len] = 0;
    *port = colon ? atoi(colon + 1) : 80;
    if (*slash == 0) {
        strlcpy(path, "/", path_size);
    } else {
        strlcpy(path, slash, path_size);
    }
    return ESP_OK;
}

static void harbor_log_first_payload(harbor_client_t *client, const uint8_t *data, size_t len)
{
    if (client->logged_first_payload || !data || len == 0) {
        return;
    }

    char hex[3 * 32 + 1] = {0};
    size_t bytes = len < 32 ? len : 32;
    for (size_t i = 0; i < bytes; ++i) {
        snprintf(hex + (i * 3), sizeof(hex) - (i * 3), "%02x ", data[i]);
    }

    ESP_LOGI(TAG, "First payload bytes: %s", hex);
    ESP_LOGI(TAG, "First payload markers: OpusHead=%s OpusTags=%s",
             strstr((const char *)data, "OpusHead") ? "yes" : "no",
             strstr((const char *)data, "OpusTags") ? "yes" : "no");
    client->logged_first_payload = true;
}

static void harbor_log_pending_response(harbor_client_t *client, const char *where)
{
    char response[sizeof(client->stats.last_response)];
    int ret = recv(client->sock, response, sizeof(response) - 1, MSG_DONTWAIT);
    if (ret > 0) {
        response[ret] = 0;
        strlcpy(client->stats.last_response, response, sizeof(client->stats.last_response));
        ESP_LOGI(TAG, "Harbor response after %s: %s", where, response);
    } else if (ret == 0) {
        strlcpy(client->stats.last_response, "peer closed", sizeof(client->stats.last_response));
        ESP_LOGW(TAG, "Harbor closed socket after %s", where);
    } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
        client->stats.last_errno = errno;
        ESP_LOGW(TAG, "Harbor recv after %s failed: errno=%d", where, errno);
    }
}

static esp_err_t harbor_wait_for_accept(harbor_client_t *client)
{
    if (client->stats.accepted) {
        return ESP_OK;
    }

    struct timeval timeout = {
        .tv_sec = HARBOR_ACCEPT_TIMEOUT_MS / 1000,
        .tv_usec = (HARBOR_ACCEPT_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(client->sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char response[sizeof(client->stats.last_response)];
    int ret = recv(client->sock, response, sizeof(response) - 1, 0);
    if (ret > 0) {
        response[ret] = 0;
        strlcpy(client->stats.last_response, response, sizeof(client->stats.last_response));
        if (harbor_response_is_success(response)) {
            client->stats.accepted = true;
            client->stats.accepted_since_us = esp_timer_get_time();
            ESP_LOGI(TAG, "Harbor accepted source: %s", response);
            return ESP_OK;
        }

        client->stats.accept_failures++;
        ESP_LOGW(TAG, "Harbor rejected source: %s", response);
        if (strstr(response, "Mount_taken") != NULL) {
            client->stats.mount_taken_failures++;
            harbor_client_close_internal(client);
            harbor_client_schedule_retry(client, HARBOR_FAIL_MOUNT_TAKEN, HARBOR_RETRY_MOUNT_TAKEN_MS);
            return ESP_ERR_INVALID_STATE;
        }

        harbor_client_close_internal(client);
        harbor_client_schedule_retry(client, HARBOR_FAIL_REJECTED, HARBOR_RETRY_SEND_FAIL_MS);
        return ESP_FAIL;
    }

    client->stats.accept_failures++;
    client->stats.last_errno = errno;
    ESP_LOGW(TAG, "Harbor accept wait failed: ret=%d errno=%d", ret, errno);
    harbor_client_close_internal(client);
    harbor_client_schedule_retry(client, HARBOR_FAIL_ACCEPT_TIMEOUT, HARBOR_RETRY_SEND_FAIL_MS);
    return ESP_ERR_TIMEOUT;
}

esp_err_t harbor_client_init(harbor_client_t *client, const harbor_client_config_t *config)
{
    ESP_RETURN_ON_FALSE(client && config, ESP_ERR_INVALID_ARG, TAG, "invalid args");
    ESP_RETURN_ON_FALSE(config->uri && config->user && config->password, ESP_ERR_INVALID_ARG, TAG, "missing harbor config");

    memset(client, 0, sizeof(*client));
    client->cfg = *config;
    client->sock = -1;
    ESP_RETURN_ON_ERROR(parse_http_uri(config->uri, client->host, sizeof(client->host), &client->port, client->path, sizeof(client->path)),
                        TAG, "invalid harbor uri");
    ESP_RETURN_ON_ERROR(build_basic_auth_header(client), TAG, "auth header build failed");
    return ESP_OK;
}

void harbor_client_deinit(harbor_client_t *client)
{
    if (!client) {
        return;
    }
    harbor_client_close_internal(client);
}

bool harbor_client_is_connected(const harbor_client_t *client)
{
    return client && client->stats.connected;
}

esp_err_t harbor_client_open_if_due(harbor_client_t *client, int64_t now_us)
{
    ESP_RETURN_ON_FALSE(client, ESP_ERR_INVALID_ARG, TAG, "invalid client");
    if (client->stats.connected) {
        return ESP_OK;
    }
    if (client->stats.next_retry_us != 0 && now_us < client->stats.next_retry_us) {
        return ESP_ERR_NOT_FINISHED;
    }

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", client->port);

    ESP_LOGI(TAG, "Connecting Harbor to http://%s:%d%s", client->host, client->port, client->path);
    int err = getaddrinfo(client->host, port_str, &hints, &res);
    if (err != 0 || !res) {
        client->stats.connect_failures++;
        harbor_client_schedule_retry(client, HARBOR_FAIL_CONNECT, HARBOR_RETRY_CONNECT_MS);
        ESP_LOGW(TAG, "getaddrinfo failed: err=%d", err);
        return ESP_FAIL;
    }

    client->sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (client->sock < 0) {
        client->stats.connect_failures++;
        client->stats.last_errno = errno;
        freeaddrinfo(res);
        harbor_client_schedule_retry(client, HARBOR_FAIL_CONNECT, HARBOR_RETRY_CONNECT_MS);
        ESP_LOGW(TAG, "socket failed: errno=%d", errno);
        return ESP_FAIL;
    }

    if (connect(client->sock, res->ai_addr, res->ai_addrlen) != 0) {
        client->stats.connect_failures++;
        client->stats.last_errno = errno;
        freeaddrinfo(res);
        harbor_client_close_internal(client);
        harbor_client_schedule_retry(client, HARBOR_FAIL_CONNECT, HARBOR_RETRY_CONNECT_MS);
        ESP_LOGW(TAG, "connect failed: errno=%d", errno);
        return ESP_FAIL;
    }
    freeaddrinfo(res);

    struct timeval timeout = {
        .tv_sec = HARBOR_SEND_TIMEOUT_MS / 1000,
        .tv_usec = (HARBOR_SEND_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(client->sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(client->sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char header[640];
    int header_len = snprintf(header, sizeof(header),
                              "%s %s %s\r\n"
                              "Host: %s:%d\r\n"
                              "Authorization: %s\r\n"
                              "Content-Type: %s\r\n"
                              "Ice-Name: %s\r\n"
                              "Ice-Description: %s\r\n"
                              "Ice-Genre: live\r\n"
                              "Ice-Bitrate: %d\r\n"
                              "Ice-Audio-Info: ice-samplerate=%d;ice-bitrate=%d;ice-channels=%d\r\n"
                              "Ice-Public: 0\r\n"
                              "User-Agent: esp32-lyrat-mini-harbor-mic\r\n"
                              "\r\n",
                              client->cfg.use_source_method ? "SOURCE" : "POST",
                              client->path,
                              client->cfg.use_source_method ? "ICE/1.0" : "HTTP/1.0",
                              client->host,
                              client->port,
                              client->auth_header,
                              client->cfg.content_type,
                              client->cfg.ice_name,
                              client->cfg.ice_description,
                              client->cfg.bitrate_bps / 1000,
                              client->cfg.sample_rate_hz,
                              client->cfg.bitrate_bps / 1000,
                              client->cfg.channel_count);
    if (header_len <= 0 || header_len >= (int)sizeof(header)) {
        harbor_client_close_internal(client);
        harbor_client_schedule_retry(client, HARBOR_FAIL_HEADER_SEND, HARBOR_RETRY_CONNECT_MS);
        return ESP_ERR_INVALID_SIZE;
    }

    int sent = 0;
    while (sent < header_len) {
        int ret = send(client->sock, header + sent, (size_t)(header_len - sent), 0);
        if (ret <= 0) {
            client->stats.connect_failures++;
            client->stats.last_errno = errno;
            harbor_client_close_internal(client);
            harbor_client_schedule_retry(client, HARBOR_FAIL_HEADER_SEND, HARBOR_RETRY_CONNECT_MS);
            ESP_LOGW(TAG, "header send failed: errno=%d", errno);
            return ESP_FAIL;
        }
        sent += ret;
    }

    client->stats.connected = true;
    client->stats.accepted = false;
    client->stats.connection_bytes_sent = 0;
    client->stats.connected_since_us = esp_timer_get_time();
    client->stats.accepted_since_us = 0;
    client->stats.last_send_ok_us = 0;
    client->stats.last_response[0] = 0;
    client->stats.last_errno = 0;
    client->logged_first_payload = false;

    esp_err_t accept_err = harbor_wait_for_accept(client);
    if (accept_err != ESP_OK) {
        return accept_err;
    }

    if (HARBOR_POST_ACCEPT_GRACE_MS > 0) {
        vTaskDelay(pdMS_TO_TICKS(HARBOR_POST_ACCEPT_GRACE_MS));
    }

    client->stats.connection_generation++;
    client->stats.next_retry_us = 0;
    client->stats.last_failure = HARBOR_FAIL_NONE;
    client->stats.last_failure_us = 0;
    return ESP_OK;
}

esp_err_t harbor_client_send(harbor_client_t *client, const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(client && data && len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid send args");
    ESP_RETURN_ON_FALSE(client->stats.connected, ESP_ERR_INVALID_STATE, TAG, "harbor not connected");

    harbor_log_first_payload(client, data, len);

    size_t sent = 0;
    int eagain_retries = 0;
    while (sent < len) {
        size_t send_len = len - sent;
        if (send_len > HARBOR_SEND_CHUNK_BYTES) {
            send_len = HARBOR_SEND_CHUNK_BYTES;
        }

        int ret = send(client->sock, data + sent, send_len, 0);
        if (ret <= 0) {
            int send_errno = errno;
            if ((send_errno == EAGAIN || send_errno == EWOULDBLOCK || send_errno == ETIMEDOUT) &&
                eagain_retries < HARBOR_EAGAIN_RETRIES) {
                eagain_retries++;
                ESP_LOGW(TAG, "send retry: errno=%d retry=%d sent=%u/%u",
                         send_errno, eagain_retries, (unsigned)sent, (unsigned)len);
                vTaskDelay(pdMS_TO_TICKS(HARBOR_EAGAIN_RETRY_MS));
                continue;
            }

            client->stats.send_failures++;
            client->stats.last_errno = send_errno;
            int retry_ms = harbor_send_failure_retry_delay_ms(client);
            int64_t accepted_age_ms = 0;
            uint64_t conn_bytes = client->stats.connection_bytes_sent;
            if (client->stats.accepted_since_us > 0) {
                int64_t now_us = esp_timer_get_time();
                if (now_us > client->stats.accepted_since_us) {
                    accepted_age_ms = (now_us - client->stats.accepted_since_us) / 1000LL;
                }
            }
            harbor_log_pending_response(client, "send failure");
            harbor_client_close_internal(client);
            harbor_client_schedule_retry(client,
                                         send_errno == 0 ? HARBOR_FAIL_PEER_CLOSED : HARBOR_FAIL_SEND,
                                         retry_ms);
            ESP_LOGW(TAG,
                     "Scheduling Harbor retry in %d ms after send failure (accepted_age_ms=%lld conn_bytes=%llu)",
                     retry_ms,
                     (long long)accepted_age_ms,
                     (unsigned long long)conn_bytes);
            ESP_LOGE(TAG, "send failed: errno=%d sent=%u/%u", send_errno, (unsigned)sent, (unsigned)len);
            return ESP_FAIL;
        }

        eagain_retries = 0;
        if ((size_t)ret < send_len) {
            client->stats.partial_writes++;
            ESP_LOGW(TAG, "partial send: ret=%d asked=%u remaining=%u",
                     ret, (unsigned)send_len, (unsigned)(len - sent));
        }
        sent += (size_t)ret;
        client->stats.total_bytes_sent += (uint64_t)ret;
        client->stats.connection_bytes_sent += (uint64_t)ret;
        client->stats.last_send_ok_us = esp_timer_get_time();
    }

    if ((client->stats.total_bytes_sent % (64 * 1024)) < len) {
        ESP_LOGI(TAG, "Harbor bytes sent=%llu", (unsigned long long)client->stats.total_bytes_sent);
        harbor_log_pending_response(client, "data send");
    }

    return ESP_OK;
}

void harbor_client_note_wifi_down(harbor_client_t *client)
{
    if (!client) {
        return;
    }
    if (client->stats.connected) {
        ESP_LOGW(TAG, "Closing Harbor because Wi-Fi is down");
    }
    harbor_client_close_internal(client);
    harbor_client_schedule_retry(client, HARBOR_FAIL_WIFI_DOWN, HARBOR_RETRY_CONNECT_MS);
}

void harbor_client_get_stats(const harbor_client_t *client, harbor_client_stats_t *stats)
{
    if (!client || !stats) {
        return;
    }
    *stats = client->stats;
}

const char *harbor_failure_reason_str(harbor_failure_reason_t reason)
{
    switch (reason) {
        case HARBOR_FAIL_NONE:
            return "none";
        case HARBOR_FAIL_CONNECT:
            return "connect";
        case HARBOR_FAIL_HEADER_SEND:
            return "header_send";
        case HARBOR_FAIL_ACCEPT_TIMEOUT:
            return "accept_timeout";
        case HARBOR_FAIL_REJECTED:
            return "rejected";
        case HARBOR_FAIL_MOUNT_TAKEN:
            return "mount_taken";
        case HARBOR_FAIL_SEND:
            return "send";
        case HARBOR_FAIL_PEER_CLOSED:
            return "peer_closed";
        case HARBOR_FAIL_WIFI_DOWN:
            return "wifi_down";
        default:
            return "unknown";
    }
}
