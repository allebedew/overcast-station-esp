#include "pir.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "buzzer.h"

/* Wiring: module OUT -> GPIO11, 5 V, GND. The pin is the LD2450's old TX,
 * free while the radar is not brought up. */
#define PIR_GPIO   11
#define POLL_MS    100

/* The sensor sees movement, not people, and its own hold is only about 3 s —
 * long enough to bridge a step across the room, nowhere near enough for
 * somebody sitting still in front of the panel. So the raw line is held here
 * too, and this is what decides when the room counts as empty. */
#define HOLD_MS    60000

static const char *TAG = "pir";

static volatile bool s_present;

static void pir_task(void *arg)
{
    /* Whatever the pin reads at startup is the state we start from, so a room
     * that is already occupied does not announce itself as an arrival. */
    s_present = gpio_get_level(PIR_GPIO) != 0;
    int64_t since_us = esp_timer_get_time(); /* start of the published state */
    /* Last time the line was high — backdated past the hold when it is not, so
     * booting into an empty room does not announce an arrival. */
    int64_t motion_us = s_present ? since_us : since_us - HOLD_MS * 1000LL;
    ESP_LOGI(TAG, "starting %s", s_present ? "occupied" : "clear");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        int64_t t = esp_timer_get_time();
        if (gpio_get_level(PIR_GPIO)) {
            motion_us = t;
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
        buzzer_play(now ? BUZZER_ARRIVE : BUZZER_LEAVE);
    }
}

bool pir_present(void)
{
    return s_present;
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
