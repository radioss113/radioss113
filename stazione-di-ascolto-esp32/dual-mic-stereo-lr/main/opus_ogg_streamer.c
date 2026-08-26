#include "opus_ogg_streamer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_audio_enc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_opus_enc.h"
#include "esp_random.h"

typedef int16_t opus_int16;
typedef int32_t opus_int32;

extern opus_int32 opus_packet_get_nb_samples(const unsigned char packet[], opus_int32 len, opus_int32 Fs);

#define OPUS_PAGE_MAX_BYTES 2048
#define OPUS_HEAD_PRESKIP    312

static const char *TAG = "OPUS_OGG";

static void put_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xff);
    dst[1] = (uint8_t)((value >> 8) & 0xff);
}

static void put_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xff);
    dst[1] = (uint8_t)((value >> 8) & 0xff);
    dst[2] = (uint8_t)((value >> 16) & 0xff);
    dst[3] = (uint8_t)((value >> 24) & 0xff);
}

static void put_u64_le(uint8_t *dst, uint64_t value)
{
    dst[0] = (uint8_t)(value & 0xff);
    dst[1] = (uint8_t)((value >> 8) & 0xff);
    dst[2] = (uint8_t)((value >> 16) & 0xff);
    dst[3] = (uint8_t)((value >> 24) & 0xff);
    dst[4] = (uint8_t)((value >> 32) & 0xff);
    dst[5] = (uint8_t)((value >> 40) & 0xff);
    dst[6] = (uint8_t)((value >> 48) & 0xff);
    dst[7] = (uint8_t)((value >> 56) & 0xff);
}

static int build_opus_head_packet(opus_ogg_streamer_t *streamer, uint8_t *dst, size_t cap)
{
    if (cap < 19) {
        return 0;
    }
    memcpy(dst, "OpusHead", 8);
    dst[8] = 1;
    dst[9] = (uint8_t)streamer->cfg.channel_count;
    put_u16_le(dst + 10, OPUS_HEAD_PRESKIP);
    put_u32_le(dst + 12, (uint32_t)streamer->cfg.sample_rate_hz);
    put_u16_le(dst + 16, 0);
    dst[18] = 0;
    return 19;
}

static int build_opus_tags_packet(uint8_t *dst, size_t cap)
{
    static const char vendor[] = "ESP32_A1S_ES8388";
    uint32_t vendor_len = (uint32_t)(sizeof(vendor) - 1);
    size_t total = 8 + 4 + vendor_len + 4;
    if (cap < total) {
        return 0;
    }
    memcpy(dst, "OpusTags", 8);
    put_u32_le(dst + 8, vendor_len);
    memcpy(dst + 12, vendor, vendor_len);
    put_u32_le(dst + 12 + vendor_len, 0);
    return (int)total;
}

static uint32_t ogg_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint32_t)data[i] << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U) ? ((crc << 1) ^ 0x04c11db7U) : (crc << 1);
        }
    }
    return crc;
}

static int ogg_lacing_count(int packet_len)
{
    if (packet_len < 0) {
        return 0;
    }
    return (packet_len / 255) + 1;
}

static int build_ogg_page(uint8_t *dst, size_t cap, uint8_t header_type, uint64_t granule_pos,
                          uint32_t stream_serial, uint32_t page_seqno,
                          const uint8_t *packet, int packet_len)
{
    if (!dst || !packet || packet_len < 0) {
        return 0;
    }

    int seg_count = ogg_lacing_count(packet_len);
    int page_len = 27 + seg_count + packet_len;
    if ((size_t)page_len > cap) {
        return 0;
    }

    memset(dst, 0, (size_t)page_len);
    memcpy(dst, "OggS", 4);
    dst[4] = 0;
    dst[5] = header_type;
    put_u64_le(dst + 6, granule_pos);
    put_u32_le(dst + 14, stream_serial);
    put_u32_le(dst + 18, page_seqno);
    put_u32_le(dst + 22, 0);
    dst[26] = (uint8_t)seg_count;

    int remaining = packet_len;
    for (int i = 0; i < seg_count; ++i) {
        if (remaining >= 255) {
            dst[27 + i] = 255;
            remaining -= 255;
        } else {
            dst[27 + i] = (uint8_t)remaining;
            remaining = 0;
        }
    }
    memcpy(dst + 27 + seg_count, packet, (size_t)packet_len);
    put_u32_le(dst + 22, ogg_crc32(dst, (size_t)page_len));
    return page_len;
}

