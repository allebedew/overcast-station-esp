#include "led.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "settings.h"
#include "wifi.h"
#include "sensors.h"
#include "ota.h"

#define LED_GPIO           8
#define DEFAULT_BRIGHTNESS 4
#define BLINK_PERIOD_MS    100
#define PULSE_CYCLE_MS     3000

/* CO2 anchors (ppm) for the connected-state color: fully green at/below
 * CO2_GREEN, pure yellow at CO2_YELLOW, fully red at/above CO2_RED, with a
 * smooth gradient between the anchors. */
#define CO2_GREEN  400
#define CO2_YELLOW 800
#define CO2_RED    1200

/* Ignore a missing SCD40 reading right after boot: the sensor needs a few
 * seconds for its first measurement, and we must not flash the "sensor error"
 * code while it is simply warming up. */
#define SENSOR_WARMUP_US (10 * 1000000LL)

/* Error codes: number of red blinks per cycle. */
#define ERR_SENSOR 2 /* SCD40 not responding */

#define SETTING_BRIGHT "led_bright"

static const char *TAG = "led";

static led_strip_handle_t led_strip;
static volatile bool s_activity;
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

void led_notify_activity(void)
{
    s_activity = true;
}

static void set_color(uint8_t r, uint8_t g, uint8_t b)
{
    ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, r, g, b));
    ESP_ERROR_CHECK(led_strip_refresh(led_strip));
}

/* True during the "pulse" slots: `count` flashes of BLINK_PERIOD_MS each,
 * separated by one dark slot, within a PULSE_CYCLE_MS window. Used both for
 * the AP client count (LED blinks OFF from a lit base) and the error code
 * (LED blinks ON from a dark base). */
static bool in_pulse(int tick, int count)
{
    int pos = tick % (PULSE_CYCLE_MS / BLINK_PERIOD_MS);
    return pos < 2 * count && pos % 2 == 0;
}

/* Current error code, 0 if none. Only reported once the sensor has had time
 * to produce its first reading (see SENSOR_WARMUP_US). */
static int error_code(void)
{
    if (esp_timer_get_time() > SENSOR_WARMUP_US) {
        scd40_data_t d;
        if (!sensors_scd40_get(&d)) {
            return ERR_SENSOR;
        }
    }
    return 0;
}

/* Maps a CO2 level to a smooth green->yellow->red color scaled to brightness
 * `b`: red rises 0->b between CO2_GREEN and CO2_YELLOW, then green falls b->0
 * between CO2_YELLOW and CO2_RED. */
static void co2_color(uint16_t co2, uint8_t b, uint8_t *r, uint8_t *g)
{
    if (co2 <= CO2_GREEN) {
        *r = 0;
        *g = b;
    } else if (co2 < CO2_YELLOW) {
        *r = (co2 - CO2_GREEN) * b / (CO2_YELLOW - CO2_GREEN); /* green -> yellow */
        *g = b;
    } else if (co2 < CO2_RED) {
        *r = b;
        *g = b - (co2 - CO2_YELLOW) * b / (CO2_RED - CO2_YELLOW); /* yellow -> red */
    } else {
        *r = b;
        *g = 0;
    }
}

/* Connected & healthy: solid color by CO2 level, dipping off for one tick on
 * network activity. Falls back to green while the first reading is pending. */
static void show_co2(int tick, bool activity)
{
    (void)tick;
    scd40_data_t d;
    uint16_t co2 = sensors_scd40_get(&d) ? d.co2_ppm : 0;

    if (activity) {
        set_color(0, 0, 0);
        return;
    }
    uint8_t r, g;
    co2_color(co2, s_brightness, &r, &g);
    set_color(r, g, 0);
}

/*
 * Display priority (highest first):
 *   1. OTA in progress      -> purple blinking
 *   2. Error present        -> red, blink count = error code
 *   3. Wi-Fi AP mode        -> blue, pulses off = number of AP clients
 *   4. Wi-Fi connecting     -> green blinking
 *   5. Connected & healthy  -> solid, color by CO2 level (+ activity dip)
 */
static void led_task(void *arg)
{
    bool blink_on = false;
    int tick = 0;

    while (true) {
        blink_on = !blink_on;
        tick++;

        bool activity = s_activity;
        s_activity = false;

        uint8_t b = s_brightness;
        int code;

        if (ota_is_active()) {
            set_color(blink_on ? b : 0, 0, blink_on ? b : 0); /* purple blink */
        } else if ((code = error_code()) != 0) {
            set_color(in_pulse(tick, code) ? b : 0, 0, 0); /* red coded blink */
        } else {
            wifi_info_t wifi;
            wifi_get_info(&wifi);
            if (wifi.ap_active) {
                /* blue, dips off `clients` times each cycle */
                bool off = in_pulse(tick, wifi.ap_clients);
                set_color(0, 0, off ? 0 : b);
            } else if (wifi.sta_state == WIFI_STA_CONNECTED) {
                show_co2(tick, activity);
            } else { /* connecting / retry / idle */
                set_color(0, blink_on ? b : 0, 0); /* green blink */
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
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz */
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    ESP_ERROR_CHECK(led_strip_clear(led_strip));

    uint8_t br = settings_get_u8(SETTING_BRIGHT, DEFAULT_BRIGHTNESS);
    s_brightness = br > 0 ? br : DEFAULT_BRIGHTNESS;

    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
    ESP_LOGI(TAG, "LED task started, GPIO %d", LED_GPIO);
}
