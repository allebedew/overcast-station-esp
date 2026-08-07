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

/* What the two panel settings held when the gui task last looked. A value that
 * has since moved was written from outside and has to be adopted; comparing
 * against the settings rather than against s_state is what keeps a knob change
 * from being reverted over the seconds before it is persisted. */
static uint8_t s_persisted_bright;
static bool    s_persisted_on;

/* One NVS write per detent is what the delay behind this exists to avoid; the
 * gui task calls it once a turn has settled. */
static void persist_brightness(void)
{
    settings_set(SETTING_DISPLAY_BRIGHT, s_state.bright);
    s_persisted_bright = s_state.bright;
}

/* A click is a single event rather than a sweep, so it is stored as it happens
 * — which also keeps what the API reports exact. */
static void persist_on(void)
{
    settings_set(SETTING_DISPLAY_ON, s_state.on);
    s_persisted_on = s_state.on;
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
    uint8_t bright = (uint8_t)settings_get(SETTING_DISPLAY_BRIGHT);
    if (bright != s_persisted_bright) {
        s_persisted_bright = bright;
        s_state.bright = bright;
        gfx_set_brightness(bright);
    }
    bool on = settings_get(SETTING_DISPLAY_ON) != 0;
    if (on != s_persisted_on) {
        s_persisted_on = on;
        s_state.on = on;
    }
}

/* Whatever the knob is currently on, on one line: the panel marks the selection
 * nowhere yet. Written at boot and after a change. */
static void log_setting(void)
{
    char line[128];

    ui_state_format(&s_state, line, sizeof(line));
    ESP_LOGI(TAG, "panel: %s", line);
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

    bool changed = false;

    for (;;) {
        adopt_display_settings();

        encoder_input_t in;
        encoder_take(&in);

        ui_event_t ev = ui_state_input(&s_state, &in);
        if (ev != UI_EV_NONE) {
            buzzer_play(EV_TUNE[ev]);
        }
        changed |= ev != UI_EV_NONE;

        if (s_state.on != s_persisted_on) {
            persist_on();
        }
        sync_location(esp_timer_get_time());

        /* A dark panel is drawn for and clocked to not at all: display-off
         * leaves its RAM alone, so s_shown keeps describing it. */
        if (s_state.on) {
            /* The frame is drawn from the selection the same call just moved,
             * so the series and the badge under it can never be a frame
             * apart. */
            ui_model_refresh(&s_model, s_state.chart_q, s_state.chart_range,
                             s_state.loc_sel);

            int64_t t0 = esp_timer_get_time();
            ui_render(&s_canvas, &s_model, &s_state);
            int64_t render_us = esp_timer_get_time() - t0;

            frames++;
            render_sum_us += render_us;
            if (render_us > render_max_us) {
                render_max_us = render_us;
            }

            if (memcmp(s_canvas.buf, s_shown, sizeof(s_shown)) != 0) {
                gfx_present(&s_canvas);
                memcpy(s_shown, s_canvas.buf, sizeof(s_shown));
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

        int64_t now = esp_timer_get_time();
        if (now >= setting_at) {
            if (changed) {
                changed = false;
                persist_brightness();
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

    s_persisted_bright = (uint8_t)settings_get(SETTING_DISPLAY_BRIGHT);
    s_persisted_on = settings_get(SETTING_DISPLAY_ON) != 0;
    ui_state_init(&s_state, s_persisted_bright, s_persisted_on);
    log_setting();

    xTaskCreate(gui_task, "gui", 4096, NULL, 2, NULL);
}
