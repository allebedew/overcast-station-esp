#include "sensors.h"

#include "esp_log.h"
#include "driver/temperature_sensor.h"

static const char *TAG = "sensors";

static temperature_sensor_handle_t s_temp_sensor;

void sensors_init(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    ESP_ERROR_CHECK(temperature_sensor_install(&cfg, &s_temp_sensor));
    ESP_ERROR_CHECK(temperature_sensor_enable(s_temp_sensor));
    ESP_LOGI(TAG, "Chip temperature sensor ready");
}

float sensors_chip_temp(void)
{
    float temp = -273;
    temperature_sensor_get_celsius(s_temp_sensor, &temp);
    return temp;
}
