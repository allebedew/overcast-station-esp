#pragma once

#include <stdbool.h>

/* The cell behind the INA260: charge mode, a coulomb count and a voltage-only
 * estimate. The BQ25185's STAT pins are not on the board, so the mode comes
 * from the current alone. esp-free, so the GUI simulator can build a model. */

#define BATTERY_CAPACITY_MAH 2000.0f

typedef enum {
    BATTERY_UNKNOWN,     /* no INA260 reading */
    BATTERY_DISCHARGING,
    BATTERY_CHARGING,
    BATTERY_FULL,        /* charge terminated, the charger's BATFET is off */
    BATTERY_IDLE,        /* no current, below full: input on, charger not charging */
} battery_state_t;

typedef struct {
    bool ok;                /* voltage, current, power, state and pct_v are live */
    battery_state_t state;
    float voltage_v;
    float current_ma;       /* + into the cell */
    float power_mw;
    double mah;             /* kept in NVS, 0 if it never was; the capacity while
                               full; may go negative */
    double pct;             /* mah over the capacity, unclamped */
    int    pct_v;           /* from the voltage alone, 0..100 */
    int    eta_s;           /* to full on charge, to empty on discharge, at the
                               count's rate over the last 30 s; else -1 */
} battery_t;

/* Restores the count from NVS. Before the sensors and the display start. */
void battery_init(void);

/* One INA260 result; called by the sensor task for each one exactly once. */
void battery_feed(float voltage_v, float current_ma);

void battery_get(battery_t *out);

/* Sets the count, the manual reset, and saves it. */
void battery_set_mah(float mah);

/* Writes the count to NVS if it has moved since the last write. For whoever is
 * about to do something that may not come back through the shutdown handler —
 * an OTA, say. */
void battery_save(void);

const char *battery_state_str(battery_state_t s);
