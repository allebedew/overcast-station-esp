#pragma once

#include <stdbool.h>

/* A passive infrared motion sensor on one GPIO: its output sits high while it
 * sees a warm body moving and falls back after a hold time set on the module
 * itself, which is seconds at the least. The module's own hold is far too
 * short for somebody sitting still, so pir.c holds the state further; see
 * HOLD_MS there. */

/* Brings up the pin and starts the watcher task. */
void pir_init(void);

/* Current state. True from boot until HOLD_MS passes without movement. */
bool pir_present(void);

/* Whether the line has gone high since boot — tells the assumed presence at
 * startup from one the sensor actually saw. */
bool pir_moved(void);

/* The pin as it reads right now, without the hold above it: diagnostics only,
 * to tell a dead sensor from a quiet room. */
bool pir_raw(void);
