#include "mic_source.h"

#include "esp_check.h"

esp_err_t mic_source_init(mic_source_t *source, const mic_source_config_t *config)
{
    ESP_RETURN_ON_FALSE(source && source->vtable && source->vtable->init, ESP_ERR_INVALID_ARG, "MIC_SOURCE", "invalid source");
    return source->vtable->init(source, config);
}

esp_err_t mic_source_deinit(mic_source_t *source)
{
    ESP_RETURN_ON_FALSE(source && source->vtable && source->vtable->deinit, ESP_ERR_INVALID_ARG, "MIC_SOURCE", "invalid source");
    return source->vtable->deinit(source);
}

int mic_source_read(mic_source_t *source,
                    int16_t *primary_out,
                    size_t primary_capacity_samples,
                    mic_source_read_info_t *info)
{
    if (!source || !source->vtable || !source->vtable->read) {
        return -1;
    }
    return source->vtable->read(source, primary_out, primary_capacity_samples, info);
}

const char *mic_source_describe(mic_source_t *source)
{
    if (!source || !source->vtable || !source->vtable->describe) {
        return "unknown";
    }
    return source->vtable->describe(source);
}
