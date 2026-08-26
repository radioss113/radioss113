#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "audio_metrics.h"
#include "harbor_client.h"
#include "mic_source.h"
#include "opus_ogg_streamer.h"
#include "pcm_conditioner.h"
#include "pcm_dc_blocker.h"
#include "runtime_config.h"
#include "snr_probe.h"
#include "wifi_station.h"

static const char *TAG = "A1S_STREAM";

#define CAPTURE_SAMPLE_RATE_HZ           48000
#define CAPTURE_REQUEST_BITS_PER_SAMPLE     16
#define STREAM_PCM_BITS_PER_SAMPLE          16
#define STREAM_CHANNEL_COUNT                 2
#define CAPTURE_READ_BLOCK_FRAMES          512
#define CAPTURE_READ_BLOCK_SAMPLES (CAPTURE_READ_BLOCK_FRAMES * STREAM_CHANNEL_COUNT)
#define CAPTURE_DMA_FRAME_SAMPLES          256
#define CAPTURE_READ_TIMEOUT_MS           1000
#define STREAM_FRAME_MS                     20
#define STREAM_FRAME_SAMPLES_PER_CHANNEL ((CAPTURE_SAMPLE_RATE_HZ * STREAM_FRAME_MS) / 1000)
#define STREAM_FRAME_SAMPLES (STREAM_FRAME_SAMPLES_PER_CHANNEL * STREAM_CHANNEL_COUNT)
#define PCM_QUEUE_DEPTH                     16
#define STREAM_TASK_STACK_BYTES          65536
#define METRICS_LOG_INTERVAL_MS           1000
#define POWER_ON_SETTLE_MS                3000
#define WIFI_LATE_INIT_DELAY_MS           8000
#define DC_BLOCK_CUTOFF_HZ                20.0f
#define OPUS_BITRATE_BPS               128000
#define OPUS_COMPLEXITY                     0
#define OPUS_WARMUP_PACKETS                 5
#define SOFTWARE_MIC_GAIN_NUM               2
#define SOFTWARE_MIC_GAIN_DEN               1
#define MIC_PROFILE_NAME                    "A1S left only duplicated to stereo"
#define MIC_ES8388_GAIN_DB                  24
#define ENABLE_PCM_CONDITIONER               0
#define ENABLE_SNR_PROBE                     0
#define PCM_CONDITIONER_WARMUP_FRAMES        80
#define PCM_CONDITIONER_QUIET_MARGIN_DB     3.0
#define PCM_CONDITIONER_EXPANDER_OPEN_DB   18.0
#define PCM_CONDITIONER_EXPANDER_MAX_DB     3.0
#define PCM_CONDITIONER_LIMITER_DBFS       -8.0
#define PCM_CONDITIONER_ATTACK_COEFF        0.35
#define PCM_CONDITIONER_RELEASE_COEFF       0.05
#define SNR_PROBE_WARMUP_QUIET_FRAMES         80
#define SNR_PROBE_MIN_EVENT_FRAMES             5
#define SNR_PROBE_MAX_EVENT_FRAMES           400
#define SNR_PROBE_RELEASE_QUIET_FRAMES        15
#define SNR_PROBE_TRIGGER_MARGIN_DB         10.0
#define SNR_PROBE_RELEASE_MARGIN_DB          6.0
#define SNR_PROBE_QUIET_UPDATE_MARGIN_DB     3.0

typedef struct {
    uint32_t sequence;
    int64_t capture_us;
    size_t sample_count;
    audio_block_metrics_t metrics;
    int16_t samples[STREAM_FRAME_SAMPLES];
} pcm_frame_t;

typedef struct {
    uint64_t total_primary_samples;
    uint64_t total_raw_frames;
    uint32_t read_calls;
    uint32_t hard_failures;
    uint32_t queue_drops;
    uint32_t queue_high_watermark;
    uint32_t last_frame_sequence;
    bool has_reference;
    audio_block_metrics_t last_raw_primary;
    audio_block_metrics_t last_raw_reference;
    audio_block_metrics_t last_raw_left;
    audio_block_metrics_t last_raw_right;
    audio_block_metrics_t last_filtered_frame;
} capture_stats_t;

