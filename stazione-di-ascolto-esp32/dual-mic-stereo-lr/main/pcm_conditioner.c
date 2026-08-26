#include "pcm_conditioner.h"

#include <math.h>
#include <string.h>

static double clamp01(double value)
{
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 1.0) {
        return 1.0;
    }
    return value;
}

static double db_to_ratio(double db)
{
    return pow(10.0, db / 20.0);
}

static void update_noise_floor(pcm_conditioner_state_t *state,
                               const pcm_conditioner_config_t *config,
                               const audio_block_metrics_t *input_metrics)
{
    if (!state || !config || !input_metrics || input_metrics->ac_rms <= 0.0) {
        return;
    }

    if (!state->noise_floor_ready) {
        if (state->quiet_frames == 0) {
            state->noise_floor_rms = input_metrics->ac_rms;
        } else {
            state->noise_floor_rms =
                (state->noise_floor_rms * (double)state->quiet_frames + input_metrics->ac_rms) /
                (double)(state->quiet_frames + 1);
        }
        state->quiet_frames++;
        if (state->quiet_frames >= config->warmup_frames) {
            state->noise_floor_ready = true;
        }
        return;
    }

    double quiet_limit = state->noise_floor_rms * db_to_ratio(config->quiet_update_margin_db);
    if (input_metrics->ac_rms > quiet_limit) {
        return;
    }

    state->quiet_frames++;
    state->noise_floor_rms = (state->noise_floor_rms * 0.97) + (input_metrics->ac_rms * 0.03);
}

void pcm_conditioner_init(pcm_conditioner_state_t *state, const pcm_conditioner_config_t *config)
{
    if (!state) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->current_gain = (config && config->base_gain > 0.0) ? config->base_gain : 1.0;
    state->last_target_gain = state->current_gain;
}

void pcm_conditioner_process_i16(pcm_conditioner_state_t *state,
                                 const pcm_conditioner_config_t *config,
                                 int16_t *samples,
                                 size_t sample_count,
                                 const audio_block_metrics_t *input_metrics)
{
    if (!state || !config || !samples || sample_count == 0 || !input_metrics) {
        return;
    }

    update_noise_floor(state, config, input_metrics);

    double target_gain = config->base_gain > 0.0 ? config->base_gain : 1.0;
    double expander_atten_db = 0.0;
    bool limiter_active = false;

    if (state->noise_floor_ready && state->noise_floor_rms > 0.0 && input_metrics->ac_rms > 0.0) {
        double delta_db = 20.0 * log10(input_metrics->ac_rms / state->noise_floor_rms);
        if (delta_db < config->expander_full_open_db) {
            double t = clamp01(delta_db / config->expander_full_open_db);
            double shape = t * t;
            expander_atten_db = (1.0 - shape) * config->expander_max_atten_db;
            target_gain *= db_to_ratio(-expander_atten_db);
        }
    }

    if (input_metrics->ac_peak_abs > 0) {
        double limiter_threshold = db_to_ratio(config->limiter_threshold_dbfs) * 32767.0;
        double projected_peak = (double)input_metrics->ac_peak_abs * target_gain;
        if (projected_peak > limiter_threshold && limiter_threshold > 0.0) {
            target_gain *= limiter_threshold / projected_peak;
            limiter_active = true;
        }
    }

    double coeff = (target_gain < state->current_gain) ? config->attack_coeff : config->release_coeff;
    coeff = clamp01(coeff);
    state->current_gain += (target_gain - state->current_gain) * coeff;
    state->last_target_gain = target_gain;
    state->last_expander_atten_db = expander_atten_db;
    state->last_limiter_active = limiter_active;

    for (size_t i = 0; i < sample_count; ++i) {
        double scaled = (double)samples[i] * state->current_gain;
        if (scaled > 32767.0) {
            scaled = 32767.0;
        } else if (scaled < -32768.0) {
            scaled = -32768.0;
        }
        samples[i] = (int16_t)lround(scaled);
    }
}

void pcm_conditioner_get_state(const pcm_conditioner_state_t *state, pcm_conditioner_state_t *snapshot)
{
    if (!state || !snapshot) {
        return;
    }
    *snapshot = *state;
}
