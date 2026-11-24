#include "app_framework.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define TAG "app_fw"
#define APP_MAX 4
#define BOOT_GPIO GPIO_NUM_0
#define HOLD_MS 2000

static const app_t *s_apps[APP_MAX];
static int s_count;
static int s_current = -1;

void app_framework_next(void)
{
    if (s_count < 2) {
        ESP_LOGI(TAG, "only one app registered");
        return;
    }
    const int previous = s_current;
    if (s_current >= 0 && s_apps[s_current]->stop) {
        s_apps[s_current]->stop();
    }
    s_current = (s_current + 1) % s_count;
    ESP_LOGI(TAG, "switch to %s", s_apps[s_current]->name);
    if (s_apps[s_current]->start() != ESP_OK) {
        ESP_LOGE(TAG, "%s start failed, restoring %s", s_apps[s_current]->name,
                 previous >= 0 ? s_apps[previous]->name : "none");
        s_current = previous;
        if (previous >= 0) {
            s_apps[previous]->start();
        }
    }
}

static void button_task(void *arg)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);
    uint32_t held_ms = 0;
    while (1) {
        if (gpio_get_level(BOOT_GPIO) == 0) {
            held_ms += 50;
            if (held_ms == HOLD_MS) {
                bool on = !display_debug_enabled();
                display_debug_enable(on);
                display_debug_set_line(0, on ? "DBG ON" : "");
                ESP_LOGI(TAG, "debug overlay %s", on ? "on" : "off");
            }
        } else {
            if (held_ms >= 50 && held_ms < HOLD_MS) {
                app_framework_next();
            }
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t app_framework_init(void)
{
    xTaskCreate(button_task, "app_btn", 4096, NULL, 3, NULL);
    return ESP_OK;
}

esp_err_t app_framework_register(const app_t *app)
{
    if (s_count >= APP_MAX) {
        return ESP_ERR_NO_MEM;
    }
    s_apps[s_count++] = app;
    return ESP_OK;
}

esp_err_t app_framework_start(void)
{
    if (!s_count) {
        return ESP_ERR_INVALID_STATE;
    }
    s_current = 0;
    return s_apps[0]->start();
}