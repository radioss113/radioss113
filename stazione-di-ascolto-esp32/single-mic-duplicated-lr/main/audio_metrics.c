#include "audio_metrics.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

void audio_metrics_reset(audio_block_metrics_t *metrics)
{
    if (!metrics) {
        return;
    }

    metrics->sample_count = 0;
    metrics->min_sample = 0;
    metrics->max_sample = 0;
    metrics->peak_abs = 0;
    metrics->ac_peak_abs = 0;
    metrics->mean_abs = 0.0;
    metrics->rms = 0.0;
    metrics->ac_rms = 0.0;
    metrics->dc_offset = 0.0;
    metrics->clipped_samples = 0;
}

void audio_metrics_compute_i16(const int16_t *samples, size_t count, audio_block_metrics_t *metrics)
{
    audio_metrics_reset(metrics);
    if (!samples || !metrics || count == 0) {
        return;
    }

    int16_t min_sample = INT16_MAX;
    int16_t max_sample = INT16_MIN;
    int32_t peak_abs = 0;
    uint32_t clipped = 0;
    double sum = 0.0;
    double sum_abs = 0.0;
    double sum_sq = 0.0;

    for (size_t i = 0; i < count; ++i) {
        int16_t sample = samples[i];
        int32_t abs_sample = sample < 0 ? -(int32_t)sample : (int32_t)sample;

        if (sample < min_sample) {
            min_sample = sample;
        }
        if (sample > max_sample) {
            max_sample = sample;
        }
        if (abs_sample > peak_abs) {
            peak_abs = abs_sample;
        }
        if (sample == INT16_MIN || sample == INT16_MAX) {
            ++clipped;
        }

        sum += sample;
        sum_abs += abs_sample;
        sum_sq += (double)sample * (double)sample;
    }

    metrics->sample_count = count;
    metrics->min_sample = min_sample;
    metrics->max_sample = max_sample;
    metrics->peak_abs = peak_abs;
    metrics->mean_abs = sum_abs / (double)count;
    metrics->rms = sqrt(sum_sq / (double)count);
    metrics->dc_offset = sum / (double)count;
    metrics->clipped_samples = clipped;

    double centered_sum_sq = 0.0;
    int32_t centered_peak_abs = 0;
    for (size_t i = 0; i < count; ++i) {
        double centered = (double)samples[i] - metrics->dc_offset;
        double centered_abs = fabs(centered);
        if (centered_abs > (double)centered_peak_abs) {
            centered_peak_abs = (int32_t)lround(centered_abs);
        }
        centered_sum_sq += centered * centered;
    }
    metrics->ac_peak_abs = centered_peak_abs;
    metrics->ac_rms = sqrt(centered_sum_sq / (double)count);
}

double audio_metrics_dbfs_from_peak(int32_t peak_abs)
{
    if (peak_abs <= 0) {
        return -INFINITY;
    }
    return 20.0 * log10((double)peak_abs / (double)INT16_MAX);
}

double audio_metrics_dbfs_from_rms(double rms)
{
    if (rms <= 0.0) {
        return -INFINITY;
    }
    return 20.0 * log10(rms / (double)INT16_MAX);
}
