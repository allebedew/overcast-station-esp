#pragma once

#include <stdint.h>

/* How the station itself is doing, as opposed to the air around it: the die
 * temperature, the CPU load, the memory, and what this firmware and this chip
 * are. These are health readings — they belong together, not with the sensors
 * on the I2C bus.
 *
 * Two shapes, by how often the answer changes: what was decided at build or
 * boot time is read once and handed out as a snapshot, and what moves while
 * the station runs is sampled per call. */

void sysinfo_init(void);

/* Fixed for the life of this boot: what was built, what it is running on, and
 * how full NVS was at startup. Never NULL; the strings live in the firmware
 * image and outlive any caller. */
typedef struct {
    const char *app_version;
    const char *build_date; /* __DATE__ of the build */
    const char *build_time; /* __TIME__ of the build */
    const char *idf_ver;
    int chip_rev_major;
    int chip_rev_minor;
    int cpu_mhz;
    uint32_t flash_mb;

    /* Counted once in sysinfo_init(). nvs_get_stats() walks the partition's
     * page headers, which is more work than a status poll every second should
     * be doing, and the figure only moves when a setting is written — so this
     * is NVS as it was at boot, not as it is now. */
    uint32_t nvs_used_entries;
    uint32_t nvs_total_entries;
} sysinfo_static_t;

const sysinfo_static_t *sysinfo_static(void);

/* What moves while the station runs. Cheap enough to sample on every status
 * poll — one IDF call per field. */
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

/* Dumps the FreeRTOS task list to the log. Diagnostics, called once after
 * start-up. */
void sysinfo_log_tasks(void);

/* Die temperature, °C; -273 when the read fails. */
float sysinfo_chip_temp_c(void);

/* Share of the last measurement window spent outside the idle task, percent.
 * The window is at least SYSINFO_CPU_WINDOW_US long and is shared by every
 * caller: a read inside the current window returns the figure already
 * computed, so the display and the HTTP API always show the same number and
 * neither one's polling rate distorts the other's. Returns 0 until the first
 * window has closed. Safe to call from any task. */
#define SYSINFO_CPU_WINDOW_US 1000000

int sysinfo_cpu_load_percent(void);

/* Why the chip last booted, as a word for the UI ("power-on", "panic", ...).
 * Never NULL. */
const char *sysinfo_reset_reason_str(void);
