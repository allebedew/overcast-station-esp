#include "ui_state.h"

#include "gfx_target.h"

void ui_state_init(ui_state_t *s)
{
    s->bright = GFX_BRIGHTNESS_DEFAULT;
    s->on     = true;
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

    /* Turns with the button down count the same: the gesture has no job yet. */
    int delta = in->cw + in->cw_held - in->ccw - in->ccw_held;
    if (delta == 0) {
        return UI_EV_NONE;
    }

    int next = s->bright + delta;
    if (next < 0)                    { next = 0; }
    if (next > GFX_BRIGHTNESS_MAX)   { next = GFX_BRIGHTNESS_MAX; }

    ui_event_t ev = next == s->bright ? UI_EV_LIMIT : UI_EV_STEP;
    s->bright = (uint8_t)next;
    gfx_set_brightness(s->bright);
    return ev;
}
