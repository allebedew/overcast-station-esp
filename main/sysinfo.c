#include "sysinfo.h"

#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/temperature_sensor.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "sysinfo";

static temperature_sensor_handle_t s_temp_sensor;

static sysinfo_static_t s_static;

static void cpu_log_task(void *arg);

/* The run-time counters are global, so the load is measured once for everyone.
 * Per-caller windows would follow each caller's polling rate, and two open
 * browsers would cut each other's windows short. */
static portMUX_TYPE s_cpu_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_prev_idle, s_prev_total;
static int64_t s_next_us;
static int s_load;

/* Everything that cannot change until the next boot, read once. */
static void collect_static(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    s_static.app_version = app->version;
    s_static.build_date = app->date;
    s_static.build_time = app->time;
    s_static.idf_ver = app->idf_ver;

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    s_static.chip_rev_major = chip.revision / 100;
    s_static.chip_rev_minor = chip.revision % 100;
    s_static.cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    s_static.flash_mb = flash_size / (1024 * 1024);

    nvs_stats_t nvs = {0};
    nvs_get_stats(NULL, &nvs);
    s_static.nvs_used_entries = nvs.used_entries;
    s_static.nvs_total_entries = nvs.total_entries;
}

void sysinfo_init(void)
{
    collect_static();

    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    ESP_ERROR_CHECK(temperature_sensor_install(&cfg, &s_temp_sensor));
    ESP_ERROR_CHECK(temperature_sensor_enable(s_temp_sensor));

    /* Anchor the first window, or the first reading averages all of boot. */
    s_prev_idle = ulTaskGetIdleRunTimeCounter();
    s_prev_total = (uint32_t)esp_timer_get_time();
    s_next_us = esp_timer_get_time() + SYSINFO_CPU_WINDOW_US;

    xTaskCreate(cpu_log_task, "cpu_log", 3072, NULL, 1, NULL);

    ESP_LOGI(TAG, "system info ready");
}

float sysinfo_chip_temp_c(void)
{
    float temp = -273;
    temperature_sensor_get_celsius(s_temp_sensor, &temp);
    return temp;
}

/* 32-bit microseconds; the ~71-minute wrap comes out right in the subtraction. */
int sysinfo_cpu_load_percent(void)
{
    int64_t now = esp_timer_get_time();

    taskENTER_CRITICAL(&s_cpu_lock);
    if (now >= s_next_us) {
        s_next_us = now + SYSINFO_CPU_WINDOW_US;

        uint32_t idle = ulTaskGetIdleRunTimeCounter();
        uint32_t total = (uint32_t)now;
        uint32_t d_idle = idle - s_prev_idle;
        uint32_t d_total = total - s_prev_total;
        s_prev_idle = idle;
        s_prev_total = total;

        if (d_total != 0 && d_idle <= d_total) {
            s_load = 100 - (int)((uint64_t)100 * d_idle / d_total);
        }
    }
    int load = s_load;
    taskEXIT_CRITICAL(&s_cpu_lock);

    return load;
}

const sysinfo_static_t *sysinfo_static(void)
{
    return &s_static;
}

void sysinfo_get_runtime(sysinfo_runtime_t *out)
{
    out->uptime_s = esp_timer_get_time() / 1000000;
    out->cpu_load_pct = sysinfo_cpu_load_percent();
    out->tasks = uxTaskGetNumberOfTasks();
    out->heap_free = esp_get_free_heap_size();
    out->heap_min = esp_get_minimum_free_heap_size();
    out->heap_total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    out->heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
}

/* Per-task CPU share, sampled over the whole period: one snapshot per tick,
 * each line the delta against the previous one. Tasks that came or went in
 * between have no counterpart and are skipped. */
#define CPU_LOG_PERIOD_MS 30000

typedef struct {
    TaskHandle_t handle;
    uint32_t runtime;
} task_run_t;

typedef struct {
    const char *name;
    unsigned pct10; /* tenths of a percent */
} task_load_t;

