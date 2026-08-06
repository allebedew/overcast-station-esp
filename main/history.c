#include "history.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "climate.h"
#include "ld2450.h"
#include "timesync.h"

#define SNAPSHOT_MAGIC   0x48495354 /* "HIST" */
/* Bumped when history_point_t changes shape: an older file is then dropped
 * rather than reinterpreted. */
#define SNAPSHOT_VERSION 3

static const char *TAG = "history";

/* Ring writer: the esp_timer task; readers: httpd and the snapshot task. The
 * accumulators need no lock — only the timer callback touches them. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* One quantity's running mean over the slot in progress. */
typedef struct {
    float sum;
    int n;
} acc_t;

typedef struct {
    const char *file; /* NULL = RAM only */
    const char *tmp;
    int len;
    int interval_s;
    int save_period_s; /* flash snapshot cadence */
    history_point_t *ring;
    int head; /* next write position */
    int count;
    /* Slot in progress, counted per quantity so one sensor dropping out does
     * not turn the whole slot into a gap. `near` runs only while a target
     * exists — averaging an empty room in as zero metres would bend the line
     * toward the sensor every time the room empties — so the radar needs a
     * sample count of its own to tell "nobody there" from "not recorded". */
    acc_t co2, temp, rh, press, lux;
    acc_t near;
    int radar_n;
    uint8_t max_targets;
} tier_t;

static history_point_t s_ring_5m[5 * 60];
static history_point_t s_ring_1h[60 * 60 / 5];
static history_point_t s_ring_1d[24 * 60];

static tier_t s_tiers[HISTORY_TIER_COUNT] = {
    [HISTORY_5M] = {
        .len = 5 * 60,
        .interval_s = 1,
        .ring = s_ring_5m,
    },
    [HISTORY_1H] = {
        .file = "/data/hist_1h.bin",
        .tmp = "/data/hist_1h.tmp",
        .len = 60 * 60 / 5,
        .interval_s = 5,
        .save_period_s = 5 * 60,
        .ring = s_ring_1h,
    },
    [HISTORY_1D] = {
        .file = "/data/history.bin",
        .tmp = "/data/history.tmp",
        .len = 24 * 60,
        .interval_s = 60,
        .save_period_s = 10 * 60,
        .ring = s_ring_1d,
    },
};

/* Snapshot file: header + all `len` slots, of which `count` are meaningful. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    int64_t last_ts; /* unix time of the newest point; 0 = clock unsynced */
} snap_hdr_t;

/* Shared by save/restore: restore runs once at the first minute tick, the
 * earliest save minutes later. */
static history_point_t s_snap_points[24 * 60]; /* sized for the largest tier */
static TaskHandle_t s_save_task;
static bool s_restored;

/* Callers hold s_lock. */
static void ring_push(tier_t *t, const history_point_t *p)
{
    t->ring[t->head] = *p;
    t->head = (t->head + 1) % t->len;
    if (t->count < t->len) {
        t->count++;
    }
}

static void save_snapshot(tier_t *t)
{
    snap_hdr_t hdr = {
        .magic = SNAPSHOT_MAGIC,
        .version = SNAPSHOT_VERSION,
        .last_ts = timesync_is_synced() ? time(NULL) : 0,
    };
    taskENTER_CRITICAL(&s_lock);
    hdr.count = t->count;
    for (int i = 0; i < t->count; i++) {
        s_snap_points[i] = t->ring[(t->head - t->count + i + t->len) % t->len];
    }
    taskEXIT_CRITICAL(&s_lock);
    memset(&s_snap_points[hdr.count], 0,
           (t->len - hdr.count) * sizeof(history_point_t));

    FILE *f = fopen(t->tmp, "wb");
    if (!f) {
        ESP_LOGW(TAG, "%s: open failed", t->tmp);
        return;
    }
    bool ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1 &&
              fwrite(s_snap_points, sizeof(history_point_t), t->len, f)
                  == (size_t)t->len;
    fclose(f);
    if (!ok || rename(t->tmp, t->file) != 0) {
        ESP_LOGW(TAG, "%s: write failed", t->file);
        return;
    }
    ESP_LOGI(TAG, "%s: saved %u points", t->file, hdr.count);
}

/* Snapshot points, then a downtime gap (computable only when both clocks are
 * SNTP-anchored), then the points collected since boot. */
