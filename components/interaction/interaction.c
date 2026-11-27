#include "interaction_internal.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "interaction"

static QueueHandle_t s_q;
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
    if (sensors_motion_active() && ev->type != EV_BUTTON_PWR) {
        return;
    }
    if ((ev->type == EV_MIC_BLOW || ev->type == EV_MIC_CLAP || ev->type == EV_MIC_VOICE) &&
        sfx_input_guard_active()) {
        return;
    }
    static const char *names[] = {
        "TAP", "DOUBLE", "STROKE", "STROKE_END", "HOLD_BEGIN", "HOLD_END",
        "SWIPE", "PAT", "TILT", "PICKUP", "PUTDOWN",
        "BLOW", "CLAP", "VOICE", "PWR",
    };
    const char *n = (ev->type >= 0 && ev->type < (int)(sizeof(names) / sizeof(names[0])))
                        ? names[ev->type] : "?";
    ESP_LOGI(TAG, "event %s", n);
    if (s_q) {
        xQueueSend(s_q, ev, 0);
    }
    if (display_debug_enabled()) {
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
    esp_err_t ret = sensors_init();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = sfx_init();
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