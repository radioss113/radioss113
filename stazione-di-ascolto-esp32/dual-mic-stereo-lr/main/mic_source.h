#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_metrics.h"
#include "esp_err.h"

typedef struct {
    const char *profile_name;
    int sample_rate_hz;
    int bits_per_sample;
    size_t dma_frame_samples;
    int read_timeout_ms;
    int es8388_mic_gain_db;
    int primary_mode;
} mic_source_config_t;

typedef enum {
    MIC_SOURCE_PRIMARY_RIGHT = 0,
    MIC_SOURCE_PRIMARY_DIFF_R_MINUS_L = 1,
    MIC_SOURCE_PRIMARY_RIGHT_MINUS_HALF_L = 2,
    MIC_SOURCE_PRIMARY_STEREO_RAW = 3,
} mic_source_primary_mode_t;

typedef struct {
    size_t raw_frames_read;
    size_t primary_samples;
    bool has_reference;
    bool has_lane_metrics;
    bool has_word_metrics;
    audio_block_metrics_t primary_metrics;
    audio_block_metrics_t reference_metrics;
    audio_block_metrics_t left_hi_metrics;
    audio_block_metrics_t left_lo_metrics;
    audio_block_metrics_t right_hi_metrics;
    audio_block_metrics_t right_lo_metrics;
    uint32_t left_pad8_nonzero_samples;
    uint32_t right_pad8_nonzero_samples;
    uint32_t left_residual8_nonzero_samples;
    uint32_t right_residual8_nonzero_samples;
    double left_residual8_mean;
    double right_residual8_mean;
} mic_source_read_info_t;

typedef struct mic_source mic_source_t;

typedef struct {
    const char *name;
    esp_err_t (*init)(mic_source_t *self, const mic_source_config_t *config);
    esp_err_t (*deinit)(mic_source_t *self);
    int (*read)(mic_source_t *self,
                int16_t *primary_out,
                size_t primary_capacity_samples,
                mic_source_read_info_t *info);
    const char *(*describe)(mic_source_t *self);
} mic_source_vtable_t;

struct mic_source {
    const mic_source_vtable_t *vtable;
    void *ctx;
};

esp_err_t mic_source_init(mic_source_t *source, const mic_source_config_t *config);
esp_err_t mic_source_deinit(mic_source_t *source);
int mic_source_read(mic_source_t *source,
                    int16_t *primary_out,
                    size_t primary_capacity_samples,
                    mic_source_read_info_t *info);
const char *mic_source_describe(mic_source_t *source);

mic_source_t mic_source_lyrat_mini_onboard_create(void);
mic_source_t mic_source_a1s_es8388_stereo_create(void);