typedef struct {
    uint32_t frames_encoded;
    uint32_t frames_dropped_offline;
    uint32_t submit_failures;
    uint32_t harbor_reconnects;
    uint32_t last_harbor_generation;
    int64_t last_stream_ok_us;
} stream_stats_t;

static QueueHandle_t s_pcm_queue;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static capture_stats_t s_capture_stats;
static stream_stats_t s_stream_stats;
static harbor_client_t s_harbor;
static opus_ogg_streamer_t s_streamer;
#if ENABLE_SNR_PROBE
static snr_probe_state_t s_snr_probe;
#endif
#if ENABLE_PCM_CONDITIONER
static pcm_conditioner_state_t s_pcm_conditioner;
#endif
static TaskHandle_t s_capture_task_handle;
static TaskHandle_t s_stream_task_handle;
static TaskHandle_t s_wifi_init_task_handle;
static runtime_config_t s_runtime_config;
static char s_harbor_uri_resolved[STREAM_URI_MAX_LEN + STREAM_DEVICE_NAME_MAX_LEN];
static char s_harbor_ice_name[96];
static char s_harbor_ice_description[160];
#if ENABLE_SNR_PROBE
static const snr_probe_config_t s_snr_probe_cfg = {
    .warmup_quiet_frames = SNR_PROBE_WARMUP_QUIET_FRAMES,
    .min_event_frames = SNR_PROBE_MIN_EVENT_FRAMES,
    .max_event_frames = SNR_PROBE_MAX_EVENT_FRAMES,
    .release_quiet_frames = SNR_PROBE_RELEASE_QUIET_FRAMES,
    .trigger_margin_db = SNR_PROBE_TRIGGER_MARGIN_DB,
    .release_margin_db = SNR_PROBE_RELEASE_MARGIN_DB,
    .quiet_update_margin_db = SNR_PROBE_QUIET_UPDATE_MARGIN_DB,
};
#endif
#if ENABLE_PCM_CONDITIONER
static const pcm_conditioner_config_t s_pcm_conditioner_cfg = {
    .base_gain = (double)SOFTWARE_MIC_GAIN_NUM / (double)SOFTWARE_MIC_GAIN_DEN,
    .quiet_update_margin_db = PCM_CONDITIONER_QUIET_MARGIN_DB,
    .expander_full_open_db = PCM_CONDITIONER_EXPANDER_OPEN_DB,
    .expander_max_atten_db = PCM_CONDITIONER_EXPANDER_MAX_DB,
    .limiter_threshold_dbfs = PCM_CONDITIONER_LIMITER_DBFS,
    .attack_coeff = PCM_CONDITIONER_ATTACK_COEFF,
    .release_coeff = PCM_CONDITIONER_RELEASE_COEFF,
    .warmup_frames = PCM_CONDITIONER_WARMUP_FRAMES,
};
#endif

static double safe_db(double value)
{
    if (isinf(value)) {
        return -999.0;
    }
    return value;
}

#if ENABLE_PCM_CONDITIONER
static void compute_fast_level_metrics_i16(const int16_t *samples, size_t count, audio_block_metrics_t *metrics)
{
    audio_metrics_reset(metrics);
    if (!samples || count == 0 || !metrics) {
        return;
    }

    double sum_sq = 0.0;
    int32_t peak_abs = 0;
    uint32_t clipped = 0;

    for (size_t i = 0; i < count; ++i) {
        int32_t sample = samples[i];
        int32_t abs_sample = sample < 0 ? -sample : sample;
        if (abs_sample > peak_abs) {
            peak_abs = abs_sample;
        }
        if (sample == INT16_MIN || sample == INT16_MAX) {
            clipped++;
        }
        sum_sq += (double)sample * (double)sample;
    }

    metrics->sample_count = count;
    metrics->peak_abs = peak_abs;
    metrics->ac_peak_abs = peak_abs;
    metrics->rms = sqrt(sum_sq / (double)count);
    metrics->ac_rms = metrics->rms;
    metrics->clipped_samples = clipped;
}
#endif

