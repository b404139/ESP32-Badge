#include "storage.h"

#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "storage";
static bool s_mounted;

esp_err_t storage_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 8,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed (%d)", ret);
        return ret;
    }
    size_t total = 0;
    size_t used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "spiffs mounted: %u/%u bytes", (unsigned)used, (unsigned)total);
    s_mounted = true;
    return ESP_OK;
}