static void restore_snapshot(tier_t *t)
{
    FILE *f = fopen(t->file, "rb");
    if (!f) {
        return;
    }
    snap_hdr_t hdr;
    bool ok = fread(&hdr, sizeof(hdr), 1, f) == 1 &&
              hdr.magic == SNAPSHOT_MAGIC &&
              hdr.version == SNAPSHOT_VERSION && hdr.count <= t->len &&
              fread(s_snap_points, sizeof(history_point_t), hdr.count, f)
                  == hdr.count;
    fclose(f);
    if (!ok) {
        ESP_LOGW(TAG, "%s: invalid snapshot, ignored", t->file);
        return;
    }

    /* restore runs at the first minute tick, so the ring holds at most
     * 60 / interval points collected since boot */
    history_point_t fresh[60 / 5];
    int fresh_n;

    taskENTER_CRITICAL(&s_lock);
    fresh_n = t->count < (int)(sizeof(fresh) / sizeof(fresh[0]))
                  ? t->count
                  : (int)(sizeof(fresh) / sizeof(fresh[0]));
    for (int i = 0; i < fresh_n; i++) {
        fresh[i] = t->ring[(t->head - fresh_n + i + t->len) % t->len];
    }
    taskEXIT_CRITICAL(&s_lock);

    int gap = 0;
    if (hdr.last_ts > 0 && timesync_is_synced()) {
        gap = (int)((time(NULL) - hdr.last_ts) / t->interval_s) - fresh_n - 1;
        gap = gap < 0 ? 0 : gap > t->len ? t->len : gap;
    }

    taskENTER_CRITICAL(&s_lock);
    const history_point_t hole = { .have = 0 };
    t->head = 0;
    t->count = 0;
    for (int i = 0; i < hdr.count; i++) {
        ring_push(t, &s_snap_points[i]);
    }
    for (int i = 0; i < gap; i++) {
        ring_push(t, &hole);
    }
    for (int i = 0; i < fresh_n; i++) {
        ring_push(t, &fresh[i]);
    }
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "%s: restored %u points, %d slot gap",
             t->file, hdr.count, gap);
}

static void save_task(void *arg)
{
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
        for (int i = 0; i < HISTORY_TIER_COUNT; i++) {
            if ((bits & (1u << i)) && s_tiers[i].file) {
                save_snapshot(&s_tiers[i]);
            }
        }
    }
}

/* esp_restart hook: OTA reboots keep the freshest possible history */
static void shutdown_save(void)
{
    for (int i = 0; i < HISTORY_TIER_COUNT; i++) {
        if (s_tiers[i].file) {
            save_snapshot(&s_tiers[i]);
        }
    }
}

static void acc_add(acc_t *a, bool ok, float value)
{
    if (ok) {
        a->sum += value;
        a->n++;
    }
}

/* Feeds one sample into every averaging tier. */
static void acc_sample(const climate_t *c, const ld2450_data_t *r, bool radar_ok)
{
    for (int i = HISTORY_1H; i < HISTORY_TIER_COUNT; i++) {
        tier_t *t = &s_tiers[i];
        acc_add(&t->co2, c->co2_ok, c->co2_ppm);
        acc_add(&t->temp, c->temp_ok, c->temp_c);
        acc_add(&t->rh, c->rh_ok, c->rh_pct);
        acc_add(&t->press, c->press_ok, c->press_hpa);
        acc_add(&t->lux, c->lux_ok, c->lux);
        acc_add(&t->near, radar_ok && r->count > 0, r->nearest_m);
        if (radar_ok) {
            t->radar_n++;
            if (r->count > t->max_targets) {
                t->max_targets = r->count;
            }
        }
    }
}

/* One climate reading as a ring point; a missing sensor leaves its bit clear. */
static history_point_t point_of(const climate_t *c)
{
    history_point_t p = {0};

    if (c->co2_ok) {
        p.co2_ppm = c->co2_ppm;
        p.have |= HISTORY_HAS_CO2;
    }
    if (c->temp_ok) {
        p.temp_cx100 = (int16_t)lroundf(100.0f * c->temp_c);
        p.have |= HISTORY_HAS_TEMP;
    }
    if (c->rh_ok) {
        p.rh_dpct = (uint16_t)lroundf(10.0f * c->rh_pct);
        p.have |= HISTORY_HAS_RH;
    }
    if (c->press_ok) {
        p.press_mhpa = (int32_t)lroundf(1000.0f * c->press_hpa);
        p.have |= HISTORY_HAS_PRESS;
    }
    if (c->lux_ok) {
        p.lux = c->lux;
        p.have |= HISTORY_HAS_LUX;
    }
    return p;
}