static void capture_stats_note_read(const mic_source_read_info_t *info)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_capture_stats.total_primary_samples += info->primary_samples;
    s_capture_stats.total_raw_frames += info->raw_frames_read;
    s_capture_stats.read_calls++;
    s_capture_stats.has_reference = info->has_reference;
    if (info->has_lane_metrics) {
        s_capture_stats.last_raw_primary = info->primary_metrics;
        s_capture_stats.last_raw_reference = info->reference_metrics;
        s_capture_stats.last_raw_left = info->left_hi_metrics;
        s_capture_stats.last_raw_right = info->right_hi_metrics;
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

static void capture_stats_note_frame(uint32_t sequence, const audio_block_metrics_t *metrics)
{
    UBaseType_t queued = uxQueueMessagesWaiting(s_pcm_queue);

    portENTER_CRITICAL(&s_stats_lock);
    s_capture_stats.last_frame_sequence = sequence;
    s_capture_stats.last_filtered_frame = *metrics;
    if ((uint32_t)queued > s_capture_stats.queue_high_watermark) {
        s_capture_stats.queue_high_watermark = (uint32_t)queued;
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

static void capture_stats_note_drop(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_capture_stats.queue_drops++;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void capture_stats_note_failure(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_capture_stats.hard_failures++;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void stream_stats_note_frame_encoded(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_stream_stats.frames_encoded++;
    s_stream_stats.last_stream_ok_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_stats_lock);
}

static void stream_stats_note_frame_dropped_offline(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_stream_stats.frames_dropped_offline++;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void stream_stats_note_submit_failure(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_stream_stats.submit_failures++;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void stream_stats_note_generation(uint32_t generation)
{
    portENTER_CRITICAL(&s_stats_lock);
    if (generation != s_stream_stats.last_harbor_generation) {
        s_stream_stats.harbor_reconnects++;
        s_stream_stats.last_harbor_generation = generation;
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

static esp_err_t harbor_page_writer(void *ctx, const uint8_t *data, size_t len)
{
    harbor_client_t *harbor = (harbor_client_t *)ctx;
    return harbor_client_send(harbor, data, len);
}

static void wifi_init_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG,
             "Deferring Wi-Fi init by %d ms so firmware startup completes before radio bring-up",
             WIFI_LATE_INIT_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(WIFI_LATE_INIT_DELAY_MS));

    esp_err_t err = wifi_station_init(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Late Wi-Fi init failed: %s", esp_err_to_name(err));
        s_wifi_init_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    wifi_station_get_runtime_config(&s_runtime_config);
    ESP_LOGI(TAG, "Late Wi-Fi init complete");
    s_wifi_init_task_handle = NULL;
    vTaskDelete(NULL);
}

static void capture_task(void *arg)
{
    (void)arg;

    int16_t *read_block = calloc(CAPTURE_READ_BLOCK_SAMPLES, sizeof(int16_t));
    int16_t *frame_buf = calloc(STREAM_FRAME_SAMPLES, sizeof(int16_t));
    pcm_frame_t *frame = calloc(1, sizeof(pcm_frame_t));
    size_t frame_fill = 0;
    uint32_t frame_sequence = 0;
    pcm_dc_blocker_t dc_blockers[STREAM_CHANNEL_COUNT];
    audio_block_metrics_t last_frame_metrics = {0};
#if ENABLE_PCM_CONDITIONER
    audio_block_metrics_t read_metrics;
#endif

    if (!read_block || !frame_buf || !frame) {
        ESP_LOGE(TAG, "capture_task alloc failed");
        free(read_block);
        free(frame_buf);
        free(frame);
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        mic_source_t mic = mic_source_a1s_es8388_stereo_create();
        mic_source_config_t mic_cfg = {
            .profile_name = MIC_PROFILE_NAME,
            .sample_rate_hz = CAPTURE_SAMPLE_RATE_HZ,
            .bits_per_sample = CAPTURE_REQUEST_BITS_PER_SAMPLE,
            .dma_frame_samples = CAPTURE_DMA_FRAME_SAMPLES,
            .read_timeout_ms = CAPTURE_READ_TIMEOUT_MS,
            .es8388_mic_gain_db = MIC_ES8388_GAIN_DB,
            .primary_mode = MIC_SOURCE_PRIMARY_LEFT,
        };

        esp_err_t err = mic_source_init(&mic, &mic_cfg);
        if (err != ESP_OK) {
            capture_stats_note_failure();
            ESP_LOGE(TAG, "Mic init failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        for (size_t ch = 0; ch < STREAM_CHANNEL_COUNT; ++ch) {
            pcm_dc_blocker_init(&dc_blockers[ch], CAPTURE_SAMPLE_RATE_HZ, DC_BLOCK_CUTOFF_HZ);
        }
#if ENABLE_PCM_CONDITIONER
        pcm_conditioner_init(&s_pcm_conditioner, &s_pcm_conditioner_cfg);
#endif
        frame_fill = 0;
        ESP_LOGI(TAG, "Mic capture ready: %s", mic_source_describe(&mic));

        while (true) {
            mic_source_read_info_t info;
            int samples = mic_source_read(&mic, read_block, CAPTURE_READ_BLOCK_SAMPLES, &info);
            if (samples < 0) {
                capture_stats_note_failure();
                ESP_LOGE(TAG, "Mic read failed hard, reinitializing capture");
                break;
            }
            if (samples == 0) {
                continue;
            }

            capture_stats_note_read(&info);
            pcm_dc_blocker_process_interleaved_i16(dc_blockers,
                                                  STREAM_CHANNEL_COUNT,
                                                  read_block,
                                                  (size_t)samples);
#if ENABLE_PCM_CONDITIONER
            compute_fast_level_metrics_i16(read_block, (size_t)samples, &read_metrics);
            pcm_conditioner_process_i16(&s_pcm_conditioner,
                                        &s_pcm_conditioner_cfg,
                                        read_block,
                                        (size_t)samples,
                                        &read_metrics);
#endif

            size_t offset = 0;
            while (offset < (size_t)samples) {
                size_t copy_count = STREAM_FRAME_SAMPLES - frame_fill;
                size_t remaining = (size_t)samples - offset;
                if (copy_count > remaining) {
                    copy_count = remaining;
                }

                memcpy(frame_buf + frame_fill, read_block + offset, copy_count * sizeof(int16_t));
                frame_fill += copy_count;
                offset += copy_count;

                if (frame_fill < STREAM_FRAME_SAMPLES) {
                    continue;
                }

                memset(frame, 0, sizeof(*frame));
                frame->sequence = ++frame_sequence;
                frame->capture_us = esp_timer_get_time();
                frame->sample_count = STREAM_FRAME_SAMPLES;
                memcpy(frame->samples, frame_buf, sizeof(frame->samples));
                if ((frame_sequence % 16u) == 1u) {
                    audio_metrics_compute_i16(frame->samples, frame->sample_count, &last_frame_metrics);
                }
                frame->metrics = last_frame_metrics;
#if ENABLE_SNR_PROBE
                snr_probe_process_frame(&s_snr_probe,
                                        &s_snr_probe_cfg,
                                        &frame->metrics,
                                        CAPTURE_SAMPLE_RATE_HZ);
#endif
                capture_stats_note_frame(frame->sequence, &frame->metrics);

                if (xQueueSend(s_pcm_queue, frame, 0) != pdPASS) {
                    pcm_frame_t discarded;
                    if (xQueueReceive(s_pcm_queue, &discarded, 0) == pdPASS &&
                        xQueueSend(s_pcm_queue, frame, 0) == pdPASS) {
                        capture_stats_note_drop();
                    } else {
                        capture_stats_note_drop();
                    }
                }

                frame_fill = 0;
                if ((frame_sequence % 50u) == 0u) {
                    vTaskDelay(1);
                }
            }
        }

        mic_source_deinit(&mic);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void stream_task(void *arg)
{
    (void)arg;

    pcm_frame_t *frame = calloc(1, sizeof(pcm_frame_t));
    int last_generation = 0;
    if (!frame) {
        ESP_LOGE(TAG, "stream_task alloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        bool have_frame = xQueueReceive(s_pcm_queue, frame, pdMS_TO_TICKS(200)) == pdPASS;
        bool wifi_connected = wifi_station_is_connected();

        if (!wifi_connected) {
            harbor_client_note_wifi_down(&s_harbor);
            if (have_frame) {
                stream_stats_note_frame_dropped_offline();
            }
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        if (!harbor_client_is_connected(&s_harbor)) {
            if (harbor_client_open_if_due(&s_harbor, now_us) == ESP_OK) {
                harbor_client_stats_t harbor_stats;
                harbor_client_get_stats(&s_harbor, &harbor_stats);
                if ((int)harbor_stats.connection_generation != last_generation) {
                    last_generation = (int)harbor_stats.connection_generation;
                    stream_stats_note_generation(harbor_stats.connection_generation);
                    opus_ogg_streamer_reset_stream(&s_streamer);
                    xQueueReset(s_pcm_queue);
                    ESP_LOGI(TAG,
                             "Harbor connected generation=%" PRIu32 ": reset Opus/Ogg stream and cleared stale PCM queue",
                             harbor_stats.connection_generation);
                }
            }
        }

        if (!have_frame) {
            continue;
        }

        if (!harbor_client_is_connected(&s_harbor)) {
            stream_stats_note_frame_dropped_offline();
            continue;
        }

        if (opus_ogg_streamer_submit(&s_streamer,
                                     frame->samples,
                                     frame->sample_count,
                                     harbor_page_writer,
                                     &s_harbor) != ESP_OK) {
            stream_stats_note_submit_failure();
            continue;
        }

        stream_stats_note_frame_encoded();
    }
}

static void log_runtime_diag(void)
{
    static uint64_t prev_capture_samples;
    static int64_t prev_log_us;

    capture_stats_t capture;
    stream_stats_t stream;
    wifi_station_stats_t wifi;
    harbor_client_stats_t harbor;
    opus_ogg_streamer_stats_t opus;
#if ENABLE_SNR_PROBE
    snr_probe_state_t snr;
#endif
#if ENABLE_PCM_CONDITIONER
    pcm_conditioner_state_t conditioner;
#endif

    portENTER_CRITICAL(&s_stats_lock);
    capture = s_capture_stats;
    stream = s_stream_stats;
    portEXIT_CRITICAL(&s_stats_lock);
    wifi_station_get_stats(&wifi);
    harbor_client_get_stats(&s_harbor, &harbor);
    opus_ogg_streamer_get_stats(&s_streamer, &opus);
#if ENABLE_SNR_PROBE
    snr_probe_get_state(&s_snr_probe, &snr);
#endif
#if ENABLE_PCM_CONDITIONER
    pcm_conditioner_get_state(&s_pcm_conditioner, &conditioner);
#endif

    int64_t now_us = esp_timer_get_time();
    double sr_est = 0.0;
    if (prev_log_us != 0 && now_us > prev_log_us) {
        sr_est = ((double)(capture.total_primary_samples - prev_capture_samples) * 1000000.0) /
                 (double)(now_us - prev_log_us);
    }
    prev_capture_samples = capture.total_primary_samples;
    prev_log_us = now_us;

    UBaseType_t capture_stack_hw = s_capture_task_handle ? uxTaskGetStackHighWaterMark(s_capture_task_handle) : 0;
    UBaseType_t stream_stack_hw = s_stream_task_handle ? uxTaskGetStackHighWaterMark(s_stream_task_handle) : 0;
    int64_t retry_ms = 0;
    if (harbor.next_retry_us > now_us) {
        retry_ms = (harbor.next_retry_us - now_us) / 1000LL;
    }

    ESP_LOGI(TAG,
             "diag sr_est=%.1f q=%u/%u drops=%" PRIu32 " stack[capture_hw=%u stream_hw=%u]"
             " mic[ac=%.1f acpk=%.1f clip=%" PRIu32 " dc=%.1f]"
             " wifi[up=%d disc=%" PRIu32 " ip=%s]"
             " harbor[conn=%d acc=%d gen=%" PRIu32 " bytes=%llu connb=%llu partial=%" PRIu32
             " fail=%" PRIu32 " mount=%" PRIu32 " last=%s errno=%d retry=%lldms]"
             " opus[pkt=%" PRIu32 " warm=%" PRIu32 " page=%" PRIu32 " last=%d]",
             sr_est,
             (unsigned)uxQueueMessagesWaiting(s_pcm_queue),
             PCM_QUEUE_DEPTH,
             capture.queue_drops,
             (unsigned)capture_stack_hw,
             (unsigned)stream_stack_hw,
             safe_db(audio_metrics_dbfs_from_rms(capture.last_filtered_frame.ac_rms)),
             safe_db(audio_metrics_dbfs_from_peak(capture.last_filtered_frame.ac_peak_abs)),
             capture.last_filtered_frame.clipped_samples,
             capture.last_filtered_frame.dc_offset,
             wifi.connected ? 1 : 0,
             wifi.disconnect_count,
             wifi.ip_addr[0] ? wifi.ip_addr : "-",
             harbor.connected ? 1 : 0,
             harbor.accepted ? 1 : 0,
             harbor.connection_generation,
             (unsigned long long)harbor.total_bytes_sent,
             (unsigned long long)harbor.connection_bytes_sent,
             harbor.partial_writes,
             harbor.send_failures + harbor.connect_failures + harbor.accept_failures,
             harbor.mount_taken_failures,
             harbor_failure_reason_str(harbor.last_failure),
             harbor.last_errno,
             (long long)retry_ms,
             opus.packet_count,
             opus.warmup_drops,
             opus.page_count,
             opus.last_packet_bytes);

#if ENABLE_SNR_PROBE
    ESP_LOGI(TAG,
             "snr[floor=%s nf=%.1f event=%d cand=%" PRIu32 " ef=%" PRIu32
             " last=%.1f sig=%.1f peak=%.1f dur=%" PRIu32 "ms clip=%" PRIu32 "]",
             snr.noise_floor_ready ? "ready" : "learn",
             safe_db(audio_metrics_dbfs_from_rms(snr.noise_floor_rms)),
             snr.event_active ? 1 : 0,
             snr.candidate_frames,
             snr.event_frames,
             snr.last_event_valid ? snr.last_snr_db : -999.0,
             snr.last_event_valid ? safe_db(audio_metrics_dbfs_from_rms(snr.last_signal_rms)) : -999.0,
             snr.last_event_valid ? safe_db(audio_metrics_dbfs_from_peak(snr.last_peak_abs)) : -999.0,
             snr.last_event_valid ? snr.last_event_duration_ms : 0,
             snr.last_event_valid ? snr.last_clipped_samples : 0);
#endif

#if ENABLE_PCM_CONDITIONER
    ESP_LOGI(TAG,
             "proc[floor=%s nf=%.1f gain=%.2f target=%.2f exp=%.1f lim=%d]",
             conditioner.noise_floor_ready ? "ready" : "learn",
             safe_db(audio_metrics_dbfs_from_rms(conditioner.noise_floor_rms)),
             conditioner.current_gain,
             conditioner.last_target_gain,
             conditioner.last_expander_atten_db,
             conditioner.last_limiter_active ? 1 : 0);
#endif

    if (capture.has_reference) {
        ESP_LOGI(TAG,
                 "audio stereo[L_ac=%.1f L_dc=%.1f R_ac=%.1f R_dc=%.1f] raw_mix[ac=%.1f dc=%.1f] frame_seq=%" PRIu32
                 " encoded=%" PRIu32 " offline_drop=%" PRIu32 " submit_fail=%" PRIu32
                 " reconnects=%" PRIu32 " last_resp=%s",
                 safe_db(audio_metrics_dbfs_from_rms(capture.last_raw_left.ac_rms)),
                 capture.last_raw_left.dc_offset,
                 safe_db(audio_metrics_dbfs_from_rms(capture.last_raw_right.ac_rms)),
                 capture.last_raw_right.dc_offset,
                 safe_db(audio_metrics_dbfs_from_rms(capture.last_raw_primary.ac_rms)),
                 capture.last_raw_primary.dc_offset,
                 capture.last_frame_sequence,
                 stream.frames_encoded,
                 stream.frames_dropped_offline,
                 stream.submit_failures,
                 stream.harbor_reconnects,
                 harbor.last_response[0] ? harbor.last_response : "-");
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set("MIC_A1S_ES8388", ESP_LOG_INFO);
    esp_log_level_set("WIFI_STA", ESP_LOG_INFO);
    esp_log_level_set("HARBOR_CLIENT", ESP_LOG_INFO);
    esp_log_level_set("OPUS_OGG", ESP_LOG_INFO);
    esp_log_level_set("SNR_PROBE", ESP_LOG_INFO);

    ESP_LOGI(TAG, "Power-on settle delay: %d ms", POWER_ON_SETTLE_MS);
    vTaskDelay(pdMS_TO_TICKS(POWER_ON_SETTLE_MS));

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_pcm_queue = xQueueCreate(PCM_QUEUE_DEPTH, sizeof(pcm_frame_t));
    ESP_ERROR_CHECK(s_pcm_queue ? ESP_OK : ESP_ERR_NO_MEM);

    load_default_runtime_config(&s_runtime_config);
    load_runtime_config(&s_runtime_config);

    const char *harbor_uri = active_stream_uri(&s_runtime_config, s_harbor_uri_resolved, sizeof(s_harbor_uri_resolved));
    const char *device_name = s_runtime_config.stream_device_name[0] ? s_runtime_config.stream_device_name : "test";
    snprintf(s_harbor_ice_name, sizeof(s_harbor_ice_name), "ESP32 A1S Stereo %s", device_name);
    snprintf(s_harbor_ice_description,
             sizeof(s_harbor_ice_description),
             "ESP32 Audio Kit A1S/A541 ES8388 stereo mic profile=%s at %d Hz",
             MIC_PROFILE_NAME,
             CAPTURE_SAMPLE_RATE_HZ);

    harbor_client_config_t harbor_cfg = {
        .uri = harbor_uri,
        .user = CONFIG_HARBOR_USER,
        .password = active_stream_password(&s_runtime_config),
        .content_type = "audio/ogg",
        .ice_name = s_harbor_ice_name,
        .ice_description = s_harbor_ice_description,
        .sample_rate_hz = CAPTURE_SAMPLE_RATE_HZ,
        .channel_count = STREAM_CHANNEL_COUNT,
        .bitrate_bps = OPUS_BITRATE_BPS,
        .use_source_method = true,
    };
    ESP_ERROR_CHECK(harbor_client_init(&s_harbor, &harbor_cfg));

    opus_ogg_streamer_config_t opus_cfg = {
        .sample_rate_hz = CAPTURE_SAMPLE_RATE_HZ,
        .channel_count = STREAM_CHANNEL_COUNT,
        .bits_per_sample = STREAM_PCM_BITS_PER_SAMPLE,
        .bitrate_bps = OPUS_BITRATE_BPS,
        .complexity = OPUS_COMPLEXITY,
        .warmup_packets = OPUS_WARMUP_PACKETS,
        .enable_fec = 0,
        .enable_dtx = 0,
        .enable_vbr = 0,
    };
    ESP_ERROR_CHECK(opus_ogg_streamer_init(&s_streamer, &opus_cfg));
    ESP_ERROR_CHECK(opus_ogg_streamer_reset_stream(&s_streamer));
#if ENABLE_SNR_PROBE
    snr_probe_init(&s_snr_probe, &s_snr_probe_cfg);
#endif
#if ENABLE_PCM_CONDITIONER
    pcm_conditioner_init(&s_pcm_conditioner, &s_pcm_conditioner_cfg);
#endif

    ESP_LOGI(TAG,
             "Heap view: internal_free=%u internal_total=%u spiram_free=%u spiram_total=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));

    ESP_LOGI(TAG, "Starting A1S stereo station: 48 kHz L/R capture, Opus/Ogg Harbor sender, queue decoupling on");
    xTaskCreatePinnedToCore(capture_task, "capture_task", 12288, NULL, 5, &s_capture_task_handle, 0);
    xTaskCreatePinnedToCore(stream_task, "stream_task", STREAM_TASK_STACK_BYTES, NULL, 5, &s_stream_task_handle, 1);
    xTaskCreatePinnedToCore(wifi_init_task, "wifi_init_task", 6144, NULL, 4, &s_wifi_init_task_handle, 1);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(METRICS_LOG_INTERVAL_MS));
        if (wifi_station_take_factory_reset_request()) {
            ESP_LOGW(TAG, "Factory reset requested by MODE button");
            wifi_station_perform_factory_reset_and_restart();
        }
        if (wifi_station_take_reconfigure_request()) {
            ESP_LOGI(TAG, "Portal saved new runtime config, restarting");
            wifi_station_perform_reconfigure_restart();
        }
        log_runtime_diag();
    }
}
