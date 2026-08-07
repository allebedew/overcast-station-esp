#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "chart.h"     /* the selection is stated in the vocabulary of what it
                        * picks: a history quantity and a chart window */
#include "encoder.h"
#include "history.h"

/* What the UI remembers about itself, as opposed to ui_model_t, which is the
 * world. Separate because refreshing the model overwrites it whole, and a
 * selection would not survive that.
 *
 * The knob's selection lives here rather than under the screen that draws what
 * it picks: the model has to be told which series to sample before the frame
 * starts, so the choice is read a layer above the drawing anyway. When a second
 * page arrives, this is the state machine that grows a page field — and
 * ui_state_input() the one that grows the gesture for it.
 *
 * Free of esp headers, like encoder.h, so the host simulator can drive the same
 * state machine with synthetic input. */

/* What the knob is on. Turning back walks this list, turning forward cycles the
 * value of the entry it stands on. */
typedef enum {
    UI_FOCUS_CHART_Q,
    UI_FOCUS_CHART_RANGE,
    UI_FOCUS_BRIGHT,
    UI_FOCUS_COUNT,
} ui_focus_t;

typedef struct {
    /* Device settings rather than a screen's: they survive whatever is on the
     * panel. Both are one of the knob's fields and both are persisted, by
     * whoever calls this — the state machine itself stays free of the settings
     * module, which is an esp header away. A station whose panel was switched
     * off therefore boots dark. */
    uint8_t bright;
    bool on;

    /* What the chart is showing, and which of the two a turn back would
     * change. */
    ui_focus_t         focus;
    history_quantity_t chart_q;
    chart_range_t      chart_range;
} ui_state_t;

/* What an input handler did, for whoever owns the sound. Kept out of the
 * handlers themselves so they stay linkable on the host. */
typedef enum {
    UI_EV_NONE = 0,
    UI_EV_STEP,    /* a value moved */
    UI_EV_FIELD,   /* the selection moved */
    UI_EV_LIMIT,   /* a turn that changed nothing: the range ends here */
} ui_event_t;

/* Takes the persisted panel settings and applies them, so the panel and this
 * struct agree whatever the transport's own init sequence left behind. */
void ui_state_init(ui_state_t *s, uint8_t bright, bool on);

ui_event_t ui_state_input(ui_state_t *s, const encoder_input_t *in);

/* The selection on one line, for the log: nothing on the panel marks it yet. */
void ui_state_format(const ui_state_t *s, char *buf, int n);
