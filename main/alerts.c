#include "alerts.h"

#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "alert_rules.h"
#include "buzzer.h"
#include "climate.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "ld2450.h"
#include "sensors.h"
#include "telegram.h"
#include "timesync.h"
#include "weather_api.h"
#include "zambretti.h"

/* --- rules & thresholds ------------------------------------------------- */

#define TICK_MS       1000
#define AIR_PERIOD_MS 10000 /* CO2 moves slowly; presence needs every tick */

/* The thresholds themselves are in alert_rules.c, shared with the panel's
 * blinking readings; here is only what a crossing is said and sounded like.
 * Severity, not the zone, picks the emoji, so a two-sided band gets the same
 * ladder as CO2. */
static const char *const EMOJI[] = { "✅", "⚠️", "🔴", "🚨" };

static const struct {
    const char *name;
    const char *unit;
    const char *fmt;  /* value, in the resolution it is shown at elsewhere */
    const char *up;   /* how the crossing is worded, in each direction */
    const char *down;
    bool abs_edge;    /* quote the threshold unsigned: only where the wording
                       * already carries the direction, so a fall does not read
                       * "faster than -1.6" */
} TEXT[ALERT_Q_COUNT] = {
    [ALERT_Q_CO2]        = { "CO₂", " ppm", "%.0f", "выше", "ниже", false },
    [ALERT_Q_TEMP]       = { "Температура", " °C", "%.2f", "выше", "ниже", false },
    [ALERT_Q_RH]         = { "Влажность", "%", "%.1f", "выше", "ниже", false },
    [ALERT_Q_DEW_SPREAD] = { "Запас до точки росы", " °C", "%.1f",
                             "выше", "ниже", false },
    [ALERT_Q_UVI]        = { "UV", "", "%.1f", "выше", "ниже", false },
    [ALERT_Q_TREND]      = { "Давление", " гПа/3ч", "%+.1f",
                             "растёт быстрее", "падает быстрее", true },
    [ALERT_Q_GUST]       = { "Порывы ветра", " км/ч", "%.0f", "выше", "ниже", false },
    [ALERT_Q_OUT_TEMP]   = { "На улице", " °C", "%.1f", "выше", "ниже", false },
};

/* A device silent for this long is out, not between reads: the hot-plug state
 * machine drops a sensor after 3 failures and then probes every 5 s. */
#define SENSOR_GONE_MS (5 * 60 * 1000)

/* Rain within the hour, announced once: re-armed only after the probability has
 * fallen well back, so one hovering at the threshold does not repeat. */
#define RAIN_ON_PCT  70
#define RAIN_OFF_PCT 50

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

/* CO2 is the one rule that also sounds: one motif per zone crossed, so the ear
 * can count what the panel shows. Landing in the top zone always plays the
 * three-motif tune — its held last note is the point of the alarm — however few
 * edges the jump crossed. */
static void co2_sound(int z, int prev)
{
    int n = z > prev ? z - prev : prev - z;
    if (n > 3) {
        n = 3;
    }
    if (z > prev) {
        if (alert_severity(ALERT_Q_CO2, z) >= 3) {
            n = 3;
        }
        buzzer_play(BUZZER_CO2_UP1 + n - 1);
    } else {
        buzzer_play(BUZZER_CO2_DOWN1 + n - 1);
    }
}

static void notify(alert_q_t q, int z, int prev, double v)
{
    /* The edge quoted is the one bounding the zone entered, so a jump over
     * several of them names where the reading ended up, not where it started. */
    bool up = z > prev;
    double edge = alert_edge(q, up ? z : z + 1);

    if (TEXT[q].abs_edge && edge < 0) {
        edge = -edge;
    }

    int sev = alert_severity(q, z);
    char val[16];
    snprintf(val, sizeof(val), TEXT[q].fmt, v);
    telegram_notify("%s %s %s %g%s: %s", EMOJI[sev < 0 ? -sev : sev],
                    TEXT[q].name, up ? TEXT[q].up : TEXT[q].down, edge,
                    TEXT[q].unit, val);
}

/* Sensors are watched through their own getters, which already answer false
 * while a device is absent or failing -- no new state in sensors.c. */
static bool tmp117_alive(void)   { tmp117_data_t d;   return sensors_tmp117_get(&d); }
static bool scd40_alive(void)    { scd40_data_t d;    return sensors_scd40_get(&d); }
static bool bmp581_alive(void)   { bmp581_data_t d;   return sensors_bmp581_get(&d); }
static bool veml7700_alive(void) { veml7700_data_t d; return sensors_veml7700_get(&d); }
static bool as3935_alive(void)   { as3935_data_t d;   return sensors_as3935_get(&d); }

