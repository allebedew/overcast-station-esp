#include "panel_hours.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "gfx_target.h"
#include "nvs.h"

#define NVS_NAMESPACE "panel"
#define NVS_KEY       "hours"

static const char *TAG = "panel_hours";

/* How much lit time may be lost to a power cut. The panel is rated in tens of
 * thousands of hours, so a quarter of an hour is below the resolution of the
 * question; what the period is really chosen against is NVS wear, and at one
 * 32-byte entry per write it has four orders of magnitude of headroom. */
#define FLUSH_S 900

/* A shorter floor for the write that follows the panel going dark: without it
 * someone stepping in and out of the radar's view would write per crossing. */
#define MIN_FLUSH_S 60

/* A frame is 100 ms; anything longer than this is the gui task having been away
 * (an OTA takes the panel over and stops calling), and is not lit time. */
#define MAX_STEP_US 1000000

/* Whole seconds are what goes out and what is stored; the remainders keep the
 * per-frame slices from being lost to truncation. Written by the gui task,
 * read by the web server and the shutdown handler -- aligned 32-bit words, so
 * a reader gets one or the other value and never a mix. */
static uint32_t s_on_s, s_dose_s;
static uint32_t s_on_ms, s_dose_ms;

static uint32_t s_saved_on_s;  /* what NVS holds, to size the unsaved debt */
static int64_t  s_last_us;
static bool     s_was_lit;

static void save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        /* One key, so the two counters cannot come back from a cut apart. */
        err = nvs_set_u64(h, NVS_KEY, (uint64_t)s_on_s << 32 | s_dose_s);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "not saved: %s", esp_err_to_name(err));
        return;
    }
    s_saved_on_s = s_on_s;
}

void panel_hours_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint64_t packed = 0;
        if (nvs_get_u64(h, NVS_KEY, &packed) == ESP_OK) {
            s_on_s   = (uint32_t)(packed >> 32);
            s_dose_s = (uint32_t)packed;
        }
        nvs_close(h);
    }
    s_saved_on_s = s_on_s;

    /* Catches a reboot from the web or an OTA; a brownout still loses the
     * unsaved debt, which is what FLUSH_S bounds. */
    esp_register_shutdown_handler(save);

    ESP_LOGI(TAG, "%lu h lit, %lu h at full brightness",
             (unsigned long)(s_on_s / 3600), (unsigned long)(s_dose_s / 3600));
}

void panel_hours_track(bool lit, uint8_t bright)
{
    int64_t now = esp_timer_get_time();
    int64_t dt  = now - s_last_us;
    s_last_us = now;

    if (lit && s_was_lit && dt > 0 && dt <= MAX_STEP_US) {
        uint32_t ms = (uint32_t)(dt / 1000);
        s_on_ms += ms;
        /* Degradation goes with the current through the pixel, and the master
         * current is linear in the brightness step: level 15 is full drive,
         * level 0 is a sixteenth of it. */
        s_dose_ms += ms * (bright + 1) / (GFX_BRIGHTNESS_MAX + 1);

        s_on_s   += s_on_ms / 1000;
        s_dose_s += s_dose_ms / 1000;
        s_on_ms   %= 1000;
        s_dose_ms %= 1000;
    }

    uint32_t debt = s_on_s - s_saved_on_s;
    if (debt >= FLUSH_S || (s_was_lit && !lit && debt >= MIN_FLUSH_S)) {
        save();
    }
    s_was_lit = lit;
}

void panel_hours_get(uint32_t *on_s, uint32_t *dose_s)
{
    *on_s   = s_on_s;
    *dose_s = s_dose_s;
}
