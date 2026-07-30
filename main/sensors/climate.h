#pragma once

#include <stdbool.h>
#include <stdint.h>

/* What the room is doing, as opposed to what the chips report.
 *
 * `sensors.h` answers "what did the BMP581 say"; this answers "what is the
 * pressure". Each quantity has exactly one source and no fallback — in
 * particular the temperature is the TMP117's and nothing else. The SCD40 also
 * measures temperature, but it reads high (it heats itself) and it is a
 * different spot on the board, so quietly standing in for a missing TMP117
 * would put a step of about a degree into the history and the charts, and that
 * step is indistinguishable from a real event. A missing sensor is reported as
 * missing instead.
 *
 * Composition of the published snapshots — no task and no lock; the only
 * state it holds is the site altitude, cached from NVS because the sea-level
 * pressure below is recomputed on every read, ten times a second while the
 * display is drawing.
 *
 * ---- Reading resolution ----
 *
 * One resolution per quantity, used everywhere a value is shown or stored —
 * the web page, the HTTP API, the charts, the history rings and the LCD:
 *
 *     temperature  0.01 °C    TMP117      "%.2f"
 *     pressure     0.001 hPa  BMP581      "%.3f"
 *     CO2          1 ppm      SCD40       "%u"
 *     humidity     0.1 %      SCD40       "%.1f"
 *     illuminance  0.1 lx     VEML7700    "%.1f"
 *
 * These are resolutions, not accuracies — the sensors are worth ±0.1 °C,
 * ±0.3 hPa absolute (±0.06 relative), ±(50 ppm + 5 %) and ±6 %RH. The point is
 * that the same reading reads the same everywhere, and that a five-minute
 * chart window is not quantised into one flat step.
 *
 * The 16x2 display is the one place that cannot always keep this: sixteen
 * characters do not fit every row at full resolution, and where it has to give
 * way screen_16x2.c says so at the layout it belongs to. Nothing else deviates.
 */

typedef struct {
    bool temp_ok;
    float temp_c; /* TMP117 */

    bool rh_ok;
    float rh_pct; /* SCD40 */

    bool co2_ok;
    uint16_t co2_ppm; /* SCD40 */

    bool press_ok;
    float press_hpa;     /* BMP581, as measured at the site */
    float press_msl_hpa; /* the same reading reduced to sea level; valid
                          * whenever press_ok is */

    bool lux_ok;
    float lux; /* VEML7700, ALS channel */
} climate_t;

/* Loads the stored site altitude. Call once, after NVS is up. */
void climate_init(void);

/* Fills *out from the latest sensor snapshots. Quantities whose sensor is
 * absent or has not produced a reading yet come back with their `_ok` false
 * and the value zeroed. Safe to call before sensors_init(). */
void climate_get(climate_t *out);

/* Height of the station above sea level, metres — what the reduction of the
 * measured pressure to sea level is computed from. Defaults to 0, which makes
 * the reduced value equal the measured one. The setter clamps to
 * CLIMATE_ALTITUDE_MIN..MAX and persists to NVS. */
#define CLIMATE_ALTITUDE_MIN (-500)
#define CLIMATE_ALTITUDE_MAX 9000

int climate_altitude_m(void);
void climate_set_altitude_m(int metres);

/* Reduces a pressure measured at the site to sea level, from the configured
 * altitude — the same transform climate_get() applies to press_msl_hpa.
 * Exposed for the stored history, which keeps the pressure as measured and is
 * reduced only on its way out. At altitude 0 it returns its argument. */
float climate_to_sea_level(float press_hpa);

/* Standard atmospheric pressure at the site altitude (ISA model) — what a
 * barometer reads there on a standard day. Used as the SCD40's ambient
 * pressure fallback when no BMP581 reading is available. */
float climate_standard_pressure_hpa(void);