static const struct {
    const char *name;
    bool (*alive)(void);
} DEVICES[] = {
    { "TMP117", tmp117_alive },     { "SCD40", scd40_alive },
    { "BMP581", bmp581_alive },     { "VEML7700", veml7700_alive },
    { "AS3935", as3935_alive },
};
#define DEVICE_COUNT (sizeof(DEVICES) / sizeof(DEVICES[0]))

/* A device that never answered is not missing -- an assembly without it would
 * otherwise report a loss at every boot. */
static void check_sensors(void)
{
    static bool seen[DEVICE_COUNT], ok[DEVICE_COUNT];
    static int64_t gone_ms[DEVICE_COUNT]; /* start of the silence, 0 = none */

    int64_t now = now_ms();
    for (int i = 0; i < (int)DEVICE_COUNT; i++) {
        bool alive = DEVICES[i].alive();
        if (!seen[i]) {
            seen[i] = ok[i] = alive;
            continue;
        }
        if (alive == ok[i]) {
            gone_ms[i] = 0;
            continue;
        }
        if (alive) {
            ok[i] = true;
            gone_ms[i] = 0;
            telegram_notify("✅ %s снова отвечает", DEVICES[i].name);
            continue;
        }
        if (!gone_ms[i]) {
            gone_ms[i] = now;
            continue;
        }
        if (now - gone_ms[i] >= SENSOR_GONE_MS) {
            ok[i] = false;
            gone_ms[i] = 0;
            telegram_notify("⚠️ %s не отвечает %d мин", DEVICES[i].name,
                            SENSOR_GONE_MS / 60000);
        }
    }
}

/* The forecast's probability for the next full hour. The series starts at the
 * hour it was fetched in, so it is indexed off the clock, the same way the
 * panel's rain strip is. */
static void check_rain(const weather_api_data_t *wx, bool ok)
{
    static bool announced;

    time_t now = timesync_is_synced() ? time(NULL) : 0;
    if (!ok || !now || wx->hour_count <= 0) {
        return;
    }

    int i = (int)(((now + wx->utc_offset_s) - wx->hour_start) / 3600) + 1;
    if (i < 0 || i >= wx->hour_count) {
        return;
    }
    int prob = wx->hour_prob_pct[i];
    if (prob < 0) {
        return;
    }

    if (!announced && prob >= RAIN_ON_PCT) {
        announced = true;
        telegram_notify("🌧 Дождь в ближайший час: %d%%", prob);
    } else if (announced && prob < RAIN_OFF_PCT) {
        announced = false;
    }
}

/* Each rule arms on its own first reading, so a sensor that appears late does
 * not report the zone it was already in as a crossing. */
static void check_air(void)
{
    static bool armed[ALERT_Q_COUNT];
    static int zone[ALERT_Q_COUNT];

    climate_t cl;
    climate_get(&cl);
    zambretti_t zb;
    zambretti_get(&zb);
    weather_api_data_t wx;
    bool wx_ok = weather_api_get(&wx);
    const alert_inputs_t in = {
        .cl = &cl, .wx = &wx, .wx_ok = wx_ok, .zb = &zb,
    };

    for (int q = 0; q < ALERT_Q_COUNT; q++) {
        double v;
        if (!alert_sample(q, &in, &v)) {
            continue;
        }
        if (!armed[q]) {
            armed[q] = true;
            zone[q] = alert_zone(q, v, alert_comfort(q));
            continue;
        }

        int z = alert_zone(q, v, zone[q]);
        if (z == zone[q]) {
            continue;
        }
        notify(q, z, zone[q], v);
        if (q == ALERT_Q_CO2) {
            co2_sound(z, zone[q]);
        }
        zone[q] = z;
    }

    check_rain(&wx, wx_ok);
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

/* Queued before Wi-Fi is up -- telegram.c holds it until the link is there.
 * Without it a power cut, a panic and a deliberate restart all look the same
 * from outside: silence and then a station that is up again. */
static void notify_boot(void)
{
    const char *why;
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   why = "питание"; break;
    case ESP_RST_SW:        why = "перезапуск"; break;
    case ESP_RST_PANIC:     why = "паника"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       why = "watchdog"; break;
    case ESP_RST_BROWNOUT:  why = "просадка питания"; break;
    default:                why = "неизвестно"; break;
    }
    telegram_notify("♻️ Станция запустилась: %s", why);
}

static void alerts_task(void *arg)
{
    notify_boot();

    for (int tick = 0;; tick++) {
        check_presence();
        if (tick % (AIR_PERIOD_MS / TICK_MS) == 0) {
            check_air();
            check_sensors();
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void alerts_init(void)
{
    /* 4 KiB stack: telegram_notify formats floats on this task's stack */
    xTaskCreate(alerts_task, "alerts", 4096, NULL, 2, NULL);
}
