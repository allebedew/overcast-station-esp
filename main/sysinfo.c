#include "sysinfo.h"

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

/* The idle and total run-time counters are global, so the load is measured
 * once for everyone: the display asks 60 times a second and the HTTP API
 * whenever a browser polls, and both get the figure from the same window.
 * Measuring per caller would give each of them a window of its own polling
 * rate — and with two browsers open, windows that interleave and cut each
 * other short. */
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

    /* Anchor the first window here: without this the first reading would
     * average everything since boot, which is not what "load right now"
     * means. */
    s_prev_idle = ulTaskGetIdleRunTimeCounter();
    s_prev_total = (uint32_t)esp_timer_get_time();
    s_next_us = esp_timer_get_time() + SYSINFO_CPU_WINDOW_US;

    ESP_LOGI(TAG, "system info ready");
}

float sysinfo_chip_temp_c(void)
{
    float temp = -273;
    temperature_sensor_get_celsius(s_temp_sensor, &temp);
    return temp;
}

/* The counters are 32-bit microseconds; the ~71-minute wrap comes out right
 * in the uint32_t subtraction. */
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

void sysinfo_log_tasks(void)
{
    static char task_list[1024];
    vTaskList(task_list);
    ESP_LOGI(TAG, "Task list:\nName          State  Prio  Stack  Num\n%s",
             task_list);
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
