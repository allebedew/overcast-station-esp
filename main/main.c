#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"

/* Встроенный адресный светодиод WS2812 на ESP32-C6-DevKitC-1 / DevKitM-1 */
#define BLINK_GPIO      8
#define BLINK_PERIOD_MS 500

static const char *TAG = "blink";

static led_strip_handle_t led_strip;

static void configure_led(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, /* 10 МГц */
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    ESP_ERROR_CHECK(led_strip_clear(led_strip));
}

void app_main(void)
{
    configure_led();
    ESP_LOGI(TAG, "Blink started, GPIO %d", BLINK_GPIO);

    bool led_on = false;
    while (true) {
        if (led_on) {
            ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, 16, 16, 16));
            ESP_ERROR_CHECK(led_strip_refresh(led_strip));
        } else {
            ESP_ERROR_CHECK(led_strip_clear(led_strip));
        }
        led_on = !led_on;
        vTaskDelay(pdMS_TO_TICKS(BLINK_PERIOD_MS));
    }
}
