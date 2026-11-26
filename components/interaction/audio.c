#include "audio.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev_defaults.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_log.h"

#define TAG "audio"
#define SAMPLE_RATE 24000
#define AUDIO_MCLK_GPIO GPIO_NUM_16

static const i2s_std_config_t s_tx_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
        .mclk = AUDIO_MCLK_GPIO,
        .bclk = BSP_I2S_SCLK,
        .ws = BSP_I2S_LCLK,
        .dout = BSP_I2S_DOUT,
        .din = I2S_GPIO_UNUSED,
        .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
};

static const i2s_tdm_config_t s_rx_cfg = {
    .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_TDM_PHILIP_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
    .gpio_cfg = {
        .mclk = AUDIO_MCLK_GPIO,
        .bclk = BSP_I2S_SCLK,
        .ws = BSP_I2S_LCLK,
        .dout = I2S_GPIO_UNUSED,
        .din = BSP_I2S_DSIN,
        .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
};

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_out_ctrl;
static const audio_codec_ctrl_if_t *s_in_ctrl;
static const audio_codec_gpio_if_t *s_gpio;
static const audio_codec_if_t *s_out_codec;
static const audio_codec_if_t *s_in_codec;
static esp_codec_dev_handle_t s_speaker;
static esp_codec_dev_handle_t s_microphone;

esp_codec_dev_handle_t audio_speaker(void)
{
    return s_speaker;
}

esp_codec_dev_handle_t audio_microphone(void)
{
    return s_microphone;
}

esp_err_t audio_init(void)
{
    if (s_data_if) {
        return ESP_OK;
    }

    esp_err_t ret;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.auto_clear_before_cb = false;
    chan_cfg.intr_priority = 0;
    ret = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (ret != ESP_OK) goto fail;
    ret = i2s_channel_init_std_mode(s_tx, &s_tx_cfg);
    if (ret != ESP_OK) goto fail;
    ret = i2s_channel_init_tdm_mode(s_rx, &s_rx_cfg);
    if (ret != ESP_OK) goto fail;
    ret = i2s_channel_enable(s_tx);
    if (ret != ESP_OK) goto fail;
    ret = i2s_channel_enable(s_rx);
    if (ret != ESP_OK) goto fail;

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!s_data_if) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    s_out_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!s_out_ctrl) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_gpio = audio_codec_new_gpio();
    if (!s_gpio) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    es8311_codec_cfg_t out_cfg = {
        .ctrl_if = s_out_ctrl,
        .gpio_if = s_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    s_out_codec = es8311_codec_new(&out_cfg);
    if (!s_out_codec) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    i2c_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
    s_in_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!s_in_ctrl) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    es7210_codec_cfg_t in_cfg = {
        .ctrl_if = s_in_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    s_in_codec = es7210_codec_new(&in_cfg);
    if (!s_in_codec) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_out_codec,
        .data_if = s_data_if,
    };
    s_speaker = esp_codec_dev_new(&dev_cfg);
    if (!s_speaker) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = s_in_codec;
    s_microphone = esp_codec_dev_new(&dev_cfg);
    if (!s_microphone) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    esp_codec_dev_sample_info_t mic_fs = {
        .sample_rate = SAMPLE_RATE,
        .channel = 4,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2),
        .bits_per_sample = 16,
    };
    ret = esp_codec_dev_open(s_microphone, &mic_fs);
    if (ret != ESP_OK) goto fail;
    ret = esp_codec_dev_set_in_channel_gain(s_microphone, 0x0f, 30.0f);
    if (ret != ESP_OK) goto fail;

    esp_codec_dev_sample_info_t out_fs = {
        .sample_rate = SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    ret = esp_codec_dev_open(s_speaker, &out_fs);
    if (ret != ESP_OK) goto fail;
    ret = esp_codec_dev_set_out_mute(s_speaker, false);
    if (ret != ESP_OK) goto fail;
    ret = esp_codec_dev_set_out_vol(s_speaker, 70);
    if (ret != ESP_OK) goto fail;

    ESP_LOGI(TAG, "I2S0 STD TX + four-slot TDM RX ready, MCLK=GPIO16, MIC1/MIC2 slots=0/2");
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(ret));
    if (s_microphone) {
        esp_codec_dev_close(s_microphone);
        esp_codec_dev_delete(s_microphone);
        s_microphone = NULL;
    }
    if (s_speaker) {
        esp_codec_dev_close(s_speaker);
        esp_codec_dev_delete(s_speaker);
        s_speaker = NULL;
    }
    if (s_in_codec) audio_codec_delete_codec_if(s_in_codec);
    if (s_out_codec) audio_codec_delete_codec_if(s_out_codec);
    if (s_in_ctrl) audio_codec_delete_ctrl_if(s_in_ctrl);
    if (s_out_ctrl) audio_codec_delete_ctrl_if(s_out_ctrl);
    if (s_gpio) audio_codec_delete_gpio_if(s_gpio);
    if (s_data_if) audio_codec_delete_data_if(s_data_if);
    if (s_tx) i2s_del_channel(s_tx);
    if (s_rx) i2s_del_channel(s_rx);
    s_data_if = NULL;
    s_in_codec = s_out_codec = NULL;
    s_in_ctrl = s_out_ctrl = NULL;
    s_gpio = NULL;
    s_tx = s_rx = NULL;
    return ret;
}
