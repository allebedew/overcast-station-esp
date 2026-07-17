#include "led.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "settings.h"

#define LED_GPIO           8
#define DEFAULT_BRIGHTNESS 4
#define BLINK_PERIOD_MS    250
#define PULSE_CYCLE_MS     3000

#define SETTING_BRIGHT "led_bright"

static const char *TAG = "led";

static led_strip_handle_t led_strip;
static volatile led_status_t s_status = LED_STATUS_OFF;
static volatile int s_pulse_count;
static volatile uint8_t s_brightness = DEFAULT_BRIGHTNESS;

void led_set_brightness(uint8_t brightness)
{
    if (brightness == 0) {
        brightness = 1;
    }
    s_brightness = brightness;
    settings_set_u8(SETTING_BRIGHT, brightness);
}

uint8_t led_get_brightness(void)
{
    return s_brightness;
}

void led_set_status(led_status_t status)
{
    s_status = status;
}

void led_set_pulse_count(int count)
{
    s_pulse_count = count;
}

static void set_color(uint8_t r, uint8_t g, uint8_t b)
{
    ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, r, g, b));
    ESP_ERROR_CHECK(led_strip_refresh(led_strip));
}

static void led_task(void *arg)
{
    bool blink_on = false;
    int tick = 0;

    while (true) {
        blink_on = !blink_on;
        tick++;

        switch (s_status) {
        case LED_STATUS_OFF:
            set_color(0, 0, 0);
            break;
        case LED_STATUS_WIFI_CONNECTING:
            if (blink_on) {
                set_color(s_brightness, s_brightness, 0);
            } else {
                set_color(0, 0, 0);
            }
            break;
        case LED_STATUS_WIFI_CONNECTED:
            set_color(0, s_brightness, 0);
            break;
        case LED_STATUS_WIFI_FAILED:
            set_color(s_brightness, 0, 0);
            break;
        case LED_STATUS_WIFI_DISCONNECTED:
            set_color(s_brightness, s_brightness, 0);
            break;
        case LED_STATUS_WIFI_AP: {
            /* каждые 3 с гаснет s_pulse_count раз (250 мс пауза + 250 мс свет) */
            int pos = tick % (PULSE_CYCLE_MS / BLINK_PERIOD_MS);
            bool off = pos < 2 * s_pulse_count && pos % 2 == 0;
            set_color(0, 0, off ? 0 : s_brightness);
            break;
        }
        }

        vTaskDelay(pdMS_TO_TICKS(BLINK_PERIOD_MS));
    }
}

void led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, /* 10 МГц */
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    ESP_ERROR_CHECK(led_strip_clear(led_strip));

    uint8_t b = settings_get_u8(SETTING_BRIGHT, DEFAULT_BRIGHTNESS);
    s_brightness = b > 0 ? b : DEFAULT_BRIGHTNESS;

    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
    ESP_LOGI(TAG, "LED task started, GPIO %d", LED_GPIO);
}
