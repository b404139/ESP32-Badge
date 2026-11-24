#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "app_framework.h"
#include "interaction.h"
#include "gif_app.h"
#include "grokbot_app.h"

static const char *TAG = "badge";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(bsp_i2c_init());

    // Always-on shared interaction layer (touch/IMU/mic sensing + sfx); both apps
    // drain the same event queue.
    ESP_ERROR_CHECK(interaction_init());
    interaction_start();

    ESP_ERROR_CHECK(app_framework_init());
    ESP_ERROR_CHECK(app_framework_register(&grokbot_app));
    ESP_ERROR_CHECK(app_framework_register(&gif_app));
    ESP_ERROR_CHECK(app_framework_start());
    ESP_LOGI(TAG, "apps registered: grokbot, gif (BOOT click=switch, hold=debug)");
}