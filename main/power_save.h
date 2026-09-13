#pragma once

#include <stdbool.h>

/* Experimental: a hand-thrown mode that drops the two largest consumers — the
 * Wi-Fi radio and the SCD40 — and leaves the panel alone. Toggled by a long
 * press on the encoder; not persisted, so a restart comes back with it off.
 *
 * The station browns out into a reboot loop near 3.55 V on the cell, and what
 * is suspected there is the current peaks (Wi-Fi TX, the SCD40's 205 mA
 * measurement pulse every 5 s) on a cell whose internal resistance has risen,
 * not the average draw. This is the manual measure while that is being
 * chased — and the A/B the INA260 can measure the saving with. */

bool power_save_on(void);

void power_save_set(bool on);

void power_save_toggle(void);
