#include "history.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "timesync.h"

#define SNAPSHOT_FILE     "/data/history.bin"
#define SNAPSHOT_TMP      "/data/history.tmp"
#define SNAPSHOT_MAGIC    0x48495354 /* "HIST" */
#define SNAPSHOT_VERSION  1
#define SAVE_PERIOD_MIN   10

static const char *TAG = "history";

/* Writers: sensor task (add) and esp_timer task (flush); reader: httpd. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static history_point_t s_ring[HISTORY_LEN];
static int s_head;  /* next write position */
static int s_count;

/* accumulator for the minute in progress */
static uint32_t s_acc_co2;
static float s_acc_temp, s_acc_rh;
static int s_acc_n;

/* Snapshot of the ring persisted to LittleFS: every SAVE_PERIOD_MIN
 * minutes and on graceful shutdown (OTA reboot). last_ts anchors the
 * newest point in absolute time so the downtime gap can be restored. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    int64_t last_ts; /* unix time of the newest point; 0 = clock unsynced */
    history_point_t points[HISTORY_LEN];
} snapshot_t;

static snapshot_t s_snap; /* shared by save/restore, ~12 KB in BSS */
static TaskHandle_t s_save_task;
static bool s_restored;

static void save_snapshot(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_snap.count = s_count;
    for (int i = 0; i < s_count; i++) {
        s_snap.points[i] = s_ring[(s_head - s_count + i + HISTORY_LEN) % HISTORY_LEN];
    }
    taskEXIT_CRITICAL(&s_lock);
    s_snap.magic = SNAPSHOT_MAGIC;
    s_snap.version = SNAPSHOT_VERSION;
    s_snap.last_ts = timesync_is_synced() ? time(NULL) : 0;

    FILE *f = fopen(SNAPSHOT_TMP, "wb");
    if (!f) {
        ESP_LOGW(TAG, "snapshot open failed");
        return;
    }
    bool ok = fwrite(&s_snap, sizeof(s_snap), 1, f) == 1;
    fclose(f);
    if (!ok || rename(SNAPSHOT_TMP, SNAPSHOT_FILE) != 0) {
        ESP_LOGW(TAG, "snapshot write failed");
        return;
    }
    ESP_LOGI(TAG, "snapshot saved (%u points)", s_snap.count);
}

/* Loads the snapshot into the ring, padding the downtime with gap
 * points when both the snapshot and the current clock are SNTP-anchored. */
static void restore_snapshot(void)
{
    FILE *f = fopen(SNAPSHOT_FILE, "rb");
    if (!f) {
        return;
    }
    bool ok = fread(&s_snap, sizeof(s_snap), 1, f) == 1;
    fclose(f);
    if (!ok || s_snap.magic != SNAPSHOT_MAGIC ||
        s_snap.version != SNAPSHOT_VERSION || s_snap.count > HISTORY_LEN) {
        ESP_LOGW(TAG, "snapshot invalid, ignored");
        return;
    }

    int gap = 0;
    if (s_snap.last_ts > 0 && timesync_is_synced()) {
        gap = (int)((time(NULL) - s_snap.last_ts) / 60) - 1;
        gap = gap < 0 ? 0 : gap > HISTORY_LEN ? HISTORY_LEN : gap;
    }

    taskENTER_CRITICAL(&s_lock);
    const history_point_t hole = { .valid = false };
    s_head = 0;
    s_count = 0;
    for (int i = 0; i < s_snap.count; i++) {
        s_ring[s_head] = s_snap.points[i];
        s_head = (s_head + 1) % HISTORY_LEN;
        if (s_count < HISTORY_LEN) s_count++;
    }
    for (int i = 0; i < gap; i++) {
        s_ring[s_head] = hole;
        s_head = (s_head + 1) % HISTORY_LEN;
        if (s_count < HISTORY_LEN) s_count++;
    }
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "snapshot restored: %u points, %d min downtime",
             s_snap.count, gap);
}

static void save_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        save_snapshot();
    }
}

/* esp_restart hook: OTA reboots keep the freshest possible history */
static void shutdown_save(void)
{
    save_snapshot();
}

/* Once a minute: turn the accumulated readings into one ring point.
 * With no readings the point is written as a gap, so the time axis
 * stays uniform even when the sensor is offline. */
static void flush_cb(void *arg)
{
    /* restore is deferred to the first tick: by now SNTP has usually
     * synced, so the downtime gap can be computed */
    if (!s_restored) {
        s_restored = true;
        restore_snapshot();
    }

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

    static int flushes;
    if (++flushes >= SAVE_PERIOD_MIN) {
        flushes = 0;
        xTaskNotifyGive(s_save_task); /* file I/O is too slow for esp_timer */
    }
}

void history_init(void)
{
    xTaskCreate(save_task, "hist_save", 3072, NULL, 1, &s_save_task);
    ESP_ERROR_CHECK(esp_register_shutdown_handler(shutdown_save));

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
