#include "ui_state.h"

#include <stdio.h>

#include "chart.h"
#include "gfx_target.h"

void ui_state_init(ui_state_t *s, const ui_settings_t *set)
{
    s->set       = *set;
    s->focus     = UI_FOCUS_NONE;
    s->loc_sel   = 0;
    s->loc_count = 0;
    gfx_set_brightness(s->set.bright);
}

ui_event_t ui_state_input(ui_state_t *s, const encoder_input_t *in)
{
    /* An odd number of clicks lands on the other state; an even one is where
     * it started, however many arrived in the one frame. */
    if (in->click & 1) {
        s->set.on = !s->set.on;
        return UI_EV_FIELD;
    }

    /* Held or not, a detent means the same thing: the gesture has no job yet.
     * Back walks the fields, forward cycles the value of the one in focus —
     * split by direction rather than by a click because the click already
     * switches the panel off. Both wrap; a turn forward moves nothing only on
     * the empty focus and on a location field with nothing to switch to. */
    int fwd  = in->cw + in->cw_held;
    int back = in->ccw + in->ccw_held;

    if (back) {
        s->focus = (ui_focus_t)((s->focus + back) % UI_FOCUS_COUNT);
    }
    if (fwd) {
        switch (s->focus) {
        case UI_FOCUS_CHART_Q:
            s->set.chart_q =
                (history_quantity_t)((s->set.chart_q + fwd) % HISTORY_Q_COUNT);
            break;
        case UI_FOCUS_CHART_RANGE:
            s->set.chart_range =
                (chart_range_t)((s->set.chart_range + fwd) % CHART_RANGE_COUNT);
            break;
        case UI_FOCUS_NONE:
            return UI_EV_LIMIT;
        case UI_FOCUS_LOC:
            /* One location, or none, is the single field a turn can be refused
             * on: there is nothing else to move to. */
            if (s->loc_count < 2) {
                return UI_EV_LIMIT;
            }
            s->loc_sel = (uint8_t)((s->loc_sel + fwd) % s->loc_count);
            break;
        default:
            /* 0 is the dimmest step the panel has, not off, so wrapping through
             * it costs nothing. */
            s->set.bright =
                (uint8_t)((s->set.bright + fwd) % (GFX_BRIGHTNESS_MAX + 1));
            gfx_set_brightness(s->set.bright);
            break;
        }
    }

    /* Both directions inside one 100 ms frame takes doing; the field move is
     * then the one worth hearing. */
    return back ? UI_EV_FIELD : fwd ? UI_EV_STEP : UI_EV_NONE;
}

void ui_state_format(const ui_state_t *s, char *buf, int n)
{
    static const char *const FIELD[UI_FOCUS_COUNT] = { "nothing", "location", "quantity",
                                                       "range", "brightness" };

    snprintf(buf, (size_t)n, "chart %s %s, bright %u, location %u/%u, knob on %s",
             chart_quantity_name(s->set.chart_q),
             CHART_RANGES[s->set.chart_range].label, s->set.bright, s->loc_sel,
             s->loc_count, FIELD[s->focus]);
}
