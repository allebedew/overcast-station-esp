#include "gui_loop.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "buzzer.h"
#include "encoder.h"
#include "gfx_canvas.h"
#include "gfx_target.h"
#include "ld2450.h"
#include "ota.h"
#include "screen_ota.h"
#include "settings.h"
#include "ssd1322.h"
#include "ui.h"
#include "ui_state.h"
#include "weather_store.h"

static const char *TAG = "gui";

/* The FreeRTOS tick is 10 ms, so every period quantises to it. 10 frames/s is
 * as fast as anything on this screen needs to move. */
#define FRAME_MS 100

/* How often the frame numbers are logged, and only while the screen is actually
 * changing -- an idle panel says nothing. Long, because they only ever say the
 * same thing: the render budget is a tenth of the frame, and what moves the
 * numbers is what is on screen, not anything worth watching. */
#define STATS_MS (10 * 60 * 1000)

/* How long the frame stays at one burn-in offset. The share of its life a pixel
 * spends under a given glyph is one over the number of offsets and does not
 * depend on this at all; what it is chosen for is that a step of one pixel this
 * far apart is not something the eye catches. */
#define SHIFT_MS (10 * 60 * 1000)

/* The selection line is on its own, much shorter timer: it is feedback on a
 * turn, useless once the stats window is out. Not on the detent itself -- a
 * sweep of the range would be one line per click. */
#define SETTING_MS 2000

static gfx_canvas_t s_canvas;

/* What the panel is showing. A frame that comes out identical is not flushed:
 * present costs a fixed 8.3 ms of SPI whatever changed, and comparing 8 KB
 * costs tens of microseconds. Comparing pixels rather than the model is what
 * makes this work at all -- a temperature moving in its third decimal changes
 * the model many times per reading it changes on screen. */
static uint8_t s_shown[GFX_W][GFX_H / 2];

/* Whether the panel is driving its pixels, as opposed to ui_state_t's `on`,
 * which is what the knob asked for. The init script leaves it on. */
static bool s_panel_on = true;

static ui_state_t s_state;
static ui_model_t s_model;

/* What the UI settings held when the gui task last looked. A value that has
 * since moved was written from outside and has to be adopted; comparing against
 * the settings rather than against s_state is what keeps a knob change from
 * being reverted over the seconds before it is persisted. */
static ui_settings_t s_persisted;

static void load_ui_settings(ui_settings_t *u)
{
    u->bright      = (uint8_t)settings_get(SETTING_DISPLAY_BRIGHT);
    u->on          = settings_get(SETTING_DISPLAY_ON) != 0;
    u->chart_q     = (history_quantity_t)settings_get(SETTING_CHART_Q);
    u->chart_range = (chart_range_t)settings_get(SETTING_CHART_RANGE);
}

/* One NVS write per detent is what the delay behind this exists to avoid; the
 * gui task calls it once a turn has settled. settings_set() is a no-op on an
 * unchanged value, so the fields the turn did not touch cost nothing. */
static void store_ui_settings(const ui_settings_t *u)
{
    settings_set(SETTING_DISPLAY_BRIGHT, u->bright);
    settings_set(SETTING_DISPLAY_ON, u->on);
    settings_set(SETTING_CHART_Q, u->chart_q);
    settings_set(SETTING_CHART_RANGE, u->chart_range);
    s_persisted = *u;
}

/* How long the location field has to settle before the pick is applied.
 * Selecting a location drops the cached reading and refetches, so a sweep down
 * the list must not queue a fetch per detent. */
#define LOC_APPLY_MS 2000

/* The pending pick's deadline, 0 when the knob and the store agree, plus what
 * each side held when this last ran: a pick made over the web is adopted, one
 * made on the knob is held until it stops moving. */
static int64_t s_loc_apply_at;
static uint8_t s_loc_last_sel;
static int     s_loc_seen_active = -1;

