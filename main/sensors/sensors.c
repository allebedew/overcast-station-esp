#include "sensors.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "climate.h"
#include "i2c_bus.h"

/* One task walks every sensor: it ticks at SENSOR_TICK_MS and each sensor comes
 * up at its own period. Hot-plug is shared — probe until the device answers,
 * then read, and MAX_ERRORS consecutive failures drop it back to probing. */

/* One FreeRTOS tick — the granularity of every period below, which is why they
 * are multiples of it (and why the light sensor's 8 Hz ceiling is 130 ms and
 * not the round 125). */
#define SENSOR_TICK_MS  10
#define PROBE_PERIOD_MS 5000 /* how often to look for an absent sensor */
#define MAX_ERRORS      3    /* consecutive failures before "offline" */

/* Each sensor is read at the rate it produces new results. The VEML7700 gates
 * itself on the integration time below this cap; the SCD40's 5 s cycle is fixed
 * in the part, and 1 Hz is how its phase is found again after a restart. */
#define SCD40_PERIOD_MS    1000
#define TMP117_PERIOD_MS   250
#define BMP581_PERIOD_MS   250
#define VEML7700_PERIOD_MS 130

/* The SCD40 needs ambient pressure it cannot measure itself, so the BMP581's
 * reading is forwarded; the fallback is the ISA pressure at the configured site
 * altitude. Resent only on a real step — smaller ones the compensation would
 * not notice anyway. */
#define SCD40_PRESSURE_MIN   700
#define SCD40_PRESSURE_MAX   1200
#define SCD40_PRESSURE_STEP  2 /* hPa of drift worth another write */

static const char *TAG = "sensors";

/* Staging buffer and published snapshot; a union rather than void* + length so
 * the copies below stay type-checked. */
typedef union {
    scd40_data_t scd40;
    tmp117_data_t tmp117;
    bmp581_data_t bmp581;
    veml7700_data_t veml7700;
} sensor_reading_t;

typedef struct {
    const char *name;
    int period_ms;
    esp_err_t (*start)(void);
    esp_err_t (*read)(sensor_reading_t *out);
    /* Optional; run without the bus lock held, so a hook that needs the bus
     * takes it itself. */
    void (*on_start)(void);
    void (*on_reading)(const sensor_reading_t *r);

    /* poll-task private */
    bool running;
    bool announced; /* its absence is already in the log */
    int errors;
    int64_t next_us;
    int64_t probe_at_us;

    /* published, guarded by s_lock */
    bool valid;
    sensor_reading_t snapshot;
} sensor_t;

/* Adapters over the drivers' own reading types — the table entries stay
 * type-safe instead of casting function pointers. */
static esp_err_t scd40_read_any(sensor_reading_t *r) { return scd40_read(&r->scd40); }
static esp_err_t tmp117_read_any(sensor_reading_t *r) { return tmp117_read(&r->tmp117); }
static esp_err_t bmp581_read_any(sensor_reading_t *r) { return bmp581_read(&r->bmp581); }
static esp_err_t veml7700_read_any(sensor_reading_t *r) { return veml7700_read(&r->veml7700); }

static void scd40_started(void);
static void bmp581_published(const sensor_reading_t *r);

/* Written by the poll task, read by httpd, the display, the LED and alerts. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

enum { SENSOR_SCD40, SENSOR_TMP117, SENSOR_BMP581, SENSOR_VEML7700, SENSOR_COUNT };

static sensor_t s_sensors[SENSOR_COUNT] = {
    [SENSOR_SCD40] = { .name = "SCD40", .period_ms = SCD40_PERIOD_MS,
                       .start = scd40_start, .read = scd40_read_any,
                       .on_start = scd40_started },
    [SENSOR_TMP117] = { .name = "TMP117", .period_ms = TMP117_PERIOD_MS,
                        .start = tmp117_start, .read = tmp117_read_any },
    [SENSOR_BMP581] = { .name = "BMP581", .period_ms = BMP581_PERIOD_MS,
                        .start = bmp581_start, .read = bmp581_read_any,
                        .on_reading = bmp581_published },
    [SENSOR_VEML7700] = { .name = "VEML7700", .period_ms = VEML7700_PERIOD_MS,
                          .start = veml7700_start, .read = veml7700_read_any },
};

static bool sensor_get(sensor_t *s, sensor_reading_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    bool valid = s->valid;
    if (valid) {
        *out = s->snapshot;
    }
    taskEXIT_CRITICAL(&s_lock);
    return valid;
}

/* ---------------- cross-sensor wiring ---------------- */

/* What the SCD40 was last told, hPa; 0 = nothing since it started. */
static uint16_t s_scd40_hpa;

static void scd40_sync_pressure(void)
{
    if (!s_sensors[SENSOR_SCD40].running) {
        return;
    }

    uint16_t hpa = (uint16_t)(climate_standard_pressure_hpa() + 0.5f);
    sensor_reading_t r;
    if (sensor_get(&s_sensors[SENSOR_BMP581], &r)) {
        int measured = (int)(r.bmp581.press_hpa + 0.5f);
        if (measured >= SCD40_PRESSURE_MIN && measured <= SCD40_PRESSURE_MAX) {
            hpa = (uint16_t)measured;
        }
    }

    if (s_scd40_hpa != 0 && abs((int)hpa - (int)s_scd40_hpa) < SCD40_PRESSURE_STEP) {
        return;
    }

    i2c_bus_lock();
    esp_err_t err = scd40_set_pressure(hpa);
    i2c_bus_unlock();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SCD40: failed to set ambient pressure: %s",
                 esp_err_to_name(err));
        return; /* leave s_scd40_hpa alone so the next reading retries */
    }
    ESP_LOGI(TAG, "SCD40 pressure compensation: %u hPa", hpa);
    s_scd40_hpa = hpa;
}

