#include "ui_state.h"

#include <math.h>
#include <stdio.h>

#include "chart.h"
#include "gfx_target.h"

/* The room's illuminance mapped to the panel's 16 steps, by decade: the eye is
 * logarithmic and a linear map would leave the panel dark all the way through a
 * lit room. Full drive from AUTO_FULL_LX up, the dimmest step at and below
 * AUTO_DARK_LX. */
#define AUTO_FULL_LX 200.0f
#define AUTO_DARK_LX 1.0f

/* How far the computed level has to sit past the current step before it is
 * taken. Without it a room resting between two steps flickers between them.
 * It is a dead band, not a delay: a real change in the light is followed on the
 * frame it arrives on. */
#define AUTO_HYST 0.6f

static float auto_level(float lux)
{
    if (lux <= AUTO_DARK_LX) {
        return 0.0f;
    }
    if (lux >= AUTO_FULL_LX) {
        return (float)GFX_BRIGHTNESS_MAX;
    }
    return (float)GFX_BRIGHTNESS_MAX * log10f(lux) / log10f(AUTO_FULL_LX);
}

/* The level the panel is driven at right now, as opposed to bright_want, which
 * is where it is heading. */
static void apply_bright(ui_state_t *s, uint8_t level)
{
    if (level != s->bright_now) {
        s->bright_now = level;
        gfx_set_brightness(level);
    }
}

/* One step of the ramp towards the target. A step a frame, so the widest sweep
 * takes a second and a half and a single detent is over in a frame: the panel
 * slides to a new light level instead of jumping, and there is no delay before
 * it starts. */
static void ramp_bright(ui_state_t *s)
{
    if (s->bright_now < s->bright_want) {
        apply_bright(s, (uint8_t)(s->bright_now + 1));
    } else if (s->bright_now > s->bright_want) {
        apply_bright(s, (uint8_t)(s->bright_now - 1));
    }
}

void ui_state_init(ui_state_t *s, const ui_settings_t *set)
{
    s->set        = *set;
    s->focus      = UI_FOCUS_NONE;
    s->loc_sel    = 0;
    s->loc_count  = 0;
    s->bright_want = s->set.bright;
    s->bright_now  = 0;

    /* At boot the panel takes the level outright; there is nothing on screen
     * yet for a ramp to be seen on. */
    apply_bright(s, s->set.bright);
}

void ui_state_light(ui_state_t *s, bool lux_ok, float lux)
{
    /* Manual mode still comes through here: it is the one call a frame that
     * reconciles the panel with the setting, wherever the setting was changed. */
    if (!s->set.auto_bright) {
        s->bright_want = s->set.bright;
    } else if (lux_ok) {
        float want = auto_level(lux);
        if (fabsf(want - (float)s->bright_want) >= AUTO_HYST) {
            s->bright_want = (uint8_t)lroundf(want);
        }
    }

    ramp_bright(s);
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
        default: {
            /* One ring: the 16 manual steps, then auto past the top. 0 is the
             * dimmest step the panel has, not off, so wrapping through it costs
             * nothing. */
            int pos = s->set.auto_bright ? GFX_BRIGHTNESS_MAX + 1 : s->set.bright;
            pos = (pos + fwd) % (GFX_BRIGHTNESS_MAX + 2);

            s->set.auto_bright = pos > GFX_BRIGHTNESS_MAX;
            if (!s->set.auto_bright) {
                s->set.bright = (uint8_t)pos;
            }
            break;
        }
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

    snprintf(buf, (size_t)n, "chart %s %s, bright %u%s, location %u/%u, knob on %s",
             chart_quantity_name(s->set.chart_q),
             CHART_RANGES[s->set.chart_range].label, s->bright_now,
             s->set.auto_bright ? " auto" : "", s->loc_sel, s->loc_count,
             FIELD[s->focus]);
}
