#include "snr_probe.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "SNR_PROBE";

static double db_to_ratio(double db)
{
    return pow(10.0, db / 20.0);
}

static double safe_db(double value)
{
    return audio_metrics_dbfs_from_rms(value);
}

static void update_noise_floor(snr_probe_state_t *state,
                               const snr_probe_config_t *config,
                               const audio_block_metrics_t *metrics)
{
    if (!state || !config || !metrics || metrics->ac_rms <= 0.0) {
        return;
    }

    if (!state->noise_floor_ready) {
        if (state->quiet_frames == 0) {
            state->noise_floor_rms = metrics->ac_rms;
        } else {
            state->noise_floor_rms =
                (state->noise_floor_rms * (double)state->quiet_frames + metrics->ac_rms) /
                (double)(state->quiet_frames + 1);
        }
        state->quiet_frames++;
        if (state->quiet_frames >= config->warmup_quiet_frames) {
            state->noise_floor_ready = true;
            ESP_LOGI(TAG, "Noise floor ready: %.1f dBFS after %u quiet frames",
                     safe_db(state->noise_floor_rms),
                     state->quiet_frames);
        }
        return;
    }

    double quiet_limit = state->noise_floor_rms * db_to_ratio(config->quiet_update_margin_db);
    if (metrics->ac_rms > quiet_limit) {
        return;
    }

    state->noise_floor_rms = (state->noise_floor_rms * 0.95) + (metrics->ac_rms * 0.05);
    state->quiet_frames++;
}

static void reset_event(snr_probe_state_t *state)
{
    state->event_active = false;
    state->event_frames = 0;
    state->release_frames = 0;
    state->candidate_frames = 0;
    state->event_power_sum = 0.0;
    state->event_peak_abs = 0;
    state->event_clipped_samples = 0;
}

static void log_event_summary(snr_probe_state_t *state,
                              const audio_block_metrics_t *metrics,
                              int sample_rate_hz)
{
    if (!state || !metrics || state->event_frames == 0) {
        return;
    }

    double signal_rms = sqrt(state->event_power_sum / (double)state->event_frames);
    double snr_db = -INFINITY;
    if (state->noise_floor_rms > 0.0 && signal_rms > 0.0) {
        snr_db = 20.0 * log10(signal_rms / state->noise_floor_rms);
    }

    uint32_t duration_ms = 0;
    if (sample_rate_hz > 0) {
        uint64_t total_samples = (uint64_t)metrics->sample_count * (uint64_t)state->event_frames;
        duration_ms = (uint32_t)((total_samples * 1000ULL) / (uint64_t)sample_rate_hz);
    }

    state->last_event_valid = true;
    state->last_event_duration_ms = duration_ms;
    state->last_signal_rms = signal_rms;
    state->last_snr_db = snr_db;
    state->last_peak_abs = state->event_peak_abs;
    state->last_clipped_samples = state->event_clipped_samples;

    ESP_LOGI(TAG,
             "Event summary: dur=%ums noise=%.1f dBFS signal=%.1f dBFS snr=%.1f dB peak=%.1f dBFS clip=%u",
             duration_ms,
             safe_db(state->noise_floor_rms),
             safe_db(signal_rms),
             snr_db,
             audio_metrics_dbfs_from_peak(state->event_peak_abs),
             state->event_clipped_samples);
}

void snr_probe_init(snr_probe_state_t *state, const snr_probe_config_t *config)
{
    (void)config;
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

void snr_probe_process_frame(snr_probe_state_t *state,
                             const snr_probe_config_t *config,
                             const audio_block_metrics_t *metrics,
                             int sample_rate_hz)
{
    if (!state || !config || !metrics || metrics->sample_count == 0) {
        return;
    }

    update_noise_floor(state, config, metrics);
    if (!state->noise_floor_ready || state->noise_floor_rms <= 0.0) {
        return;
    }

    double trigger_rms = state->noise_floor_rms * db_to_ratio(config->trigger_margin_db);
    double release_rms = state->noise_floor_rms * db_to_ratio(config->release_margin_db);

    if (!state->event_active) {
        if (metrics->ac_rms >= trigger_rms) {
            state->candidate_frames++;
            if (state->candidate_frames >= 2) {
                state->event_active = true;
                state->event_frames = 0;
                state->release_frames = 0;
                state->event_power_sum = 0.0;
                state->event_peak_abs = 0;
                state->event_clipped_samples = 0;
                ESP_LOGI(TAG,
                         "Event start: frame_rms=%.1f dBFS noise=%.1f dBFS threshold=%.1f dBFS",
                         safe_db(metrics->ac_rms),
                         safe_db(state->noise_floor_rms),
                         safe_db(trigger_rms));
            } else {
                return;
            }
        } else {
            state->candidate_frames = 0;
            return;
        }
    }

    state->event_frames++;
    state->event_power_sum += metrics->ac_rms * metrics->ac_rms;
    if (metrics->ac_peak_abs > state->event_peak_abs) {
        state->event_peak_abs = metrics->ac_peak_abs;
    }
    state->event_clipped_samples += metrics->clipped_samples;

    if (metrics->ac_rms < release_rms) {
        state->release_frames++;
    } else {
        state->release_frames = 0;
    }

    if (state->event_frames >= config->max_event_frames ||
        (state->event_frames >= config->min_event_frames &&
         state->release_frames >= config->release_quiet_frames)) {
        log_event_summary(state, metrics, sample_rate_hz);
        reset_event(state);
    }
}

void snr_probe_get_state(const snr_probe_state_t *state, snr_probe_state_t *snapshot)
{
    if (!state || !snapshot) {
        return;
    }
    *snapshot = *state;
}
