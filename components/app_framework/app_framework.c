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
static TaskHandle_t s_switch_task;

void app_framework_next(void)
{
    if (s_count < 2) {
        ESP_LOGI(TAG, "only one app registered");
        return;
    }
    const int previous = s_current;
    const int next = (s_current + 1) % s_count;
    ESP_LOGI(TAG, "switch begin %s -> %s", previous >= 0 ? s_apps[previous]->name : "none",
             s_apps[next]->name);
    if (s_current >= 0 && s_apps[s_current]->stop) {
        s_apps[s_current]->stop();
    }
    s_current = next;
    esp_err_t ret = s_apps[s_current]->start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s start failed: %s, restoring %s", s_apps[s_current]->name,
                 esp_err_to_name(ret), previous >= 0 ? s_apps[previous]->name : "none");
        s_current = previous;
        if (previous >= 0) {
            s_apps[previous]->start();
        }
    } else {
        ESP_LOGI(TAG, "switch complete %s", s_apps[s_current]->name);
    }
}

static void switch_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        app_framework_next();
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
                xTaskNotifyGive(s_switch_task);
            }
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t app_framework_init(void)
{
    if (xTaskCreate(switch_task, "app_switch", 4096, NULL, 3, &s_switch_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(button_task, "app_btn", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
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