#include "alerts.h"

#include <math.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "sensors.h"
#include "telegram.h"
#include "wifi.h"

/* --- rules & thresholds ------------------------------------------------- */

#define CHECK_PERIOD_MS 10000

/* CO2 zones: 0 = fresh (<800), 1 = stale (800..1200), 2 = bad (>1200).
 * A zone change is reported; the hysteresis margin keeps sensor noise
 * from flapping around a threshold. */
static const int CO2_THRESHOLDS[] = { 500, 800, 1200 };
#define CO2_HYST_PPM 25

/* Report temperature/humidity when they drift this far from the last
 * reported value. */
#define TEMP_DELTA_C 2.0f
#define RH_DELTA_PCT 10.0f

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

/* Current zone given the previous one; moving to another zone requires
 * clearing the threshold by the hysteresis margin. */
static int co2_zone(int ppm, int prev)
{
    int z = prev;
    while (z < 2 && ppm > CO2_THRESHOLDS[z] + CO2_HYST_PPM) {
        z++;
    }
    while (z > 0 && ppm < CO2_THRESHOLDS[z - 1] - CO2_HYST_PPM) {
        z--;
    }
    return z;
}

static void check_air(void)
{
    static bool baseline;
    static int zone;
    static float last_temp, last_rh;

    scd40_data_t d;
    if (!sensors_scd40_get(&d)) {
        return;
    }
    if (!baseline) { /* first reading only arms the rules */
        baseline = true;
        zone = co2_zone(d.co2_ppm, 0);
        last_temp = d.temp_c;
        last_rh = d.rh_pct;
        return;
    }

    int z = co2_zone(d.co2_ppm, zone);
    if (z > zone) {
        telegram_notify("%s CO₂ выше %d ppm: %u",
                        z == 2 ? "🔴" : "⚠️", CO2_THRESHOLDS[z - 1], d.co2_ppm);
    } else if (z < zone) {
        telegram_notify("✅ CO₂ ниже %d ppm: %u", CO2_THRESHOLDS[z], d.co2_ppm);
    }
    zone = z;

    if (fabsf(d.temp_c - last_temp) >= TEMP_DELTA_C) {
        telegram_notify("🌡 Температура %.1f °C (было %.1f)",
                        d.temp_c, last_temp);
        last_temp = d.temp_c;
    }
    if (fabsf(d.rh_pct - last_rh) >= RH_DELTA_PCT) {
        telegram_notify("💧 Влажность %.0f %% (было %.0f)",
                        d.rh_pct, last_rh);
        last_rh = d.rh_pct;
    }
}

static void alerts_task(void *arg)
{
    for (;;) {
        check_ip();
        check_air();
        vTaskDelay(pdMS_TO_TICKS(CHECK_PERIOD_MS));
    }
}

void alerts_init(void)
{
    /* 4 KiB stack: telegram_notify formats floats on this task's stack */
    xTaskCreate(alerts_task, "alerts", 4096, NULL, 2, NULL);
}
