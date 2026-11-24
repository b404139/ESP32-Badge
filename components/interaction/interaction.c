#include "interaction_internal.h"
#include "display.h"
#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "interaction"

static QueueHandle_t s_q;
static bool s_audio_inited;
static bool s_started;

uint32_t interaction_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

QueueHandle_t interaction_queue(void)
{
    return s_q;
}

void interaction_send(const input_event_t *ev)
{
    if (s_q) {
        xQueueSend(s_q, ev, 0);
    }
    if (display_debug_enabled()) {
        static const char *names[] = {
            "TAP", "DOUBLE", "STROKE", "STROKE_END", "HOLD_BEGIN", "HOLD_END",
            "SWIPE", "PAT", "SHAKE", "TILT", "PICKUP", "PUTDOWN",
            "BLOW", "CLAP", "VOICE",
        };
        const char *n = (ev->type >= 0 && ev->type < (int)(sizeof(names) / sizeof(names[0])))
                            ? names[ev->type] : "?";
        char line[48];
        snprintf(line, sizeof(line), "EV %s", n);
        display_debug_set_line(1, line);
    }
}

void interaction_flush(void)
{
    if (!s_q) {
        return;
    }
    input_event_t ev;
    while (xQueueReceive(s_q, &ev, 0) == pdTRUE) {
    }
}

esp_err_t interaction_init(void)
{
    if (s_q) {
        return ESP_OK;
    }
    s_q = xQueueCreate(16, sizeof(input_event_t));
    if (!s_q) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_audio_inited) {
        const i2s_std_config_t audio_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
            .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = BSP_I2S_MCLK,
                .bclk = BSP_I2S_SCLK,
                .ws = BSP_I2S_LCLK,
                .dout = BSP_I2S_DOUT,
                .din = BSP_I2S_DSIN,
                .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
            },
        };
        esp_err_t ret = bsp_audio_init(&audio_cfg);
        if (ret != ESP_OK) {
            return ret;
        }
        s_audio_inited = true;
    }
    esp_err_t ret = sfx_init();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = sensors_init();
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "interaction ready (touch+imu+mic+sfx)");
    return ESP_OK;
}

void interaction_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;
    sfx_start();
    sensors_start();
}

void interaction_stop(void)
{
    if (!s_started) {
        return;
    }
    s_started = false;
    sensors_stop();
    sfx_stop();
}