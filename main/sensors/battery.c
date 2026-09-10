#include "battery.h"

#include <stdint.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "sensors.h"

#define NVS_NAMESPACE "battery"
#define NVS_KEY       "charge" /* i64, mA·µs */

/* Wider than the INA260's worst offset (5 mA), narrower than the station's own
 * draw on battery (43 mA) and the BQ25185's termination current (10 % of the
 * 1 A or 500 mA charge current). */
#define DEADBAND_MA 10.0f

/* The charger recharges below VBATREG - 100 mV = 4.1 V, so a terminated cell
 * rests above this. */
#define FULL_MIN_V 4.05f

/* Results in a row before `full` pins the count to the capacity: the one result
 * that straddles an unplug mid-charge can average into the deadband. */
#define FULL_CONFIRM 2

/* Longer than any bus hold-up between results; a sensor that went offline comes
 * back after at least the 5 s probe period, and that gap is not integrated. */
#define MAX_GAP_US 2000000

#define MAUS_PER_MAH 3.6e9 /* mA·µs */
#define FULL_MAUS    ((int64_t)(BATTERY_CAPACITY_MAH * MAUS_PER_MAH))

/* What a power cut may lose; on a 1 A charge that is a write every 3 min. */
#define SAVE_STEP_MAUS ((int64_t)(50 * MAUS_PER_MAH))

/* The time left runs at the count's own rate: sampled every second over the last
 * 30 s, restarted on a mode change, a gap or a hand-set count, and trusted once
 * it spans 10 s. */
#define RATE_STEP_US 1000000
#define RATE_SLOTS   31
#define RATE_MIN_US  10000000
#define RATE_MIN_MA  1.0 /* slower is "never": keeps eta_s in an int */

static const char *TAG = "battery";

/* Typical LiPo resting curve. Under the station's load it reads a few % low; on
 * charge the charger lifts the terminal, so it reads high. */
static const struct {
    uint16_t mv;
    uint8_t  pct;
} OCV[] = {
    { 3270, 0 },  { 3610, 5 },  { 3690, 10 }, { 3730, 20 },
    { 3770, 30 }, { 3800, 40 }, { 3840, 50 }, { 3870, 60 },
    { 3950, 70 }, { 4020, 80 }, { 4110, 90 }, { 4200, 100 },
};

/* Written by the sensor task, set by httpd, read by httpd and the display. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_charge_maus;
static int64_t s_charge_us; /* when the count was last moved */
static int64_t s_saved_maus;

static struct {
    int64_t us, maus;
} s_rate[RATE_SLOTS];
static int s_rate_head, s_rate_len;

/* The sensor task, httpd and the shutdown handler all save; the value is read
 * under this, so the last write to NVS is the latest count. */
static SemaphoreHandle_t s_save_mutex;

/* sensor-task private */
static int64_t         s_last_us;
static int             s_full_run;
static battery_state_t s_last_state;

static void save(void)
{
    xSemaphoreTake(s_save_mutex, portMAX_DELAY);
    taskENTER_CRITICAL(&s_lock);
    int64_t maus = s_charge_maus;
    taskEXIT_CRITICAL(&s_lock);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i64(h, NVS_KEY, maus);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        s_saved_maus = maus;
        taskEXIT_CRITICAL(&s_lock);
    } else {
        ESP_LOGW(TAG, "not saved: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_save_mutex);
}

void battery_init(void)
{
    s_save_mutex = xSemaphoreCreateMutex();

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i64(h, NVS_KEY, &s_charge_maus);
        nvs_close(h);
    }
    s_saved_maus = s_charge_maus;

    esp_register_shutdown_handler(save);

    ESP_LOGI(TAG, "count restored at %.1f mAh", s_charge_maus / MAUS_PER_MAH);
}

static battery_state_t classify(float voltage_v, float current_ma)
{
    if (current_ma > DEADBAND_MA) {
        return BATTERY_CHARGING;
    }
    if (current_ma < -DEADBAND_MA) {
        return BATTERY_DISCHARGING;
    }
    return voltage_v >= FULL_MIN_V ? BATTERY_FULL : BATTERY_IDLE;
}