static esp_err_t emit_page(opus_ogg_streamer_t *streamer,
                           uint8_t header_type,
                           uint64_t granule_pos,
                           const uint8_t *packet,
                           int packet_len,
                           opus_ogg_page_write_cb_t callback,
                           void *callback_ctx)
{
    int page_len = build_ogg_page(streamer->ogg_buf,
                                  OPUS_PAGE_MAX_BYTES,
                                  header_type,
                                  granule_pos,
                                  streamer->stats.stream_serial,
                                  streamer->stats.page_count,
                                  packet,
                                  packet_len);
    if (page_len <= 0) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(callback(callback_ctx, streamer->ogg_buf, (size_t)page_len), TAG, "page write failed");
    streamer->stats.page_count++;
    return ESP_OK;
}

static esp_err_t emit_headers(opus_ogg_streamer_t *streamer,
                              opus_ogg_page_write_cb_t callback,
                              void *callback_ctx)
{
    uint8_t packet[128];
    int packet_len = build_opus_head_packet(streamer, packet, sizeof(packet));
    ESP_RETURN_ON_FALSE(packet_len > 0, ESP_FAIL, TAG, "OpusHead build failed");
    ESP_RETURN_ON_ERROR(emit_page(streamer, 0x02, 0, packet, packet_len, callback, callback_ctx), TAG, "OpusHead emit failed");

    packet_len = build_opus_tags_packet(packet, sizeof(packet));
    ESP_RETURN_ON_FALSE(packet_len > 0, ESP_FAIL, TAG, "OpusTags build failed");
    ESP_RETURN_ON_ERROR(emit_page(streamer, 0x00, 0, packet, packet_len, callback, callback_ctx), TAG, "OpusTags emit failed");
    streamer->headers_sent = true;
    return ESP_OK;
}

esp_err_t opus_ogg_streamer_init(opus_ogg_streamer_t *streamer, const opus_ogg_streamer_config_t *config)
{
    ESP_RETURN_ON_FALSE(streamer && config, ESP_ERR_INVALID_ARG, TAG, "invalid args");
    memset(streamer, 0, sizeof(*streamer));
    streamer->cfg = *config;

    esp_opus_enc_config_t cfg = (esp_opus_enc_config_t)ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = config->sample_rate_hz;
    cfg.channel = config->channel_count;
    cfg.bits_per_sample = config->bits_per_sample;
    cfg.bitrate = config->bitrate_bps;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity = config->complexity;
    cfg.enable_fec = config->enable_fec;
    cfg.enable_dtx = config->enable_dtx;
    cfg.enable_vbr = config->enable_vbr;

    esp_audio_err_t ret = esp_opus_enc_open(&cfg, sizeof(cfg), &streamer->enc_hd);
    ESP_RETURN_ON_FALSE(ret == ESP_AUDIO_ERR_OK && streamer->enc_hd, ESP_FAIL, TAG, "esp_opus_enc_open failed: %d", ret);

    ret = esp_opus_enc_get_frame_size(streamer->enc_hd, &streamer->frame_info.in_frame_size, &streamer->frame_info.out_frame_size);
    ESP_RETURN_ON_FALSE(ret == ESP_AUDIO_ERR_OK, ESP_FAIL, TAG, "esp_opus_enc_get_frame_size failed: %d", ret);
    ret = esp_opus_enc_get_info(streamer->enc_hd, &streamer->enc_info);
    ESP_RETURN_ON_FALSE(ret == ESP_AUDIO_ERR_OK, ESP_FAIL, TAG, "esp_opus_enc_get_info failed: %d", ret);

    streamer->pcm_buf = malloc(streamer->frame_info.in_frame_size);
    streamer->opus_buf = malloc(streamer->frame_info.out_frame_size);
    streamer->ogg_buf = malloc(OPUS_PAGE_MAX_BYTES);
    ESP_RETURN_ON_FALSE(streamer->pcm_buf && streamer->opus_buf && streamer->ogg_buf, ESP_ERR_NO_MEM, TAG, "encoder buffers alloc failed");

    streamer->stats.frame_samples = streamer->frame_info.in_frame_size / (config->bits_per_sample / 8);
    streamer->stats.stream_serial = esp_random();
    if (streamer->stats.stream_serial == 0) {
        streamer->stats.stream_serial = 1;
    }

    ESP_LOGI(TAG, "Opus/Ogg ready: in=%d out=%d frame_samples=%d serial=%u",
             streamer->frame_info.in_frame_size,
             streamer->frame_info.out_frame_size,
             streamer->stats.frame_samples,
             streamer->stats.stream_serial);
    return ESP_OK;
}

void opus_ogg_streamer_deinit(opus_ogg_streamer_t *streamer)
{
    if (!streamer) {
        return;
    }
    if (streamer->enc_hd) {
        esp_opus_enc_close(streamer->enc_hd);
        streamer->enc_hd = NULL;
    }
    free(streamer->pcm_buf);
    free(streamer->opus_buf);
    free(streamer->ogg_buf);
    streamer->pcm_buf = NULL;
    streamer->opus_buf = NULL;
    streamer->ogg_buf = NULL;
}

