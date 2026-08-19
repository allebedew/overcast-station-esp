#include "alert_rules.h"

#define EDGES_MAX 4

/* Hysteresis is set at each sensor's own noise, so the margin costs no real
 * delay: a reading that has genuinely moved clears it at once. */
static const struct {
    double edges[EDGES_MAX];
    int    count;
    int    comfort;
    double hyst;
} RULES[ALERT_Q_COUNT] = {
    [ALERT_Q_CO2]         = { { 800, 1200, 2000 },  3, 0, 25 },
    [ALERT_Q_TEMP]        = { { 22, 30 },           2, 1, 0.3 },
    [ALERT_Q_RH]          = { { 20, 80 },           2, 1, 2 },
    [ALERT_Q_DEW_SPREAD]  = { { 1, 3 },             2, 2, 0.3 },
    [ALERT_Q_UVI]         = { { 8 },                1, 0, 0.3 },
    [ALERT_Q_TREND]       = { { -3, -1.6, 1.6, 3 }, 4, 2, 0.2 },
    [ALERT_Q_GUST]        = { { 50, 70 },           2, 0, 3 },
    [ALERT_Q_OUT_TEMP]    = { { -10, 0, 30 },       3, 2, 0.5 },
};

int alert_zone(alert_q_t q, double v, int prev)
{
    int z = prev < 0 ? 0 : prev > RULES[q].count ? RULES[q].count : prev;

    while (z < RULES[q].count && v > RULES[q].edges[z] + RULES[q].hyst) {
        z++;
    }
    while (z > 0 && v < RULES[q].edges[z - 1] - RULES[q].hyst) {
        z--;
    }
    return z;
}

int alert_severity(alert_q_t q, int zone)
{
    return zone - RULES[q].comfort;
}

int alert_comfort(alert_q_t q)
{
    return RULES[q].comfort;
}

double alert_edge(alert_q_t q, int z)
{
    return RULES[q].edges[z - 1];
}

bool alert_sample(alert_q_t q, const alert_inputs_t *in, double *v)
{
    switch (q) {
    case ALERT_Q_CO2:
        *v = in->cl->co2_ppm;
        return in->cl->co2_ok;
    case ALERT_Q_TEMP:
        *v = in->cl->temp_c;
        return in->cl->temp_ok;
    case ALERT_Q_RH:
        *v = in->cl->rh_pct;
        return in->cl->rh_ok;
    case ALERT_Q_DEW_SPREAD:
        *v = in->cl->dew_spread_c;
        return in->cl->dew_spread_ok;
    case ALERT_Q_UVI:
        *v = in->wx->uvi;
        return in->wx_ok && in->wx->uvi != WEATHER_API_UVI_NONE;
    case ALERT_Q_TREND:
        *v = in->zb->delta_3h_hpa;
        return in->zb->ok;
    case ALERT_Q_GUST:
        *v = in->wx->gust_kmh;
        return in->wx_ok;
    case ALERT_Q_OUT_TEMP:
        *v = in->wx->temp_c;
        return in->wx_ok;
    default:
        return false;
    }
}
