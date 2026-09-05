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
#include "led.h"
#include "ota.h"
#include "pir.h"
#include "sensors.h"
#include "settings.h"
#include "storage.h"
#include "sysinfo.h"
#include "telegram.h"
#include "timesync.h"
#include "weather_api.h"
#include "weather_store.h"
#include "webserver.h"
#include "wg.h"
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

    settings_init(); /* before everything that reads a setting at init */

    led_init();
    buzzer_init();
    button_init();
    encoder_init();

    ESP_ERROR_CHECK(wifi_connect());
    timesync_init(); /* right after: it hooks the got-IP event wifi is racing to */
    storage_init();
    climate_init(); /* before history: it samples climate every second */
    history_init();
    weather_store_init(); /* before the gui: its model reads the active location */

    /* Early, so the panel is alive through the slow init below; the values it
     * reads fill in as those modules come up. */
    gui_loop_init();
    buzzer_play(BUZZER_BOOT);

    sysinfo_init();

    /* Before anything that talks on it, and while the log is still quiet. */
    i2c_bus_init();
    sensors_init();
    /* The radar is unplugged and the PIR sits on its old RX pin; ld2450.c
     * stays in the build, and everything that reads it copes with silence. */
    pir_init();

    telegram_init();
    alerts_init();
    weather_api_init();
    wg_init(); /* waits for the clock itself: the handshake is timestamped */

    /* Last: the handlers read from every module above. */
    webserver_start();

    /* Init went through — mark the image valid, cancelling the OTA rollback. */
    ota_confirm_running_image();

    sysinfo_log_tasks();
    /* app_main returns; the led and wifi tasks carry on. */
}
