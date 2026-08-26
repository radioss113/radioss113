#include "mic_source.h"

#include <string.h>

#include "audio_element.h"
#include "audio_hal.h"
#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "es8388.h"
#include "i2s_stream.h"

typedef struct {
    mic_source_config_t cfg;
    audio_hal_handle_t codec;
    audio_element_handle_t reader;
    int16_t stereo_samples[1024];
    int16_t left_samples[512];
    int16_t right_samples[512];
    uint32_t metrics_decimator;
    char description[192];
} a1s_es8388_stereo_ctx_t;

static const char *TAG = "MIC_A1S_ES8388";
static a1s_es8388_stereo_ctx_t s_ctx;

static audio_hal_iface_samples_t map_hal_sample_rate(int sample_rate_hz)
{
    switch (sample_rate_hz) {
        case 8000:
            return AUDIO_HAL_08K_SAMPLES;
        case 11025:
            return AUDIO_HAL_11K_SAMPLES;
        case 16000:
            return AUDIO_HAL_16K_SAMPLES;
        case 22050:
            return AUDIO_HAL_22K_SAMPLES;
        case 24000:
            return AUDIO_HAL_24K_SAMPLES;
        case 32000:
            return AUDIO_HAL_32K_SAMPLES;
        case 44100:
            return AUDIO_HAL_44K_SAMPLES;
        case 48000:
            return AUDIO_HAL_48K_SAMPLES;
        default:
            ESP_LOGW(TAG, "Unsupported HAL sample rate request %d Hz, falling back to 48 kHz", sample_rate_hz);
            return AUDIO_HAL_48K_SAMPLES;
    }
}

static esp_err_t set_es8388_pga_gain_db(int gain_db)
{
    if (gain_db < 0) {
        gain_db = 0;
    }
    if (gain_db > 24) {
        gain_db = 24;
    }
    gain_db = (gain_db / 3) * 3;
    uint8_t gain_n = (uint8_t)(gain_db / 3);
    uint8_t reg = (uint8_t)((gain_n << 4) | gain_n);
    return es8388_write_reg(ES8388_ADCCONTROL1, reg);
}

static esp_err_t configure_es8388_stereo_mic_path(int gain_db)
{
    esp_err_t ret = ESP_OK;

    ret |= es8388_write_reg(ES8388_CONTROL1, 0x17);
    ret |= es8388_write_reg(ES8388_ADCPOWER, 0xff);
    ret |= es8388_write_reg(ES8388_ADCCONTROL2, ADC_INPUT_LINPUT2_RINPUT2);
    ret |= es8388_write_reg(ES8388_ADCCONTROL3, 0x02);
    ret |= es8388_write_reg(ES8388_ADCCONTROL4, 0x0c);
    ret |= es8388_write_reg(ES8388_ADCCONTROL5, 0x02);
    ret |= set_es8388_pga_gain_db(gain_db);
    ret |= es8388_write_reg(ES8388_ADCCONTROL8, 0x00);
    ret |= es8388_write_reg(ES8388_ADCCONTROL9, 0x00);
    ret |= es8388_write_reg(ES8388_ADCPOWER, 0x00);

    return ret;
}

