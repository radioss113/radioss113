#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t sample_count;
    int16_t min_sample;
    int16_t max_sample;
    int32_t peak_abs;
    int32_t ac_peak_abs;
    double mean_abs;
    double rms;
    double ac_rms;
    double dc_offset;
    uint32_t clipped_samples;
} audio_block_metrics_t;

void audio_metrics_reset(audio_block_metrics_t *metrics);
void audio_metrics_compute_i16(const int16_t *samples, size_t count, audio_block_metrics_t *metrics);
double audio_metrics_dbfs_from_peak(int32_t peak_abs);
double audio_metrics_dbfs_from_rms(double rms);
