#include "ui_state.h"

#include <stdio.h>

#include "chart.h"
#include "gfx_target.h"

void ui_state_init(ui_state_t *s, uint8_t bright, bool on)
{
    s->bright      = bright;
    s->on          = on;
    s->focus       = UI_FOCUS_CHART_Q;
    s->chart_q     = HISTORY_Q_TEMP;
    s->chart_range = CHART_RANGE_1M;
    gfx_set_brightness(s->bright);
}

ui_event_t ui_state_input(ui_state_t *s, const encoder_input_t *in)
{
    /* An odd number of clicks lands on the other state; an even one is where
     * it started, however many arrived in the one frame. */
    if (in->click & 1) {
        s->on = !s->on;
        return UI_EV_FIELD;
    }

    /* Held or not, a detent means the same thing: the gesture has no job yet.
     * Back walks the fields, forward cycles the value of the one in focus —
     * split by direction rather than by a click because the click already
     * switches the panel off. Both wrap, so no turn is ever refused. */
    int fwd  = in->cw + in->cw_held;
    int back = in->ccw + in->ccw_held;

    if (back) {
        s->focus = (ui_focus_t)((s->focus + back) % UI_FOCUS_COUNT);
    }
    if (fwd) {
        switch (s->focus) {
        case UI_FOCUS_CHART_Q:
            s->chart_q = (history_quantity_t)((s->chart_q + fwd) % HISTORY_Q_COUNT);
            break;
        case UI_FOCUS_CHART_RANGE:
            s->chart_range = (chart_range_t)((s->chart_range + fwd) % CHART_RANGE_COUNT);
            break;
        default:
            /* 0 is the dimmest step the panel has, not off, so wrapping through
             * it costs nothing. */
            s->bright = (uint8_t)((s->bright + fwd) % (GFX_BRIGHTNESS_MAX + 1));
            gfx_set_brightness(s->bright);
            break;
        }
    }

    /* Both directions inside one 100 ms frame takes doing; the field move is
     * then the one worth hearing. */
    return back ? UI_EV_FIELD : fwd ? UI_EV_STEP : UI_EV_NONE;
}

void ui_state_format(const ui_state_t *s, char *buf, int n)
{
    static const char *const FIELD[UI_FOCUS_COUNT] = { "quantity", "range", "brightness" };

    snprintf(buf, (size_t)n, "chart %s %s, bright %u, knob on %s",
             chart_quantity_name(s->chart_q), CHART_RANGES[s->chart_range].label,
             s->bright, FIELD[s->focus]);
}
