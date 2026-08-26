#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_audio_enc.h"
#include "esp_err.h"

typedef struct {
    int sample_rate_hz;
    int channel_count;
    int bits_per_sample;
    int bitrate_bps;
    int complexity;
    int warmup_packets;
    int enable_fec;
    int enable_dtx;
    int enable_vbr;
} opus_ogg_streamer_config_t;

typedef struct {
    uint32_t packet_count;
    uint32_t warmup_drops;
    uint32_t page_count;
    uint32_t stream_serial;
    int frame_samples;
    int last_packet_bytes;
    int64_t granule_pos;
} opus_ogg_streamer_stats_t;

typedef struct opus_ogg_streamer {
    opus_ogg_streamer_config_t cfg;
    esp_audio_enc_handle_t enc_hd;
    esp_audio_enc_frame_info_t frame_info;
    esp_audio_enc_info_t enc_info;
    uint8_t *pcm_buf;
    uint8_t *opus_buf;
    uint8_t *ogg_buf;
    int pcm_fill;
    bool headers_sent;
    opus_ogg_streamer_stats_t stats;
} opus_ogg_streamer_t;

typedef esp_err_t (*opus_ogg_page_write_cb_t)(void *ctx, const uint8_t *data, size_t len);

esp_err_t opus_ogg_streamer_init(opus_ogg_streamer_t *streamer, const opus_ogg_streamer_config_t *config);
void opus_ogg_streamer_deinit(opus_ogg_streamer_t *streamer);
esp_err_t opus_ogg_streamer_reset_stream(opus_ogg_streamer_t *streamer);
int opus_ogg_streamer_frame_samples(const opus_ogg_streamer_t *streamer);
esp_err_t opus_ogg_streamer_submit(opus_ogg_streamer_t *streamer,
                                   const int16_t *samples,
                                   size_t sample_count,
                                   opus_ogg_page_write_cb_t callback,
                                   void *callback_ctx);
void opus_ogg_streamer_get_stats(const opus_ogg_streamer_t *streamer, opus_ogg_streamer_stats_t *stats);
