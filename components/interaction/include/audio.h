#pragma once
#include "esp_codec_dev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_init(void);
esp_codec_dev_handle_t audio_speaker(void);
esp_codec_dev_handle_t audio_microphone(void);

#ifdef __cplusplus
}
#endif