esp_err_t opus_ogg_streamer_reset_stream(opus_ogg_streamer_t *streamer)
{
    ESP_RETURN_ON_FALSE(streamer, ESP_ERR_INVALID_ARG, TAG, "invalid streamer");
    uint32_t next_serial = streamer->stats.stream_serial + 1u;
    if (next_serial == 0) {
        next_serial = 1;
    }

    streamer->pcm_fill = 0;
    streamer->headers_sent = false;
    streamer->stats.packet_count = 0;
    streamer->stats.warmup_drops = 0;
    streamer->stats.page_count = 0;
    streamer->stats.last_packet_bytes = 0;
    streamer->stats.granule_pos = 0;
    streamer->stats.stream_serial = next_serial;
    ESP_LOGI(TAG, "Reset Ogg stream: serial=%u", streamer->stats.stream_serial);
    return ESP_OK;
}

int opus_ogg_streamer_frame_samples(const opus_ogg_streamer_t *streamer)
{
    if (!streamer) {
        return 0;
    }
    return streamer->stats.frame_samples;
}

esp_err_t opus_ogg_streamer_submit(opus_ogg_streamer_t *streamer,
                                   const int16_t *samples,
                                   size_t sample_count,
                                   opus_ogg_page_write_cb_t callback,
                                   void *callback_ctx)
{
    ESP_RETURN_ON_FALSE(streamer && samples && callback, ESP_ERR_INVALID_ARG, TAG, "invalid submit args");

    size_t bytes = sample_count * sizeof(int16_t);
    size_t copied = 0;
    while (copied < bytes) {
        size_t room = (size_t)streamer->frame_info.in_frame_size - (size_t)streamer->pcm_fill;
        size_t chunk = bytes - copied;
        if (chunk > room) {
            chunk = room;
        }
        memcpy(streamer->pcm_buf + streamer->pcm_fill, ((const uint8_t *)samples) + copied, chunk);
        streamer->pcm_fill += (int)chunk;
        copied += chunk;

        if (streamer->pcm_fill < streamer->frame_info.in_frame_size) {
            continue;
        }

        esp_audio_enc_in_frame_t in_frame = {
            .buffer = streamer->pcm_buf,
            .len = (uint32_t)streamer->frame_info.in_frame_size,
        };
        esp_audio_enc_out_frame_t out_frame = {
            .buffer = streamer->opus_buf,
            .len = (uint32_t)streamer->frame_info.out_frame_size,
            .encoded_bytes = 0,
            .pts = 0,
        };

        esp_audio_err_t ret = esp_opus_enc_process(streamer->enc_hd, &in_frame, &out_frame);
        streamer->pcm_fill = 0;
        ESP_RETURN_ON_FALSE(ret == ESP_AUDIO_ERR_OK && out_frame.encoded_bytes > 0,
                            ESP_FAIL, TAG, "esp_opus_enc_process failed: %d encoded=%u", ret, (unsigned)out_frame.encoded_bytes);

        streamer->stats.packet_count++;
        streamer->stats.last_packet_bytes = (int)out_frame.encoded_bytes;

        if (!streamer->headers_sent && (int)streamer->stats.packet_count <= streamer->cfg.warmup_packets) {
            streamer->stats.warmup_drops++;
            continue;
        }

        if (!streamer->headers_sent) {
            ESP_RETURN_ON_ERROR(emit_headers(streamer, callback, callback_ctx), TAG, "emit headers failed");
        }

        opus_int32 nb_samples = opus_packet_get_nb_samples(streamer->opus_buf,
                                                           (opus_int32)out_frame.encoded_bytes,
                                                           (opus_int32)streamer->cfg.sample_rate_hz);
        ESP_RETURN_ON_FALSE(nb_samples > 0, ESP_FAIL, TAG, "opus_packet_get_nb_samples failed: %ld", (long)nb_samples);
        streamer->stats.granule_pos += nb_samples;

        ESP_RETURN_ON_ERROR(emit_page(streamer,
                                      0x00,
                                      (uint64_t)streamer->stats.granule_pos,
                                      streamer->opus_buf,
                                      (int)out_frame.encoded_bytes,
                                      callback,
                                      callback_ctx),
                            TAG,
                            "emit data page failed");
    }

    return ESP_OK;
}

void opus_ogg_streamer_get_stats(const opus_ogg_streamer_t *streamer, opus_ogg_streamer_stats_t *stats)
{
    if (!streamer || !stats) {
        return;
    }
    *stats = streamer->stats;
}
