#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    float r;
    float prev_x;
    float prev_y;
} pcm_dc_blocker_t;

void pcm_dc_blocker_init(pcm_dc_blocker_t *state, int sample_rate_hz, float cutoff_hz);
void pcm_dc_blocker_process_i16(pcm_dc_blocker_t *state, int16_t *samples, size_t sample_count);
void pcm_dc_blocker_process_interleaved_i16(pcm_dc_blocker_t *states,
                                            size_t channel_count,
                                            int16_t *samples,
                                            size_t sample_count);