static esp_err_t a1s_es8388_stereo_init(mic_source_t *self, const mic_source_config_t *config)
{
    ESP_RETURN_ON_FALSE(self && config, ESP_ERR_INVALID_ARG, TAG, "invalid args");

    esp_err_t ret = ESP_OK;
    a1s_es8388_stereo_ctx_t *ctx = (a1s_es8388_stereo_ctx_t *)self->ctx;
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = *config;

    audio_hal_codec_config_t codec_cfg = AUDIO_CODEC_DEFAULT_CONFIG();
    codec_cfg.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;
    codec_cfg.adc_input = AUDIO_HAL_ADC_INPUT_LINE2;
    codec_cfg.i2s_iface.mode = AUDIO_HAL_MODE_SLAVE;
    codec_cfg.i2s_iface.fmt = AUDIO_HAL_I2S_NORMAL;
    codec_cfg.i2s_iface.samples = map_hal_sample_rate(config->sample_rate_hz);
    codec_cfg.i2s_iface.bits = AUDIO_HAL_BIT_LENGTH_16BITS;

    ctx->codec = audio_hal_init(&codec_cfg, &AUDIO_CODEC_ES8388_DEFAULT_HANDLE);
    ESP_RETURN_ON_FALSE(ctx->codec != NULL, ESP_FAIL, TAG, "ES8388 codec init failed");

    ESP_GOTO_ON_ERROR(audio_hal_codec_iface_config(ctx->codec,
                                                   AUDIO_HAL_CODEC_MODE_ENCODE,
                                                   &codec_cfg.i2s_iface),
                      cleanup,
                      TAG,
                      "ES8388 ADC iface config failed");
    ESP_GOTO_ON_ERROR(audio_hal_ctrl_codec(ctx->codec, AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START),
                      cleanup,
                      TAG,
                      "ES8388 ADC start failed");
    ESP_GOTO_ON_ERROR(configure_es8388_stereo_mic_path(config->es8388_mic_gain_db),
                      cleanup,
                      TAG,
                      "ES8388 LINE2 stereo mic path config failed");

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(CODEC_ADC_I2S_PORT,
                                                                config->sample_rate_hz,
                                                                I2S_DATA_BIT_WIDTH_16BIT,
                                                                AUDIO_STREAM_READER);
    i2s_cfg.task_stack = -1;
    i2s_cfg.buffer_len = 2064;
    i2s_cfg.chan_cfg.dma_desc_num = 6;
    i2s_cfg.chan_cfg.dma_frame_num = (uint32_t)config->dma_frame_samples;
    i2s_stream_set_channel_type(&i2s_cfg, I2S_CHANNEL_TYPE_RIGHT_LEFT);

    ctx->reader = i2s_stream_init(&i2s_cfg);
    ESP_GOTO_ON_FALSE(ctx->reader != NULL, ESP_FAIL, cleanup, TAG, "i2s_stream_init failed");
    ESP_GOTO_ON_ERROR(i2s_stream_set_clk(ctx->reader, config->sample_rate_hz, 16, 2),
                      cleanup,
                      TAG,
                      "i2s_stream_set_clk failed");

    snprintf(ctx->description,
             sizeof(ctx->description),
             "ESP32 Audio Kit A1S/A541 ES8388 stereo capture profile=%s I2S%d %d Hz 16-bit L/R",
             (config->profile_name && config->profile_name[0]) ? config->profile_name : "default",
             CODEC_ADC_I2S_PORT,
             config->sample_rate_hz);
    ESP_LOGI(TAG, "Capture ready: %s, mic_gain=%d dB", ctx->description, config->es8388_mic_gain_db);
    return ESP_OK;

cleanup:
    if (ctx->reader) {
        audio_element_deinit(ctx->reader);
        ctx->reader = NULL;
    }
    if (ctx->codec) {
        audio_hal_deinit(ctx->codec);
        ctx->codec = NULL;
    }
    return ret;
}

static esp_err_t a1s_es8388_stereo_deinit(mic_source_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "invalid source");

    a1s_es8388_stereo_ctx_t *ctx = (a1s_es8388_stereo_ctx_t *)self->ctx;
    if (ctx->reader) {
        audio_element_deinit(ctx->reader);
        ctx->reader = NULL;
    }
    if (ctx->codec) {
        audio_hal_deinit(ctx->codec);
        ctx->codec = NULL;
    }
    return ESP_OK;
}

