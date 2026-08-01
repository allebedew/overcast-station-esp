#include "sun.h"

#include <math.h>

#include "freertos/FreeRTOS.h"

#include "timesync.h"
#include "weather_store.h"

#define DEG (M_PI / 180.0)

#define HORIZON_DEG (-0.833) /* refraction 34' + the disc's own radius 16' */
#define OBLIQUITY_DEG 23.4397

#define J2000     2451545.0 /* 2000-01-01 12:00 TT */
#define JD_UNIX   2440587.5
#define SECS_DAY  86400

static double jd_of(time_t t)
{
    return (double)t / (double)SECS_DAY + JD_UNIX;
}

static time_t unix_of(double jd)
{
    return (time_t)llround((jd - JD_UNIX) * (double)SECS_DAY);
}

/* Sine of the declination `t` days from J2000, and true noon's offset from
 * mean noon. */
static void sun_at(double t, double *sin_dec, double *noon_off)
{
    double m = fmod(357.5291 + 0.98560028 * t, 360.0); /* mean anomaly */
    /* equation of the center, truncated where the terms fall under a second */
    double c = 1.9148 * sin(m * DEG) + 0.0200 * sin(2 * m * DEG) +
               0.0003 * sin(3 * m * DEG);
    double lambda = fmod(m + c + 282.9372, 360.0); /* ecliptic longitude */
    *sin_dec = sin(lambda * DEG) * sin(OBLIQUITY_DEG * DEG);
    *noon_off = 0.0053 * sin(m * DEG) - 0.0069 * sin(2 * lambda * DEG);
}

/* Outside [-1, 1] the daily circle misses the horizon: above 1 the sun never
 * rises, below -1 it never sets. */
static double cos_hour_angle(double lat, double sin_dec)
{
    double cos_dec = sqrt(1.0 - sin_dec * sin_dec);
    return (sin(HORIZON_DEG * DEG) - sin(lat * DEG) * sin_dec) /
           (cos(lat * DEG) * cos_dec);
}

/* Three slots: a countdown reaches from today into both neighbours. */
#define CACHE_SLOTS 3

typedef struct {
    bool valid;
    double lat, lon;
    time_t noon;
    sun_info_t day;
} cache_slot_t;

static cache_slot_t s_cache[CACHE_SLOTS];
static int s_next_slot;

/* Guards the copy only — the trig stays outside; a race just recomputes. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static bool cache_get(double lat, double lon, time_t noon, sun_info_t *out)
{
    bool hit = false;
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (s_cache[i].valid && s_cache[i].noon == noon &&
            s_cache[i].lat == lat && s_cache[i].lon == lon) {
            *out = s_cache[i].day;
            hit = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return hit;
}

static void cache_put(double lat, double lon, time_t noon, const sun_info_t *day)
{
    taskENTER_CRITICAL(&s_lock);
    s_cache[s_next_slot] = (cache_slot_t){
        .valid = true, .lat = lat, .lon = lon, .noon = noon, .day = *day,
    };
    s_next_slot = (s_next_slot + 1) % CACHE_SLOTS;
    taskEXIT_CRITICAL(&s_lock);
}

/* Keyed on place and local noon, which fix the answer; `up` is not part of it. */
static void sun_day(double lat, double lon, time_t noon, sun_info_t *out)
{
    if (cache_get(lat, lon, noon, out)) {
        return;
    }

    /* Mean solar time at the meridian; eastern longitudes reach noon earlier
     * in UTC. The term must also sit inside the rounding, or a zone far from
     * its longitude's solar time (UTC+13 at 175°E) picks the wrong day. */
    double n = round(jd_of(noon) - J2000 + 0.0008 + lon / 360.0);
    double jstar = n - lon / 360.0;
    double sin_dec, noon_off;
    sun_at(jstar, &sin_dec, &noon_off);
    double transit = J2000 + jstar + noon_off; /* true solar noon */

    double cos_w = cos_hour_angle(lat, sin_dec);
    if (cos_w >= 1.0 || cos_w <= -1.0 || isnan(cos_w)) {
        bool never_sets = cos_w <= -1.0;
        *out = (sun_info_t){
            .state = never_sets ? SUN_POLAR_DAY : SUN_POLAR_NIGHT,
            .day_len_s = never_sets ? SECS_DAY : 0,
        };
    } else {
        double half = (acos(cos_w) / DEG) / 360.0; /* days from noon to either */
        time_t rise = unix_of(transit - half);
        time_t set = unix_of(transit + half);
        *out = (sun_info_t){
            .state = SUN_RISES,
            .rise = rise,
            .set = set,
            .day_len_s = (int32_t)(set - rise),
        };
    }
    cache_put(lat, lon, noon, out);
}