/* The radar's slot into its byte. The distance keeps a step of its own even
 * when it rounds to nothing, so that a stored zero means an empty fan and only
 * that. */
static void point_set_radar(history_point_t *p, uint8_t targets, bool near_ok,
                            float near_m)
{
    long steps = near_ok ? lroundf(near_m / HISTORY_RADAR_STEP_M) : 0;
    if (near_ok && steps < 1) {
        steps = 1;
    }
    if (steps > 31) {
        steps = 31;
    }
    if (targets > 3) {
        targets = 3;
    }
    p->radar = (uint8_t)(targets | (steps << 2));
    p->have |= HISTORY_HAS_RADAR;
}

/* The tier's accumulated samples as one ring point. With no samples at all it
 * is a gap, so the time axis stays uniform even when everything is offline. */
static void flush_tier(tier_t *t)
{
    climate_t mean = {0};

    if (t->co2.n) {
        mean.co2_ok = true;
        mean.co2_ppm = (uint16_t)lroundf(t->co2.sum / t->co2.n);
    }
    if (t->temp.n) {
        mean.temp_ok = true;
        mean.temp_c = t->temp.sum / t->temp.n;
    }
    if (t->rh.n) {
        mean.rh_ok = true;
        mean.rh_pct = t->rh.sum / t->rh.n;
    }
    if (t->press.n) {
        mean.press_ok = true;
        mean.press_hpa = t->press.sum / t->press.n;
    }
    if (t->lux.n) {
        mean.lux_ok = true;
        mean.lux = t->lux.sum / t->lux.n;
    }
    history_point_t p = point_of(&mean);
    if (t->radar_n) {
        point_set_radar(&p, t->max_targets, t->near.n > 0,
                        t->near.n ? t->near.sum / t->near.n : 0.0f);
    }

    t->co2 = (acc_t){0};
    t->temp = (acc_t){0};
    t->rh = (acc_t){0};
    t->press = (acc_t){0};
    t->lux = (acc_t){0};
    t->near = (acc_t){0};
    t->radar_n = 0;
    t->max_targets = 0;

    taskENTER_CRITICAL(&s_lock);
    ring_push(t, &p);
    taskEXIT_CRITICAL(&s_lock);
}

static void tick_cb(void *arg)
{
    static uint32_t ticks;
    ticks++;

    /* Sampling, not listening: a sensor slower than 1 Hz repeats its value into
     * the 1 s tier, which draws better than four gaps out of five. */
    climate_t c;
    climate_get(&c);
    ld2450_data_t r = {0};
    bool radar_ok = ld2450_get(&r);
    acc_sample(&c, &r, radar_ok);

    history_point_t p = point_of(&c);
    if (radar_ok) {
        /* A single sample has no slot to take a maximum over, so the 1 s tier
         * carries the raw count and flickers with it: a still person leaves the
         * frame for seconds at a time. The averaging tiers are where the
         * maximum makes that whole again. */
        point_set_radar(&p, r.count, r.count > 0, r.nearest_m);
    }
    taskENTER_CRITICAL(&s_lock);
    ring_push(&s_tiers[HISTORY_5M], &p);
    taskEXIT_CRITICAL(&s_lock);

    if (ticks % s_tiers[HISTORY_1H].interval_s == 0) {
        flush_tier(&s_tiers[HISTORY_1H]);
    }
    if (ticks % s_tiers[HISTORY_1D].interval_s == 0) {
        /* deferred to the first minute tick, by when SNTP has usually synced
         * and the downtime gap can be computed */
        if (!s_restored) {
            s_restored = true;
            for (int i = 0; i < HISTORY_TIER_COUNT; i++) {
                if (s_tiers[i].file) {
                    restore_snapshot(&s_tiers[i]);
                }
            }
        }
        flush_tier(&s_tiers[HISTORY_1D]);
    }

    uint32_t save_bits = 0;
    for (int i = 0; i < HISTORY_TIER_COUNT; i++) {
        if (s_tiers[i].file && ticks % s_tiers[i].save_period_s == 0) {
            save_bits |= 1u << i;
        }
    }
    if (save_bits) {
        /* file I/O is too slow for the esp_timer task */
        xTaskNotify(s_save_task, save_bits, eSetBits);
    }
}

