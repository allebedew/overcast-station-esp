#include "pir.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

/* Wiring: module OUT -> GPIO11, 5 V, GND. */
#define PIR_GPIO   11
#define POLL_MS    100

/* The sensor sees movement, not people, and its own hold is only about 3 s —
 * long enough to bridge a step across the room, nowhere near enough for
 * somebody sitting still in front of the panel. So the raw line is held here
 * too, and this is what decides when the room counts as empty. */
#define HOLD_MS    (5 * 60 * 1000)

static const char *TAG = "pir";

/* Boot counts as a movement: the panel comes up lit, before pir_init() too, and
 * goes dark after HOLD_MS if nothing moves. The pin itself is no guide here —
 * someone sitting still reads low, and the module is unsettled while it warms. */
static volatile bool s_present = true;
static volatile bool s_moved;

static void pir_task(void *arg)
{
    int64_t since_us = esp_timer_get_time(); /* start of the published state */
    int64_t motion_us = since_us;            /* last time the line was high */
    ESP_LOGI(TAG, "starting occupied (assumed)");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        int64_t t = esp_timer_get_time();
        if (gpio_get_level(PIR_GPIO)) {
            motion_us = t;
            s_moved = true;
        }

        bool now = (t - motion_us) / 1000 <= HOLD_MS;
        if (now == s_present) {
            continue;
        }

        s_present = now;
        /* Dated by the movement rather than by the hold expiring, so the
         * reported spans do not carry HOLD_MS around with them. */
        int64_t edge_us = now ? t : motion_us;
        ESP_LOGI(TAG, "%s after %llds", now ? "occupied" : "clear",
                 (long long)((edge_us - since_us) / 1000000));
        since_us = edge_us;
    }
}

bool pir_present(void)
{
    return s_present;
}

bool pir_moved(void)
{
    return s_moved;
}

bool pir_raw(void)
{
    return gpio_get_level(PIR_GPIO) != 0;
}

void pir_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIR_GPIO,
        .mode = GPIO_MODE_INPUT,
        /* The module drives the line both ways, but a pull-down keeps an
         * unplugged sensor reading as an empty room rather than floating. */
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    xTaskCreate(pir_task, "pir", 2560, NULL, 2, NULL);
    ESP_LOGI(TAG, "PIR on GPIO%d", PIR_GPIO);
}