static int a1s_es8388_stereo_read(mic_source_t *self,
                                  int16_t *primary_out,
                                  size_t primary_capacity_samples,
                                  mic_source_read_info_t *info)
{
    a1s_es8388_stereo_ctx_t *ctx = (a1s_es8388_stereo_ctx_t *)self->ctx;
    if (!ctx->reader || !primary_out || primary_capacity_samples < 2) {
        return -1;
    }

    size_t frame_capacity = sizeof(ctx->left_samples) / sizeof(ctx->left_samples[0]);
    size_t output_frame_capacity = primary_capacity_samples / 2u;
    if (output_frame_capacity < frame_capacity) {
        frame_capacity = output_frame_capacity;
    }

    int wanted_bytes = (int)(frame_capacity * 2u * sizeof(int16_t));
    int bytes_read = audio_element_input(ctx->reader, (char *)ctx->stereo_samples, wanted_bytes);
    if (bytes_read <= 0) {
        ESP_LOGW(TAG, "audio_element_input failed: %d", bytes_read);
        return 0;
    }

    size_t sample_count = (size_t)bytes_read / sizeof(int16_t);
    if ((sample_count % 2u) != 0u) {
        sample_count -= 1u;
    }
    size_t frame_count = sample_count / 2u;

    for (size_t i = 0; i < frame_count; ++i) {
        int16_t left = ctx->stereo_samples[i * 2u];
        int16_t right = ctx->stereo_samples[(i * 2u) + 1u];
        int16_t primary = right;

        switch ((mic_source_primary_mode_t)ctx->cfg.primary_mode) {
            case MIC_SOURCE_PRIMARY_STEREO_RAW:
                primary_out[i * 2u] = left;
                primary_out[(i * 2u) + 1u] = right;
                break;
            case MIC_SOURCE_PRIMARY_RIGHT:
                primary = right;
                primary_out[i * 2u] = primary;
                primary_out[(i * 2u) + 1u] = primary;
                break;
            case MIC_SOURCE_PRIMARY_DIFF_R_MINUS_L: {
                int mixed = (int)right - (int)left;
                if (mixed > INT16_MAX) {
                    mixed = INT16_MAX;
                } else if (mixed < INT16_MIN) {
                    mixed = INT16_MIN;
                }
                primary = (int16_t)mixed;
                primary_out[i * 2u] = primary;
                primary_out[(i * 2u) + 1u] = primary;
                break;
            }
            case MIC_SOURCE_PRIMARY_RIGHT_MINUS_HALF_L: {
                int mixed = (int)right - ((int)left / 2);
                if (mixed > INT16_MAX) {
                    mixed = INT16_MAX;
                } else if (mixed < INT16_MIN) {
                    mixed = INT16_MIN;
                }
                primary = (int16_t)mixed;
                primary_out[i * 2u] = primary;
                primary_out[(i * 2u) + 1u] = primary;
                break;
            }
            default:
                primary = right;
                primary_out[i * 2u] = primary;
                primary_out[(i * 2u) + 1u] = primary;
                break;
        }

        ctx->left_samples[i] = left;
        ctx->right_samples[i] = right;
    }

    if (info) {
        memset(info, 0, sizeof(*info));
        info->raw_frames_read = frame_count;
        info->primary_samples = frame_count;
        info->has_reference = true;
        if ((ctx->metrics_decimator++ % 16u) == 0u) {
            info->has_lane_metrics = true;
            audio_metrics_compute_i16(primary_out, sample_count, &info->primary_metrics);
            audio_metrics_compute_i16(ctx->right_samples, frame_count, &info->reference_metrics);
            audio_metrics_compute_i16(ctx->left_samples, frame_count, &info->left_hi_metrics);
            audio_metrics_compute_i16(ctx->right_samples, frame_count, &info->right_hi_metrics);
        }
    }

    return (int)sample_count;
}

static const char *a1s_es8388_stereo_describe(mic_source_t *self)
{
    a1s_es8388_stereo_ctx_t *ctx = (a1s_es8388_stereo_ctx_t *)self->ctx;
    return ctx->description[0] ? ctx->description : "ESP32 Audio Kit A1S/A541 ES8388 stereo mic";
}

static const mic_source_vtable_t s_vtable = {
    .name = "a1s_es8388_stereo",
    .init = a1s_es8388_stereo_init,
    .deinit = a1s_es8388_stereo_deinit,
    .read = a1s_es8388_stereo_read,
    .describe = a1s_es8388_stereo_describe,
};

mic_source_t mic_source_a1s_es8388_stereo_create(void)
{
    mic_source_t source = {
        .vtable = &s_vtable,
        .ctx = &s_ctx,
    };
    return source;
}

mic_source_t mic_source_lyrat_mini_onboard_create(void)
{
    return mic_source_a1s_es8388_stereo_create();
}
