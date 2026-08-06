#include "esp_err.h"
#include "nvs_flash.h"
#include "alerts.h"
#include "button.h"
#include "buzzer.h"
#include "climate.h"
#include "encoder.h"
#include "gui_loop.h"
#include "history.h"
#include "i2c_bus.h"
#include "ld2450.h"
#include "led.h"
#include "ota.h"
#include "sensors.h"
#include "storage.h"
#include "sysinfo.h"
#include "telegram.h"
#include "timesync.h"
#include "weather_api.h"
#include "weather_store.h"
#include "webserver.h"
#include "wifi.h"

void app_main(void)
{
    /* Needed by the modules below (LED brightness, saved Wi-Fi networks). */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    led_init();
    buzzer_init();
    button_init();
    encoder_init();

    ESP_ERROR_CHECK(wifi_connect());
    storage_init();
    climate_init(); /* before history: it samples climate every second */
    history_init();
    sysinfo_init();

    /* Before anything that talks on it, and while the log is still quiet. */
    i2c_bus_init();
    sensors_init();
    ld2450_init(); /* own UART, unrelated to the I2C bus above */

    timesync_init();
    telegram_init();
    alerts_init();
    weather_store_init();
    weather_api_init();

    /* After the data modules, so the display starts on real readings. */
    gui_loop_init();

    /* Last: the handlers read from every module above. */
    webserver_start();

    /* Init went through — mark the image valid, cancelling the OTA rollback. */
    ota_confirm_running_image();
    buzzer_play(BUZZER_BOOT); /* at the end, so it means "came up", not "powered" */

    sysinfo_log_tasks();
    /* app_main returns; the led and wifi tasks carry on. */
}
