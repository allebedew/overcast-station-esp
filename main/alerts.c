#include "alerts.h"

#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "buzzer.h"
#include "esp_timer.h"
#include "ld2450.h"
#include "sensors.h"
#include "telegram.h"

/* --- rules & thresholds ------------------------------------------------- */

#define TICK_MS       1000
#define AIR_PERIOD_MS 10000 /* CO2 moves slowly; presence needs every tick */

/* CO2 zones: fresh (<800), stale (800..1200), bad (1200..2000), alarming
 * (>2000). The hysteresis margin keeps sensor noise from flapping around a
 * threshold. */
static const int CO2_THRESHOLDS[] = { 800, 1200, 2000 };
#define CO2_ZONES (sizeof(CO2_THRESHOLDS) / sizeof(CO2_THRESHOLDS[0]))
#define CO2_HYST_PPM 25

static const char *const CO2_EMOJI[] = { "✅", "⚠️", "🔴", "🚨" };

/* The radar loses whoever sits still, so an absence counts only after it has
 * outlasted plausible motionless sitting. Arrival needs far less: its own
 * presence flag is already held for seconds, and this only rejects a lone
 * spurious frame. */
#define ARRIVE_CONFIRM_MS 3000
#define LEAVE_CONFIRM_MS  (5 * 60 * 1000)

/* ------------------------------------------------------------------------ */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* Leaving a zone requires clearing its threshold by the hysteresis margin. */
static int co2_zone(int ppm, int prev)
{
    int z = prev;
    while (z < (int)CO2_ZONES && ppm > CO2_THRESHOLDS[z] + CO2_HYST_PPM) {
        z++;
    }
    while (z > 0 && ppm < CO2_THRESHOLDS[z - 1] - CO2_HYST_PPM) {
        z--;
    }
    return z;
}

static void check_air(void)
{
    static bool armed;
    static int zone;

    scd40_data_t d;
    if (!sensors_scd40_get(&d)) {
        return;
    }
    if (!armed) { /* first reading only arms the rule */
        armed = true;
        zone = co2_zone(d.co2_ppm, 0);
        return;
    }

    int z = co2_zone(d.co2_ppm, zone);
    if (z > zone) {
        telegram_notify("%s CO₂ выше %d ppm: %u",
                        CO2_EMOJI[z], CO2_THRESHOLDS[z - 1], d.co2_ppm);
    } else if (z < zone) {
        telegram_notify("%s CO₂ ниже %d ppm: %u",
                        CO2_EMOJI[z], CO2_THRESHOLDS[z], d.co2_ppm);
    }
    zone = z;
}

static void format_span(char *buf, size_t n, int64_t ms)
{
    int min = (int)(ms / 60000);
    if (min < 1) {
        snprintf(buf, n, "%d с", (int)(ms / 1000));
    } else if (min < 60) {
        snprintf(buf, n, "%d мин", min);
    } else {
        snprintf(buf, n, "%d ч %02d мин", min / 60, min % 60);
    }
}

/* The buzzer follows the raw flag rather than the confirmed edges below: the
 * point of it is to answer at once, and the flag's own 5 s hold already keeps
 * it from chattering. */
static void presence_sound(bool present)
{
    static bool armed, last;

    if (!armed) {
        armed = true;
        last = present;
        return;
    }
    if (present == last) {
        return;
    }
    last = present;
    buzzer_play(present ? BUZZER_ARRIVE : BUZZER_LEAVE);
}

/* Confirmed arrivals and departures. Both edges are dated by when the raw flag
 * actually flipped, not by when the confirmation window expired, so the
 * reported durations exclude the window. */
static void check_presence(void)
{
    static bool armed, occupied, since_boot = true;
    static int64_t since_ms; /* start of the current confirmed state */
    static int64_t edge_ms;  /* start of the contradicting run, 0 = none */

    ld2450_data_t r;
    if (!ld2450_get(&r)) { /* radar silent: state unknown, not empty */
        return;
    }

    presence_sound(r.presence);

    int64_t now = now_ms();
    if (!armed) {
        armed = true;
        occupied = r.presence;
        since_ms = now;
        return;
    }
    if (r.presence == occupied) {
        edge_ms = 0;
        return;
    }
    if (!edge_ms) {
        edge_ms = now;
        return;
    }
    if (now - edge_ms < (occupied ? LEAVE_CONFIRM_MS : ARRIVE_CONFIRM_MS)) {
        return;
    }

    char span[24];
    format_span(span, sizeof(span), edge_ms - since_ms);
    if (occupied) {
        telegram_notify("🚪 Ушёл, был здесь %s", span);
    } else if (since_boot) {
        telegram_notify("👋 Пришёл");
    } else {
        telegram_notify("👋 Пришёл, никого не было %s", span);
    }

    occupied = !occupied;
    since_ms = edge_ms;
    edge_ms = 0;
    since_boot = false;
}

static void alerts_task(void *arg)
{
    for (int tick = 0;; tick++) {
        check_presence();
        if (tick % (AIR_PERIOD_MS / TICK_MS) == 0) {
            check_air();
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void alerts_init(void)
{
    /* 4 KiB stack: telegram_notify formats floats on this task's stack */
    xTaskCreate(alerts_task, "alerts", 4096, NULL, 2, NULL);
}