/* The knob's location field against the store, which the web API also edits. */
static void sync_location(int64_t now)
{
    int count  = weather_store_count();
    int active = weather_store_get_active();

    s_state.loc_count = (uint8_t)count;

    /* A pick made elsewhere, or a list edited under the index, wins over
     * whatever the knob was holding. */
    if (active != s_loc_seen_active || s_state.loc_sel >= count) {
        s_loc_seen_active = active;
        s_state.loc_sel   = (uint8_t)(active < 0 ? 0 : active);
        s_loc_last_sel    = s_state.loc_sel;
        s_loc_apply_at    = 0;
        return;
    }

    if (s_state.loc_sel != s_loc_last_sel) {
        s_loc_last_sel = s_state.loc_sel;
        s_loc_apply_at = now + LOC_APPLY_MS * 1000;
    }
    if (s_loc_apply_at && now >= s_loc_apply_at) {
        s_loc_apply_at = 0;
        if (active >= 0 && s_state.loc_sel != (uint8_t)active) {
            weather_store_set_active(s_state.loc_sel);
            s_loc_seen_active = s_state.loc_sel;
        }
    }
}

/* A setting written from elsewhere (the web API talks to the settings module,
 * not to this one), applied where the panel has its single owner. */
static void adopt_display_settings(void)
{
    ui_settings_t now;
    load_ui_settings(&now);

    if (now.bright != s_persisted.bright) {
        s_state.set.bright = now.bright;
        gfx_set_brightness(now.bright);
    }
    if (now.on != s_persisted.on) {
        s_state.set.on = now.on;
    }
    s_persisted = now;
}

/* The radar's own flag, the same one the arrive/leave sounds follow, so the
 * panel goes dark on the beep rather than on a hold of its own. A silent module
 * counts as present: a dead radar must not blank the panel for good. */
static bool presence_now(void)
{
    ld2450_data_t r;
    return !ld2450_get(&r) || r.presence;
}

/* Whatever the knob is currently on, on one line: the panel marks the selection
 * nowhere yet. Written at boot and after a change. */
static void log_setting(void)
{
    char line[128];

    ui_state_format(&s_state, line, sizeof(line));
    ESP_LOGI(TAG, "panel: %s", line);
}

/* Returns whether the panel was actually written. */
static bool present_if_changed(void)
{
    if (memcmp(s_canvas.buf, s_shown, sizeof(s_shown)) == 0) {
        return false;
    }
    gfx_present(&s_canvas);
    memcpy(s_shown, s_canvas.buf, sizeof(s_shown));
    return true;
}

/* A flash in progress takes the panel over: no model, no input, and a display
 * the knob switched off is lit for the duration. The normal path turns it back
 * off on the first frame after the upload ends. */
static void ota_frame(void)
{
    size_t received, total;
    ota_get_progress(&received, &total);

    gfx_set_shift(&s_canvas, 0);
    screen_ota(&s_canvas, received, total);
    present_if_changed();

    if (!s_panel_on) {
        gfx_set_on(true);
        s_panel_on = true;
    }
}

static const buzzer_tune_t EV_TUNE[] = {
    [UI_EV_STEP]  = BUZZER_CLICK,
    [UI_EV_FIELD] = BUZZER_CLICK_HI,
    [UI_EV_LIMIT] = BUZZER_CLICK_LO,
};

