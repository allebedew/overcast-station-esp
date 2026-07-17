#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "button.h"
#include "led.h"
#include "sensors.h"
#include "webserver.h"
#include "wifi.h"

static const char *TAG = "main";

static void log_task_list(void)
{
    static char task_list_buf[1024];
    vTaskList(task_list_buf);
    ESP_LOGI(TAG, "Task list:\nName          State  Prio  Stack  Num\n%s", task_list_buf);
}

static void on_time_sync(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized (SNTP)");
}

static void on_wifi_status(wifi_status_t status)
{
    switch (status) {
    case WIFI_STATUS_CONNECTING:
        led_set_status(LED_STATUS_WIFI_CONNECTING);
        break;
    case WIFI_STATUS_CONNECTED:
        led_set_status(LED_STATUS_WIFI_CONNECTED);
        break;
    case WIFI_STATUS_FAILED:
        led_set_status(LED_STATUS_WIFI_FAILED);
        break;
    case WIFI_STATUS_AP_MODE:
        led_set_pulse_count(wifi_get_ap_client_count());
        led_set_status(LED_STATUS_WIFI_AP);
        break;
    case WIFI_STATUS_WAITING_RETRY:
    case WIFI_STATUS_DISCONNECTED:
        led_set_status(LED_STATUS_WIFI_DISCONNECTED);
        break;
    }
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
    led_set_status(LED_STATUS_WIFI_DISCONNECTED);
    button_init();

    wifi_set_status_callback(on_wifi_status);
    ESP_ERROR_CHECK(wifi_connect());
    sensors_init();
    webserver_start();

    /* часы в UTC; клиент сам повторяет запросы, пока сеть не появится */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_cfg.sync_cb = on_time_sync;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_cfg));

    log_task_list();
    /* app_main завершается — работают задачи led и wifi */
}
