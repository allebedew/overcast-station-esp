#pragma once

#include <stdint.h>

/* How the station itself is doing, as opposed to the air around it. Two
 * shapes, by how often the answer changes: what was fixed at build or boot time
 * is a snapshot, what moves while it runs is sampled per call. */

void sysinfo_init(void);

/* Fixed for the life of this boot. Never NULL; the strings live in the
 * firmware image and outlive any caller. */
typedef struct {
    const char *app_version;
    const char *build_date; /* __DATE__ of the build */
    const char *build_time; /* __TIME__ of the build */
    const char *idf_ver;
    int chip_rev_major;
    int chip_rev_minor;
    int cpu_mhz;
    uint32_t flash_mb;

    /* NVS as it was at boot: nvs_get_stats() walks the partition's page
     * headers, too much for a per-second status poll. */
    uint32_t nvs_used_entries;
    uint32_t nvs_total_entries;
} sysinfo_static_t;

const sysinfo_static_t *sysinfo_static(void);

/* Sampled per call — one IDF call per field. */
typedef struct {
    int64_t uptime_s;
    int cpu_load_pct;
    unsigned tasks;
    unsigned heap_free;
    unsigned heap_min;     /* low-water mark since boot */
    unsigned heap_total;
    unsigned heap_largest; /* largest free block — heap fragmentation */
} sysinfo_runtime_t;

void sysinfo_get_runtime(sysinfo_runtime_t *out);

/* Dumps the FreeRTOS task list to the log; diagnostics, called once. */
void sysinfo_log_tasks(void);

/* Die temperature, °C; -273 when the read fails. */
float sysinfo_chip_temp_c(void);

/* Share of the last window spent outside the idle task, percent. The window is
 * at least SYSINFO_CPU_WINDOW_US and shared by every caller, so the display and
 * the API see the same number and neither's polling rate distorts the other's.
 * 0 until the first window closes. Safe to call from any task. */
#define SYSINFO_CPU_WINDOW_US 1000000

int sysinfo_cpu_load_percent(void);

/* Why the chip last booted, as a word for the UI ("power-on", "panic", ...).
 * Never NULL. */
const char *sysinfo_reset_reason_str(void);