static int pct_from_voltage(float voltage_v)
{
    int mv = (int)(voltage_v * 1000.0f);
    int n  = sizeof(OCV) / sizeof(OCV[0]);
    if (mv <= OCV[0].mv) {
        return 0;
    }
    for (int i = 1; i < n; i++) {
        if (mv < OCV[i].mv) {
            int span = OCV[i].mv - OCV[i - 1].mv;
            return OCV[i - 1].pct +
                   (mv - OCV[i - 1].mv) * (OCV[i].pct - OCV[i - 1].pct) / span;
        }
    }
    return 100;
}

void battery_feed(float voltage_v, float current_ma)
{
    int64_t now = esp_timer_get_time();
    int64_t dt  = now - s_last_us;
    bool integrate = s_last_us != 0 && dt <= MAX_GAP_US;
    s_last_us = now;

    battery_state_t state = classify(voltage_v, current_ma);
    if (state != BATTERY_FULL) {
        s_full_run = 0;
    } else if (s_full_run < FULL_CONFIRM) {
        s_full_run++;
    }
    /* Pinned, not just set once: days on USB would otherwise integrate the
     * INA260's offset. Also catches a boot after the charge had ended. */
    bool full = s_full_run >= FULL_CONFIRM;
    bool restart = !integrate || state != s_last_state;
    s_last_state = state;

    taskENTER_CRITICAL(&s_lock);
    if (full) {
        s_charge_maus = FULL_MAUS;
    } else if (integrate) {
        s_charge_maus += (int64_t)(current_ma * (float)dt);
    }
    s_charge_us = now;
    if (restart) {
        s_rate_len = 0;
    }
    if (s_rate_len == 0 || now - s_rate[s_rate_head].us >= RATE_STEP_US) {
        s_rate_head = (s_rate_head + 1) % RATE_SLOTS;
        s_rate[s_rate_head].us   = now;
        s_rate[s_rate_head].maus = s_charge_maus;
        if (s_rate_len < RATE_SLOTS) {
            s_rate_len++;
        }
    }
    int64_t debt = llabs(s_charge_maus - s_saved_maus);
    taskEXIT_CRITICAL(&s_lock);

    if (debt >= SAVE_STEP_MAUS || (full && debt != 0)) {
        save();
    }
}

void battery_get(battery_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    int64_t maus = s_charge_maus;
    int64_t span_us = 0, span_maus = 0;
    if (s_rate_len > 1) {
        int oldest = (s_rate_head - s_rate_len + 1 + RATE_SLOTS) % RATE_SLOTS;
        span_us   = s_charge_us - s_rate[oldest].us;
        span_maus = s_charge_maus - s_rate[oldest].maus;
    }
    taskEXIT_CRITICAL(&s_lock);

    ina260_data_t ina;
    *out = (battery_t){ .state = BATTERY_UNKNOWN, .eta_s = -1 };
    out->mah = maus / MAUS_PER_MAH;
    out->pct = out->mah * 100.0 / BATTERY_CAPACITY_MAH;
    if (!sensors_ina260_get(&ina)) {
        return;
    }
    out->ok         = true;
    out->state      = classify(ina.voltage_v, ina.current_ma);
    out->voltage_v  = ina.voltage_v;
    out->current_ma = ina.current_ma;
    out->power_mw   = ina.power_mw;
    out->pct_v      = pct_from_voltage(ina.voltage_v);

    if (span_us < RATE_MIN_US) {
        return;
    }
    double rate_ma = (double)span_maus / span_us;
    double left_maus;
    if (out->state == BATTERY_CHARGING) {
        left_maus = FULL_MAUS - maus;
    } else if (out->state == BATTERY_DISCHARGING) {
        left_maus = maus;
        rate_ma   = -rate_ma;
    } else {
        return;
    }
    if (rate_ma < RATE_MIN_MA) {
        return;
    }
    /* Linear: optimistic through the CV taper. */
    out->eta_s = left_maus > 0 ? (int)(left_maus / rate_ma / 1e6) : 0;
}

void battery_set_mah(float mah)
{
    taskENTER_CRITICAL(&s_lock);
    s_charge_maus = (int64_t)(mah * MAUS_PER_MAH);
    s_rate_len = 0;
    taskEXIT_CRITICAL(&s_lock);
    save();
}

const char *battery_state_str(battery_state_t s)
{
    switch (s) {
    case BATTERY_DISCHARGING: return "discharging";
    case BATTERY_CHARGING:    return "charging";
    case BATTERY_FULL:        return "full";
    case BATTERY_IDLE:        return "idle";
    default:                  return "unknown";
    }
}
