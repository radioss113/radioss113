#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_metrics.h"

typedef struct {
    uint32_t warmup_quiet_frames;
    uint32_t min_event_frames;
    uint32_t max_event_frames;
    uint32_t release_quiet_frames;
    double trigger_margin_db;
    double release_margin_db;
    double quiet_update_margin_db;
} snr_probe_config_t;

typedef struct {
    bool noise_floor_ready;
    double noise_floor_rms;
    uint32_t quiet_frames;
    bool event_active;
    uint32_t event_frames;
    uint32_t release_frames;
    uint32_t candidate_frames;
    double event_power_sum;
    int32_t event_peak_abs;
    uint32_t event_clipped_samples;
    bool last_event_valid;
    uint32_t last_event_duration_ms;
    double last_signal_rms;
    double last_snr_db;
    int32_t last_peak_abs;
    uint32_t last_clipped_samples;
} snr_probe_state_t;

void snr_probe_init(snr_probe_state_t *state, const snr_probe_config_t *config);
void snr_probe_process_frame(snr_probe_state_t *state,
                             const snr_probe_config_t *config,
                             const audio_block_metrics_t *metrics,
                             int sample_rate_hz);
void snr_probe_get_state(const snr_probe_state_t *state, snr_probe_state_t *snapshot);
