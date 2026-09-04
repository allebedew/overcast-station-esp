#pragma once

#include <stdbool.h>

/* A passive infrared motion sensor on one GPIO: its output sits high while it
 * sees a warm body moving and falls back after a hold time set on the module
 * itself, which is seconds at the least. That hold is why nothing here needs
 * one of its own — unlike the radar, the sensor does not lose a person who
 * pauses, it just keeps the line high until the room has been still. */

/* Brings up the pin and starts the watcher task. */
void pir_init(void);

/* Current state. False before pir_init(), which reads as an empty room. */
bool pir_present(void);

/* The pin as it reads right now, without the hold above it: diagnostics only,
 * to tell a dead sensor from a quiet room. */
bool pir_raw(void);