static void gui_task(void *arg)
{
    (void)arg;

    TickType_t last = xTaskGetTickCount();

    uint32_t frames = 0, flushes = 0;
    int64_t  render_sum_us = 0, render_max_us = 0;
    int64_t  stats_at   = esp_timer_get_time() + STATS_MS * 1000;
    int64_t  setting_at = esp_timer_get_time() + SETTING_MS * 1000;
    int64_t  shift_at   = esp_timer_get_time() + SHIFT_MS * 1000;

    /* The offset walks the range a pixel at a time and turns round at each end:
     * closing the cycle 5 -> 0 instead would be one visible jump per cycle. */
    int shift = 0, shift_dir = 1;

    bool changed = false;

    for (;;) {
        if (ota_is_active()) {
            ota_frame();
        } else {
            int64_t now = esp_timer_get_time();

            adopt_display_settings();

            encoder_input_t in;
            encoder_take(&in);

            ui_event_t ev = ui_state_input(&s_state, &in);
            if (ev != UI_EV_NONE) {
                buzzer_play(EV_TUNE[ev]);
            }
            changed |= ev != UI_EV_NONE;

            /* A click is a single event rather than a sweep, so it is stored as
             * it happens — which also keeps what the API reports exact. */
            if (s_state.set.on != s_persisted.on) {
                settings_set(SETTING_DISPLAY_ON, s_state.set.on);
                s_persisted.on = s_state.set.on;
            }
            sync_location(now);

            /* Lit only for someone who is there to read it. */
            bool want_on = s_state.set.on && presence_now();

            /* A dark panel is drawn for and clocked to not at all: display-off
             * leaves its RAM alone, so s_shown keeps describing it. */
            if (want_on) {
                /* Only a lit panel ages, so a dark one holds the offset where
                 * it stands rather than walking it invisibly. */
                if (now >= shift_at) {
                    if (shift + shift_dir < 0 || shift + shift_dir > GFX_SHIFT_MAX) {
                        shift_dir = -shift_dir;
                    }
                    shift += shift_dir;
                    shift_at = now + SHIFT_MS * 1000;
                }
                gfx_set_shift(&s_canvas, shift);

                /* The frame is drawn from the selection the same call just
                 * moved, so the series and the badge under it can never be a
                 * frame apart. */
                ui_model_refresh(&s_model, s_state.set.chart_q,
                                 s_state.set.chart_range, s_state.loc_sel);

                int64_t t0 = esp_timer_get_time();
                ui_render(&s_canvas, &s_model, &s_state);
                int64_t render_us = esp_timer_get_time() - t0;

                frames++;
                render_sum_us += render_us;
                if (render_us > render_max_us) {
                    render_max_us = render_us;
                }

                if (present_if_changed()) {
                    flushes++;
                }

                /* Powered up only once a fresh frame is in the panel's RAM —
                 * the other order flashes the one it was switched off on. */
                if (!s_panel_on) {
                    gfx_set_on(true);
                    s_panel_on = true;
                }
            } else if (s_panel_on) {
                gfx_set_on(false);
                s_panel_on = false;
            }

            if (now >= setting_at) {
                if (changed) {
                    changed = false;
                    store_ui_settings(&s_state.set);
                    log_setting();
                }
                setting_at = now + SETTING_MS * 1000;
            }
            if (now >= stats_at) {
                if (flushes) {
                    ESP_LOGI(TAG, "%lu frames, %lu flushed, render avg %lld us, max %lld us",
                             (unsigned long)frames, (unsigned long)flushes,
                             render_sum_us / frames, render_max_us);
                }
                frames = flushes = 0;
                render_sum_us = render_max_us = 0;
                stats_at = now + STATS_MS * 1000;
            }
        }

        /* A render that overran the period leaves the task runnable, and it
         * outranks idle on this single core -- without the yield the watchdog
         * trips on IDLE within 5 s. */
        if (!xTaskDelayUntil(&last, pdMS_TO_TICKS(FRAME_MS))) {
            taskYIELD();
        }
    }
}

void gui_loop_init(void)
{
    ssd1322_init();
    gfx_init(&s_canvas);

    /* Blank the panel, to put it in the state s_shown claims it is in: its RAM
     * comes up undefined and the first rendered frame may well match s_shown. */
    gfx_present(&s_canvas);

    load_ui_settings(&s_persisted);
    ui_state_init(&s_state, &s_persisted);
    log_setting();

    xTaskCreate(gui_task, "gui", 4096, NULL, 2, NULL);
}