void sun_compute(double lat, double lon, int32_t utc_offset_s, time_t now,
                 int day_offset, sun_info_t *out)
{
    /* Anchored on local noon, the instant furthest from either day boundary. */
    long long local = (long long)now + utc_offset_s;
    long long day = local / SECS_DAY - (local % SECS_DAY < 0 ? 1 : 0);
    time_t noon = (time_t)((day + day_offset) * SECS_DAY + SECS_DAY / 2 -
                           utc_offset_s);

    sun_day(lat, lon, noon, out);
    /* From the angle, not from this day's pair: where true noon falls well
     * after the local one, the small hours belong to the previous day's arc. */
    out->up = sun_elevation_at(lat, lon, now) > HORIZON_DEG;
}

double sun_elevation_at(double lat, double lon, time_t now)
{
    double t = jd_of(now) - J2000;
    double sin_dec, noon_off;
    sun_at(t, &sin_dec, &noon_off);

    /* Hour angle past true noon. Which day the transit lands on is immaterial:
     * a day is 360° and the cosine is periodic. */
    double transit = round(t + 0.0008) - lon / 360.0 + noon_off;
    double h = (t - transit) * 360.0;

    double cos_dec = sqrt(1.0 - sin_dec * sin_dec);
    double sin_alt = sin(lat * DEG) * sin_dec +
                     cos(lat * DEG) * cos_dec * cos(h * DEG);
    return asin(sin_alt) / DEG;
}

bool sun_get(int day_offset, sun_info_t *out)
{
    weather_location_t loc;
    if (!timesync_is_synced() || !weather_store_get_active_location(&loc)) {
        return false;
    }
    /* Only picks the local day, so an hour of error is harmless: until a fetch
     * supplies the real offset, the longitude's own solar time stands in. */
    int32_t off = loc.utc_offset_s != WEATHER_TZ_UNKNOWN
                      ? loc.utc_offset_s
                      : (int32_t)lround(loc.lon / 15.0) * 3600;
    sun_compute(loc.lat, loc.lon, off, time(NULL), day_offset, out);
    return true;
}

bool sun_elevation_deg(double *out)
{
    weather_location_t loc;
    if (!timesync_is_synced() || !weather_store_get_active_location(&loc)) {
        return false;
    }
    *out = sun_elevation_at(loc.lat, loc.lon, time(NULL));
    return true;
}

bool sun_next_event(int32_t *in_s, bool *is_rise)
{
    sun_info_t today;
    if (!sun_get(0, &today) || today.state != SUN_RISES) {
        return false;
    }

    time_t now = time(NULL);
    time_t next;
    bool rise;
    sun_info_t yday;
    /* A day running past local midnight leaves its sunset ahead of today's
     * sunrise. */
    if (now < today.rise && sun_get(-1, &yday) && yday.state == SUN_RISES &&
        now < yday.set) {
        next = yday.set;
        rise = false;
    } else if (now < today.rise) {
        next = today.rise;
        rise = true;
    } else if (now < today.set) {
        next = today.set;
        rise = false;
    } else {
        sun_info_t tomorrow;
        if (!sun_get(1, &tomorrow) || tomorrow.state != SUN_RISES) {
            return false;
        }
        next = tomorrow.rise;
        rise = true;
    }

    *in_s = (int32_t)(next - now);
    *is_rise = rise;
    return true;
}
