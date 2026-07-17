#include "history.h"

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

/* Writers: sensor task (add) and esp_timer task (flush); reader: httpd. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static history_point_t s_ring[HISTORY_LEN];
static int s_head;  /* next write position */
static int s_count;

/* accumulator for the minute in progress */
static uint32_t s_acc_co2;
static float s_acc_temp, s_acc_rh;
static int s_acc_n;

/* Once a minute: turn the accumulated readings into one ring point.
 * With no readings the point is written as a gap, so the time axis
 * stays uniform even when the sensor is offline. */
static void flush_cb(void *arg)
{
    taskENTER_CRITICAL(&s_lock);
    history_point_t p = { .valid = s_acc_n > 0 };
    if (p.valid) {
        p.co2_ppm = s_acc_co2 / s_acc_n;
        p.temp_cx10 = (int16_t)lroundf(10.0f * s_acc_temp / s_acc_n);
        p.rh_pct = (uint8_t)lroundf(s_acc_rh / s_acc_n);
    }
    s_acc_co2 = 0;
    s_acc_temp = 0;
    s_acc_rh = 0;
    s_acc_n = 0;

    s_ring[s_head] = p;
    s_head = (s_head + 1) % HISTORY_LEN;
    if (s_count < HISTORY_LEN) {
        s_count++;
    }
    taskEXIT_CRITICAL(&s_lock);
}

void history_init(void)
{
    const esp_timer_create_args_t args = {
        .callback = flush_cb,
        .name = "history",
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 60 * 1000000ULL));
}

void history_add(uint16_t co2_ppm, float temp_c, float rh_pct)
{
    taskENTER_CRITICAL(&s_lock);
    s_acc_co2 += co2_ppm;
    s_acc_temp += temp_c;
    s_acc_rh += rh_pct;
    s_acc_n++;
    taskEXIT_CRITICAL(&s_lock);
}

int history_count(void)
{
    taskENTER_CRITICAL(&s_lock);
    int count = s_count;
    taskEXIT_CRITICAL(&s_lock);
    return count;
}

bool history_get(int idx, history_point_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    bool ok = idx >= 0 && idx < s_count;
    if (ok) {
        *out = s_ring[(s_head - s_count + idx + HISTORY_LEN) % HISTORY_LEN];
    }
    taskEXIT_CRITICAL(&s_lock);
    return ok;
}
