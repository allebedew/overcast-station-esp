#include "alerts.h"

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "sensors.h"
#include "telegram.h"
#include "wifi.h"

/* --- rules & thresholds ------------------------------------------------- */

#define CHECK_PERIOD_MS 10000

/* Chip temperature: alert above HIGH, recovery below HIGH - HYST.
 * The hysteresis gap prevents message spam when hovering at the threshold. */
#define CHIP_TEMP_HIGH 35.0f
#define CHIP_TEMP_HYST 1.0f

/* ------------------------------------------------------------------------ */

static bool get_sta_ip(esp_ip4_addr_t *ip)
{
    if (wifi_get_status() != WIFI_STATUS_CONNECTED) {
        return false;
    }
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        return false;
    }
    *ip = info.ip;
    return ip->addr != 0;
}

/* Welcome message once the first IP is obtained after boot,
 * then a notification every time the IP changes. */
static void check_ip(void)
{
    static bool online;
    static esp_ip4_addr_t last_ip;

    esp_ip4_addr_t ip;
    if (!get_sta_ip(&ip)) {
        return;
    }
    if (!online) {
        online = true;
        telegram_notify("Станция запущена: чип %.1f °C, IP " IPSTR,
                        sensors_chip_temp(), IP2STR(&ip));
    } else if (ip.addr != last_ip.addr) {
        telegram_notify("IP изменился: " IPSTR " → " IPSTR,
                        IP2STR(&last_ip), IP2STR(&ip));
    }
    last_ip = ip;
}

static void check_chip_temp(void)
{
    static bool alarm;

    float t = sensors_chip_temp();
    if (!alarm && t > CHIP_TEMP_HIGH) {
        alarm = true;
        telegram_notify("⚠️ Температура чипа %.1f °C (порог %.0f °C)",
                        t, CHIP_TEMP_HIGH);
    } else if (alarm && t < CHIP_TEMP_HIGH - CHIP_TEMP_HYST) {
        alarm = false;
        telegram_notify("✅ Температура чипа в норме: %.1f °C", t);
    }
}

static void alerts_task(void *arg)
{
    for (;;) {
        check_ip();
        check_chip_temp();
        vTaskDelay(pdMS_TO_TICKS(CHECK_PERIOD_MS));
    }
}

void alerts_init(void)
{
    /* 4 KiB stack: telegram_notify formats floats on this task's stack */
    xTaskCreate(alerts_task, "alerts", 4096, NULL, 2, NULL);
}
