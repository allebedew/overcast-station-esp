#pragma once

#include <stdbool.h>

#include "climate.h"
#include "weather_api.h"
#include "zambretti.h"

/* The comfort bands, in one table: the same numbers decide what blinks on the
 * panel and what goes out over Telegram. Numbers only — the wording of a
 * message belongs to whoever sends it.
 *
 * A quantity is a ladder of zones separated by ascending thresholds, plus the
 * index of the comfortable one, so a two-sided band (too cold / too warm) and a
 * one-sided ceiling (CO2) are the same shape. */

typedef enum {
    ALERT_Q_CO2,
    ALERT_Q_TEMP,
    ALERT_Q_RH,
    ALERT_Q_DEW_SPREAD, /* falling is the bad direction, so its comfort zone is
                         * the top one and severity comes out negative */
    ALERT_Q_UVI,
    ALERT_Q_TREND,      /* pressure, either way out of the middle */
    ALERT_Q_GUST,
    ALERT_Q_OUT_TEMP,
    ALERT_Q_COUNT,
} alert_q_t;

/* The sources the rules read, sampled together so every rule in one pass sees
 * the same instant. wx_ok covers the fetch; the other two carry their own
 * per-quantity flags. */
typedef struct {
    const climate_t          *cl;
    const weather_api_data_t *wx;
    bool                      wx_ok;
    const zambretti_t        *zb;
} alert_inputs_t;

/* Zone of v, entered from `prev`: a zone is left only once its threshold is
 * cleared by the hysteresis margin, so a reading sitting on a threshold does
 * not flap. A caller that keeps no state passes alert_comfort(). */
int alert_zone(alert_q_t q, double v, int prev);

/* Distance from the comfortable zone: 0 is fine, the sign is the side, the
 * magnitude is how far out — 1 warning, 2 bad, 3 alarming. */
int alert_severity(alert_q_t q, int zone);

int alert_comfort(alert_q_t q);

/* The threshold under zone z, i.e. the one crossed between z-1 and z. */
double alert_edge(alert_q_t q, int z);

/* The reading rule q watches. False when nothing is behind it — the rule then
 * holds whatever it had. */
bool alert_sample(alert_q_t q, const alert_inputs_t *in, double *v);