static int load_cmp(const void *a, const void *b)
{
    return (int)((const task_load_t *)b)->pct10 - (int)((const task_load_t *)a)->pct10;
}

static void cpu_log_task(void *arg)
{
    (void)arg;

    task_run_t *prev = NULL;
    unsigned prev_count = 0;
    int64_t prev_us = 0;

    for (;;) {
        /* Slack for tasks started between the count and the walk. */
        unsigned cap = uxTaskGetNumberOfTasks() + 4;
        TaskStatus_t *cur = malloc(cap * sizeof(*cur));
        task_run_t *next = malloc(cap * sizeof(*next));
        task_load_t *load = malloc(cap * sizeof(*load));
        if (cur == NULL || next == NULL || load == NULL) {
            ESP_LOGW(TAG, "CPU usage: out of memory");
            free(cur);
            free(next);
            free(load);
            vTaskDelay(pdMS_TO_TICKS(CPU_LOG_PERIOD_MS));
            continue;
        }

        unsigned count = uxTaskGetSystemState(cur, cap, NULL);
        int64_t now = esp_timer_get_time();
        uint32_t window = (uint32_t)now - (uint32_t)prev_us;

        unsigned n = 0;
        for (unsigned i = 0; i < count; i++) {
            next[i].handle = cur[i].xHandle;
            next[i].runtime = cur[i].ulRunTimeCounter;

            for (unsigned j = 0; j < prev_count; j++) {
                if (prev[j].handle != cur[i].xHandle) {
                    continue;
                }
                uint32_t d = cur[i].ulRunTimeCounter - prev[j].runtime;
                if (d > window) {
                    break; /* counter wrapped past the window; drop the line */
                }
                load[n].name = cur[i].pcTaskName;
                load[n].pct10 = window ? (unsigned)((uint64_t)1000 * d / window) : 0;
                n++;
                break;
            }
        }

        if (n > 0) {
            qsort(load, n, sizeof(*load), load_cmp);

            /* configMAX_TASK_NAME_LEN plus the padded percentage. */
            size_t len = n * (configMAX_TASK_NAME_LEN + 12) + 1;
            char *text = malloc(len);
            if (text != NULL) {
                size_t off = 0;
                unsigned quiet = 0;
                for (unsigned i = 0; i < n && off < len; i++) {
                    if (load[i].pct10 == 0) {
                        quiet++;
                        continue;
                    }
                    off += snprintf(text + off, len - off, "%-16s %3u.%u%%\n",
                                    load[i].name, load[i].pct10 / 10,
                                    load[i].pct10 % 10);
                }
                if (quiet > 0 && off < len) {
                    snprintf(text + off, len - off, "%u task(s) below 0.1%%\n", quiet);
                }
                ESP_LOGI(TAG, "CPU usage over %us:\n%s",
                         (unsigned)(window / 1000000), text);
                free(text);
            }
        }

        free(prev);
        free(cur);
        free(load);
        prev = next;
        prev_count = count;
        prev_us = now;

        vTaskDelay(pdMS_TO_TICKS(CPU_LOG_PERIOD_MS));
    }
}

void sysinfo_log_tasks(void)
{
    /* vTaskList() writes without bounds checking, so the buffer is sized from
     * the task count: ~40 bytes a line, plus slack for tasks started between
     * the count and the walk. */
    size_t len = (uxTaskGetNumberOfTasks() + 4) * (configMAX_TASK_NAME_LEN + 32);
    char *task_list = malloc(len);
    if (task_list == NULL) {
        ESP_LOGW(TAG, "Task list: %u bytes not available", (unsigned)len);
        return;
    }

    /* Empty when uxTaskGetSystemState() failed to allocate, which it does not
     * report otherwise. */
    task_list[0] = '\0';
    vTaskList(task_list);
    if (task_list[0] == '\0') {
        ESP_LOGW(TAG, "Task list unavailable");
    } else {
        ESP_LOGI(TAG, "Task list:\nName          State  Prio  Stack  Num\n%s",
                 task_list);
    }
    free(task_list);
}

const char *sysinfo_reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default:                return "other";
    }
}
