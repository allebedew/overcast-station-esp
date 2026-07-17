#include "storage.h"

#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "storage";

void storage_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/data",
        .partition_label = "storage",
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at /data: %u/%u KiB used",
             (unsigned)(used / 1024), (unsigned)(total / 1024));
}
