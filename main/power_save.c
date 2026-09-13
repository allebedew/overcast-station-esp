#include "power_save.h"

#include "esp_log.h"

#include "sensors.h"
#include "wifi.h"

static const char *TAG = "power_save";

static bool s_on;

bool power_save_on(void)
{
    return s_on;
}

void power_save_set(bool on)
{
    if (on == s_on) {
        return;
    }
    s_on = on;
    ESP_LOGI(TAG, "%s: wifi and SCD40 %s", on ? "on" : "off", on ? "off" : "back");

    wifi_radio_enable(!on);
    sensors_scd40_enable(!on);
}

void power_save_toggle(void)
{
    power_save_set(!s_on);
}
