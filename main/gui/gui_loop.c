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
#include "ssd1322.h"
#include "ui.h"
#include "ui_state.h"

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

static ui_state_t s_state;
static ui_model_t s_model;

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
        encoder_input_t in;
        encoder_take(&in);

        ui_event_t ev = ui_state_input(&s_state, &in);
        if (ev != UI_EV_NONE) {
            buzzer_play(EV_TUNE[ev]);
        }
        changed |= ev != UI_EV_NONE;

        /* The frame is drawn from the selection the same call just moved, so
         * the series and the badge under it can never be a frame apart. */
        ui_model_refresh(&s_model, s_state.chart_q, s_state.chart_range);

        int64_t t0 = esp_timer_get_time();
        if (s_state.on) {
            ui_render(&s_canvas, &s_model, &s_state);
        } else {
            /* Blanked rather than switched off: every pixel dark is what an
             * OLED does with a display-off command anyway, and it keeps the
             * transport out of it. */
            gfx_clear(&s_canvas, GFX_OFF);
        }
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

        int64_t now = esp_timer_get_time();
        if (now >= setting_at) {
            if (changed) {
                changed = false;
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

    ui_state_init(&s_state);
    log_setting();

    xTaskCreate(gui_task, "gui", 4096, NULL, 2, NULL);
}