/* The sensor comes up uncompensated, so tell it the pressure straight away. */
static void scd40_started(void)
{
    s_scd40_hpa = 0;
    scd40_sync_pressure();
}

static void bmp581_published(const sensor_reading_t *r)
{
    (void)r; /* scd40_sync_pressure() reads the published snapshot */
    scd40_sync_pressure();
}

/* ---------------- polling ---------------- */

/* One line per disappearance, not one per probe: an empty bus would otherwise
 * fill the log every PROBE_PERIOD_MS, and it is the boot log that gets read. */
static void announce_absent(sensor_t *s, esp_err_t err)
{
    if (s->announced) {
        return;
    }
    s->announced = true;
    ESP_LOGE(TAG, "%s absent (%s), probing every %d s", s->name,
             esp_err_to_name(err), PROBE_PERIOD_MS / 1000);
}

static void set_offline(sensor_t *s, int64_t now)
{
    taskENTER_CRITICAL(&s_lock);
    s->valid = false;
    taskEXIT_CRITICAL(&s_lock);
    s->running = false;
    s->probe_at_us = now + (int64_t)PROBE_PERIOD_MS * 1000;
}

/* True when the sensor asked to be revisited promptly — a sequence in
 * progress, not a reading. */
static bool sensor_step(sensor_t *s)
{
    int64_t now = esp_timer_get_time();

    if (!s->running) {
        if (now < s->probe_at_us) {
            return false;
        }
        i2c_bus_lock();
        esp_err_t err = s->start();
        i2c_bus_unlock();

        if (err == ESP_ERR_NOT_FINISHED) {
            /* the wait is the sensor's, not the bus's: resume next tick and
             * leave the bus free meanwhile */
            return true;
        }
        if (err != ESP_OK) {
            announce_absent(s, err);
            set_offline(s, now);
            return false;
        }
        s->running = true;
        s->errors = 0;
        s->announced = false;
        if (s->on_start) {
            s->on_start();
        }
        return true; /* first conversion under the new settings is not in yet */
    }

    sensor_reading_t r;
    i2c_bus_lock();
    esp_err_t err = s->read(&r);
    i2c_bus_unlock();

    if (err == ESP_ERR_NOT_FINISHED) {
        /* Nothing new yet, and not a failure. Waiting a full period per step
         * would cost seconds over a VEML7700 range walk. */
        return true;
    }
    if (err != ESP_OK) {
        if (++s->errors >= MAX_ERRORS) {
            announce_absent(s, err);
            set_offline(s, now);
        }
        return false;
    }
    s->errors = 0;

    taskENTER_CRITICAL(&s_lock);
    s->snapshot = r;
    s->valid = true;
    taskEXIT_CRITICAL(&s_lock);

    if (s->on_reading) {
        s->on_reading(&r);
    }
    return false;
}

static void sensors_task(void *arg)
{
    for (;;) {
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < SENSOR_COUNT; i++) {
            sensor_t *s = &s_sensors[i];
            if (now < s->next_us) {
                continue;
            }
            /* measured from now, so a poll held up by the bus lock shifts the
             * schedule instead of firing a catch-up burst */
            s->next_us = now + (int64_t)s->period_ms * 1000;
            if (sensor_step(s)) {
                s->next_us = 0; /* mid-sequence: next tick, not next period */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_TICK_MS));
    }
}

/* ---------------- public snapshots ---------------- */

bool sensors_scd40_get(scd40_data_t *out)
{
    sensor_reading_t r;
    bool valid = sensor_get(&s_sensors[SENSOR_SCD40], &r);
    if (valid) {
        *out = r.scd40;
    }
    return valid;
}

bool sensors_tmp117_get(tmp117_data_t *out)
{
    sensor_reading_t r;
    bool valid = sensor_get(&s_sensors[SENSOR_TMP117], &r);
    if (valid) {
        *out = r.tmp117;
    }
    return valid;
}

bool sensors_bmp581_get(bmp581_data_t *out)
{
    sensor_reading_t r;
    bool valid = sensor_get(&s_sensors[SENSOR_BMP581], &r);
    if (valid) {
        *out = r.bmp581;
    }
    return valid;
}

bool sensors_veml7700_get(veml7700_data_t *out)
{
    sensor_reading_t r;
    bool valid = sensor_get(&s_sensors[SENSOR_VEML7700], &r);
    if (valid) {
        *out = r.veml7700;
    }
    return valid;
}

esp_err_t sensors_scd40_calibrate(uint16_t target_ppm, int *correction_ppm)
{
    i2c_bus_lock();
    /* "running" belongs to the poll task; a stale read here only costs a bus
     * round trip that fails and reports back anyway. */
    if (!s_sensors[SENSOR_SCD40].running) {
        i2c_bus_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = scd40_calibrate(target_ppm, correction_ppm);
    i2c_bus_unlock();
    return err;
}

void sensors_init(void)
{
    scd40_init(); /* the only driver whose device handle is fixed up front */
    xTaskCreate(sensors_task, "sensors", 3072, NULL, 2, NULL);
}
