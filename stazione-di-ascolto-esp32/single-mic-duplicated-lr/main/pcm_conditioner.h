#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_metrics.h"

typedef struct {
    double base_gain;
    double quiet_update_margin_db;
    double expander_full_open_db;
    double expander_max_atten_db;
    double limiter_threshold_dbfs;
    double attack_coeff;
    double release_coeff;
    uint32_t warmup_frames;
} pcm_conditioner_config_t;

typedef struct {
    bool noise_floor_ready;
    uint32_t quiet_frames;
    double noise_floor_rms;
    double current_gain;
    double last_target_gain;
    double last_expander_atten_db;
    bool last_limiter_active;
} pcm_conditioner_state_t;

void pcm_conditioner_init(pcm_conditioner_state_t *state, const pcm_conditioner_config_t *config);
void pcm_conditioner_process_i16(pcm_conditioner_state_t *state,
                                 const pcm_conditioner_config_t *config,
                                 int16_t *samples,
                                 size_t sample_count,
                                 const audio_block_metrics_t *input_metrics);
void pcm_conditioner_get_state(const pcm_conditioner_state_t *state, pcm_conditioner_state_t *snapshot);
