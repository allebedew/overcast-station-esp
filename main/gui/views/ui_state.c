#include "ui_state.h"

#include <stdio.h>

#include "chart.h"
#include "gfx_target.h"

void ui_state_init(ui_state_t *s)
{
    s->bright      = GFX_BRIGHTNESS_DEFAULT;
    s->on          = true;
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
     * blanks the panel. Both wrap, so no turn is ever refused. */
    int fwd  = in->cw + in->cw_held;
    int back = in->ccw + in->ccw_held;

    if (back) {
        s->focus = (ui_focus_t)((s->focus + back) % UI_FOCUS_COUNT);
    }
    if (fwd) {
        if (s->focus == UI_FOCUS_CHART_Q) {
            s->chart_q = (history_quantity_t)((s->chart_q + fwd) % HISTORY_Q_COUNT);
        } else {
            s->chart_range = (chart_range_t)((s->chart_range + fwd) % CHART_RANGE_COUNT);
        }
    }

    /* Both directions inside one 100 ms frame takes doing; the field move is
     * then the one worth hearing. */
    return back ? UI_EV_FIELD : fwd ? UI_EV_STEP : UI_EV_NONE;
}

void ui_state_format(const ui_state_t *s, char *buf, int n)
{
    snprintf(buf, (size_t)n, "chart %s %s, knob on %s",
             chart_quantity_name(s->chart_q), CHART_RANGES[s->chart_range].label,
             s->focus == UI_FOCUS_CHART_Q ? "quantity" : "range");
}
