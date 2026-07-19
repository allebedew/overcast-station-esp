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
#include "timesync.h"

#define SNAPSHOT_MAGIC   0x48495354 /* "HIST" */
#define SNAPSHOT_VERSION 1

static const char *TAG = "history";

/* Writers: sensor task (add) and esp_timer task (tick); reader: httpd. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    const char *file; /* NULL = RAM only */
    const char *tmp;
    int len;
    int interval_s;
    int save_period_s; /* flash snapshot cadence */
    history_point_t *ring;
    int head; /* next write position */
    int count;
    /* accumulator for the slot in progress (tiers with interval > 1 s) */
    uint32_t acc_co2;
    float acc_temp, acc_rh;
    int acc_n;
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

/* Snapshot file: header + all `len` slots (only the first `count` are
 * meaningful). Byte-compatible with the old single-ring format, so a
 * pre-existing 24 h file is read back as-is after an OTA update. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    int64_t last_ts; /* unix time of the newest point; 0 = clock unsynced */
} snap_hdr_t;

/* Shared by save/restore; no overlap: restore runs once at the first
 * minute tick, the earliest save happens minutes later. */
static history_point_t s_snap_points[24 * 60]; /* sized for the largest tier */
static TaskHandle_t s_save_task;
static bool s_restored;

/* Latest raw reading, replayed into the 1 s tier while it stays fresh. */
static history_point_t s_last;
static int64_t s_last_us = INT64_MIN;
#define LAST_FRESH_US (6 * 1000000LL) /* SCD40 updates every 5 s */

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

/* Loads the snapshot into the ring: snapshot points, then a downtime gap
 * (computable only when both the snapshot and the current clock are
 * SNTP-anchored), then whatever points were collected since boot. */
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
    const history_point_t hole = { .valid = false };
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

/* Turns the readings accumulated over the tier interval into one ring
 * point. With no readings the point is a gap, so the time axis stays
 * uniform even when the sensor is offline. */
static void flush_tier(tier_t *t)
{
    taskENTER_CRITICAL(&s_lock);
    history_point_t p = { .valid = t->acc_n > 0 };
    if (p.valid) {
        p.co2_ppm = t->acc_co2 / t->acc_n;
        p.temp_cx10 = (int16_t)lroundf(10.0f * t->acc_temp / t->acc_n);
        p.rh_pct = (uint8_t)lroundf(t->acc_rh / t->acc_n);
    }
    t->acc_co2 = 0;
    t->acc_temp = 0;
    t->acc_rh = 0;
    t->acc_n = 0;
    ring_push(t, &p);
    taskEXIT_CRITICAL(&s_lock);
}

static void tick_cb(void *arg)
{
    static uint32_t ticks;
    ticks++;

    /* 1 s tier: replay the latest reading while it stays fresh */
    taskENTER_CRITICAL(&s_lock);
    history_point_t p = s_last;
    p.valid = s_last.valid &&
              esp_timer_get_time() - s_last_us <= LAST_FRESH_US;
    ring_push(&s_tiers[HISTORY_5M], &p);
    taskEXIT_CRITICAL(&s_lock);

    if (ticks % s_tiers[HISTORY_1H].interval_s == 0) {
        flush_tier(&s_tiers[HISTORY_1H]);
    }
    if (ticks % s_tiers[HISTORY_1D].interval_s == 0) {
        /* restore is deferred to the first minute tick: by now SNTP has
         * usually synced, so the downtime gap can be computed */
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

void history_add(uint16_t co2_ppm, float temp_c, float rh_pct)
{
    taskENTER_CRITICAL(&s_lock);
    for (int i = HISTORY_1H; i < HISTORY_TIER_COUNT; i++) {
        tier_t *t = &s_tiers[i];
        t->acc_co2 += co2_ppm;
        t->acc_temp += temp_c;
        t->acc_rh += rh_pct;
        t->acc_n++;
    }
    s_last = (history_point_t){
        .valid = true,
        .co2_ppm = co2_ppm,
        .temp_cx10 = (int16_t)lroundf(10.0f * temp_c),
        .rh_pct = (uint8_t)lroundf(rh_pct),
    };
    s_last_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_lock);
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