void history_init(void)
{
    xTaskCreate(save_task, "hist_save", 3072, NULL, 1, &s_save_task);
    ESP_ERROR_CHECK(esp_register_shutdown_handler(shutdown_save));

    const esp_timer_create_args_t args = {
        .callback = tick_cb,
        .name = "history",
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 1000000ULL));
}

int history_count(history_tier_t tier)
{
    taskENTER_CRITICAL(&s_lock);
    int count = s_tiers[tier].count;
    taskEXIT_CRITICAL(&s_lock);
    return count;
}

int history_interval(history_tier_t tier)
{
    return s_tiers[tier].interval_s;
}

bool history_get(history_tier_t tier, int idx, history_point_t *out)
{
    tier_t *t = &s_tiers[tier];
    taskENTER_CRITICAL(&s_lock);
    bool ok = idx >= 0 && idx < t->count;
    if (ok) {
        *out = t->ring[(t->head - t->count + idx + t->len) % t->len];
    }
    taskEXIT_CRITICAL(&s_lock);
    return ok;
}

/* One quantity out of a slot, in stored units. False leaves *out alone, so a
 * caller scanning for a non-gap can keep its own fallback there. */
static bool point_value(const history_point_t *p, history_quantity_t q, float *out)
{
    switch (q) {
    case HISTORY_Q_TEMP:
        if (!(p->have & HISTORY_HAS_TEMP)) { return false; }
        *out = p->temp_cx100 / 100.0f;
        return true;
    case HISTORY_Q_RH:
        if (!(p->have & HISTORY_HAS_RH)) { return false; }
        *out = p->rh_dpct / 10.0f;
        return true;
    case HISTORY_Q_PRESS:
        if (!(p->have & HISTORY_HAS_PRESS)) { return false; }
        *out = p->press_mhpa / 1000.0f;
        return true;
    case HISTORY_Q_CO2:
        if (!(p->have & HISTORY_HAS_CO2)) { return false; }
        *out = p->co2_ppm;
        return true;
    case HISTORY_Q_LUX:
        if (!(p->have & HISTORY_HAS_LUX)) { return false; }
        *out = p->lux;
        return true;
    case HISTORY_Q_COUNT:
        break;
    }
    return false;
}

int history_series(history_tier_t tier, history_quantity_t q, int stride,
                   float *v, int n)
{
    if (stride < 1) { stride = 1; }

    int count = history_count(tier);
    int cols  = count > 0 ? (count - 1) / stride + 1 : 0;
    if (cols > n) { cols = n; }

    for (int k = 0; k < cols; k++) {
        int   idx = count - 1 - k * stride;
        float raw = NAN;
        /* The column stands for its whole stride, so a gap in the slot the
         * decimation lands on falls back down the window instead of breaking
         * the line. */
        for (int j = 0; j < stride && idx - j >= 0; j++) {
            history_point_t p;
            if (history_get(tier, idx - j, &p) && point_value(&p, q, &raw)) {
                break;
            }
        }
        v[cols - 1 - k] = raw;
    }

    if (q == HISTORY_Q_PRESS) {
        /* Stored as measured, plotted reduced, like /api/history. The reduction
         * is linear, so the series costs one powf rather than one per point. */
        float k_msl = climate_to_sea_level(1.0f);
        for (int k = 0; k < cols; k++) {
            v[k] *= k_msl;
        }
    }
    return cols;
}

void history_reset(void)
{
    /* count = 0 makes the stored points unreachable (history_get bounds-checks
     * on it), so the ring arrays need no clearing. */
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < HISTORY_TIER_COUNT; i++) {
        tier_t *t = &s_tiers[i];
        t->head = 0;
        t->count = 0;
        t->co2 = (acc_t){0};
        t->temp = (acc_t){0};
        t->rh = (acc_t){0};
        t->press = (acc_t){0};
        t->lux = (acc_t){0};
    }
    taskEXIT_CRITICAL(&s_lock);

    for (int i = 0; i < HISTORY_TIER_COUNT; i++) {
        if (s_tiers[i].file) {
            remove(s_tiers[i].file);
        }
    }
    ESP_LOGI(TAG, "history reset by user");
}
