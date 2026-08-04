#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Rotary encoder with a push button: an EC11 on GPIO23 (A) / GPIO22 (B), the
 * button on GPIO21, all three pulled up inside the chip. Nothing here knows
 * what a turn means — the events are raw, and the interaction scheme that maps
 * them to selection, editing or a menu lives with the screens.
 *
 * Deliberately free of esp headers so the host-side GUI simulator can include
 * it and feed synthetic input to the same screen code. */

typedef struct {
    /* Detents since the last take, counted per direction rather than netted:
     * a scheme may give the two directions different jobs. Saturating. */
    int8_t cw, ccw;

    /* The same, for detents turned while the button was down. Cannot be
     * recovered by the caller: which detent belongs to the press is settled
     * when it arrives, not when the frame gets around to asking. */
    int8_t cw_held, ccw_held;

    /* Events since the last take. Counters, not flags — a frame that runs
     * late must not swallow the second press. */
    uint8_t click, double_click, long_press;

    bool held; /* live state, not an event */
} encoder_input_t;

void encoder_init(void);

/* Takes everything accumulated and clears it, so each detent and each press is
 * acted on exactly once. Safe from any task. */
void encoder_take(encoder_input_t *out);
