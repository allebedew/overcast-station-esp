#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "alerts.h"
#include "button.h"
#include "chip_temp.h"
#include "history.h"
#include "i2c_bus.h"
#include "led.h"
#include "ota.h"
#include "screen_16x2.h"
#include "sensors.h"
#include "storage.h"
#include "telegram.h"
#include "timesync.h"
#include "weather_api.h"
#include "weather_store.h"
#include "webserver.h"
#include "wifi.h"

static const char *TAG = "main";

static void log_task_list(void)
{
    static char task_list_buf[1024];
    vTaskList(task_list_buf);
    ESP_LOGI(TAG, "Task list:\nName          State  Prio  Stack  Num\n%s", task_list_buf);
}

void app_main(void)
{
    /* NVS нужен модулям ниже (яркость LED, список сетей Wi-Fi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    led_init();
    button_init();

    ESP_ERROR_CHECK(wifi_connect());
    storage_init();
    history_init();
    chip_temp_init();

    /* The bus comes up before anything that talks on it — the sensors here,
     * the display further down — and logs a scan while the log is still
     * quiet. */
    i2c_bus_init();
    sensors_init();

    timesync_init();
    telegram_init();
    alerts_init();
    weather_store_init();
    weather_api_init();

    /* Kept after the data modules so the display can go straight to showing
     * their readings. */
    screen_16x2_init();

    /* Last: the request handlers read from every module above, so the server
     * must not accept a request before those are initialised. */
    webserver_start();

    /* инициализация прошла — фиксируем прошивку, отменяя OTA-откат */
    ota_confirm_running_image();

    log_task_list();
    /* app_main завершается — работают задачи led и wifi */
}
