#pragma once

#include <stdbool.h>
#include <stdint.h>

/* What the room is doing, as opposed to what the chips report: `sensors.h`
 * answers "what did the BMP581 say", this answers "what is the pressure".
 *
 * Each quantity has exactly one source and no fallback. The SCD40 also measures
 * temperature, but it reads high and sits elsewhere on the board, so standing
 * in for a missing TMP117 would put a degree-sized step into the history that
 * is indistinguishable from a real event.
 *
 * Composition only — no task, no lock; the one piece of state is the site
 * altitude, cached from NVS because the sea-level pressure is recomputed on
 * every read, ten times a second while the display draws.
 *
 * One resolution per quantity, used everywhere a value is shown or stored (the
 * page, the API, the charts, the history rings, the panel):
 *
 *     temperature  0.01 °C    TMP117      "%.2f"
 *     pressure     0.001 hPa  BMP581      "%.3f"
 *     CO2          1 ppm      SCD40       "%u"
 *     humidity     0.1 %      SCD40       "%.1f"
 *     dew point    0.1 °C     SCD40       "%.1f"
 *     illuminance  0.1 lx     VEML7700    "%.1f"
 *
 * These are resolutions, not accuracies; they exist so a reading reads the same
 * everywhere and a five-minute window is not quantised flat. A display narrow
 * enough to have to drop digits says so where it does. */

typedef struct {
    bool temp_ok;
    float temp_c; /* TMP117 */

    bool rh_ok;
    float rh_pct; /* SCD40 */
    float dew_c;  /* SCD40, derived there; valid whenever rh_ok is */

    bool co2_ok;
    uint16_t co2_ppm; /* SCD40 */

    bool press_ok;
    float press_hpa;     /* BMP581, as measured at the site */
    float press_msl_hpa; /* the same reading reduced to sea level; valid
                          * whenever press_ok is */

    bool lux_ok;
    float lux; /* VEML7700, ALS channel */

    /* How far the air is from condensing: the TMP117 temperature less the
     * SCD40's dew point. Cross-device, so it is composed here rather than in
     * either driver; a small spread is condensation on the cold surfaces of
     * the room, which are colder still than the air. */
    bool dew_spread_ok;
    float dew_spread_c;
} climate_t;

/* Loads the stored site altitude. Call once, after NVS is up. */
void climate_init(void);

/* Fills *out from the latest sensor snapshots; an absent sensor's quantity
 * comes back `_ok` false and zeroed. Safe to call before sensors_init(). */
void climate_get(climate_t *out);

/* Metres above sea level, the input to the sea-level pressure reduction;
 * 0 makes the reduced value equal the measured one. It is
 * SETTING_ALTITUDE_M — set it there and this module picks it up; the range
 * below is what the settings table clamps it to. */
#define CLIMATE_ALTITUDE_MIN (-500)
#define CLIMATE_ALTITUDE_MAX 9000

/* The transform climate_get() applies to press_msl_hpa, exposed for the stored
 * history, which keeps the pressure as measured and reduces it on the way out.
 * At altitude 0 it returns its argument. */
float climate_to_sea_level(float press_hpa);

/* ISA pressure at the site altitude — the SCD40's ambient-pressure fallback
 * when no BMP581 reading is available. */
float climate_standard_pressure_hpa(void);
