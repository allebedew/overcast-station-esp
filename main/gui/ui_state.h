#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "encoder.h"

/* What the UI remembers about itself, as opposed to ui_model_t, which is the
 * world. Separate because refreshing the model overwrites it whole, and a
 * selection would not survive that.
 *
 * Only what every screen shares lives here. A screen's own state — the panel
 * registers screen_panel sweeps, a chart's window — belongs to that screen's
 * file, so that adding one does not widen this struct.
 *
 * Free of esp headers, like encoder.h, so the host simulator can drive the same
 * state machine with synthetic input. */

typedef struct {
    /* A device setting rather than a screen's: it survives whatever is on the
     * panel, and it is what moves into the settings module, and into NVS, once
     * there is one. Until then the knob edits it directly. */
    uint8_t bright;

    /* Temporary: a click blanks the panel and another brings it back. Comes up
     * on and is not persisted — a station that boots dark looks broken. */
    bool on;
} ui_state_t;

/* What an input handler did, for whoever owns the sound. Kept out of the
 * handlers themselves so they stay linkable on the host. */
typedef enum {
    UI_EV_NONE = 0,
    UI_EV_STEP,    /* a value moved */
    UI_EV_FIELD,   /* the selection moved */
    UI_EV_LIMIT,   /* a turn that changed nothing: the range ends here */
} ui_event_t;

/* Applies what it starts with, so the panel and this struct agree whatever the
 * transport's own init sequence left behind. */
void ui_state_init(ui_state_t *s);

ui_event_t ui_state_input(ui_state_t *s, const encoder_input_t *in);
