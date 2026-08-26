#include "pcm_dc_blocker.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int16_t clamp_i16(float value)
{
    if (value > 32767.0f) {
        return 32767;
    }
    if (value < -32768.0f) {
        return -32768;
    }
    return (int16_t)value;
}

void pcm_dc_blocker_init(pcm_dc_blocker_t *state, int sample_rate_hz, float cutoff_hz)
{
    if (!state) {
        return;
    }

    if (sample_rate_hz <= 0) {
        sample_rate_hz = 48000;
    }
    if (cutoff_hz <= 0.0f) {
        cutoff_hz = 20.0f;
    }

    state->r = expf((-2.0f * (float)M_PI * cutoff_hz) / (float)sample_rate_hz);
    state->prev_x = 0.0f;
    state->prev_y = 0.0f;
}

void pcm_dc_blocker_process_i16(pcm_dc_blocker_t *state, int16_t *samples, size_t sample_count)
{
    if (!state || !samples) {
        return;
    }

    for (size_t i = 0; i < sample_count; ++i) {
        float x = (float)samples[i];
        float y = x - state->prev_x + (state->r * state->prev_y);
        state->prev_x = x;
        state->prev_y = y;
        samples[i] = clamp_i16(y);
    }
}

void pcm_dc_blocker_process_interleaved_i16(pcm_dc_blocker_t *states,
                                            size_t channel_count,
                                            int16_t *samples,
                                            size_t sample_count)
{
    if (!states || !samples || channel_count == 0) {
        return;
    }

    for (size_t i = 0; i < sample_count; ++i) {
        pcm_dc_blocker_t *state = &states[i % channel_count];
        float x = (float)samples[i];
        float y = x - state->prev_x + (state->r * state->prev_y);
        state->prev_x = x;
        state->prev_y = y;
        samples[i] = clamp_i16(y);
    }
}
