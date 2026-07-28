#include "chip_temp.h"

#include "driver/temperature_sensor.h"
#include "esp_log.h"

static const char *TAG = "chip_temp";

static temperature_sensor_handle_t s_sensor;

void chip_temp_init(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    ESP_ERROR_CHECK(temperature_sensor_install(&cfg, &s_sensor));
    ESP_ERROR_CHECK(temperature_sensor_enable(s_sensor));
    ESP_LOGI(TAG, "chip temperature sensor ready");
}

float chip_temp_celsius(void)
{
    float temp = -273;
    temperature_sensor_get_celsius(s_sensor, &temp);
    return temp;
}
