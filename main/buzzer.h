#pragma once

#include <stdint.h>

/* Passive piezo (KPT-1410) on GPIO19, driven by one LEDC channel.
 *
 * The element resonates at 4 kHz and falls off steeply either side, so the
 * tunes are told apart by rhythm inside that band, not by pitch — a musical
 * octave below it would barely be audible. */

typedef enum {
    /* Three ticks of the same length, spread across the usable band: the same
     * event sounds different without sounding like a different event. */
    BUZZER_CLICK_LO,
    BUZZER_CLICK,
    BUZZER_CLICK_HI,

    /* Well below resonance, to hear how much the element gives up down there. */
    BUZZER_TONE_2K5,
    BUZZER_TONE_1K5,

    BUZZER_OK,
    BUZZER_WARN,
    BUZZER_ALARM,
    BUZZER_ERROR,

    /* CO2 crossing a zone edge: pitch rises going up and falls coming down,
     * and the motif repeats once per zone crossed, so the level is countable
     * without watching the screen. */
    BUZZER_CO2_UP1,
    BUZZER_CO2_UP2,
    BUZZER_CO2_UP3,
    BUZZER_CO2_DOWN1,
    BUZZER_CO2_DOWN2,
    BUZZER_CO2_DOWN3,

    BUZZER_STORM,  /* lightning: three cracks and a rumble */
    BUZZER_ARRIVE, /* somebody in front of the station, and gone again */
    BUZZER_LEAVE,

    BUZZER_BOOT, /* rising three-note chirp at startup */
    BUZZER_TUNE_COUNT,
} buzzer_tune_t;

void buzzer_init(void);

/* Starts the tune, cutting off whatever is playing mid-note. Returns at once;
 * callable from any task. No-op before buzzer_init(). */
void buzzer_play(buzzer_tune_t tune);

void buzzer_stop(void);

/* Short label, at most ten characters — what the display has room for. */
const char *buzzer_tune_name(buzzer_tune_t tune);

/* Volume as a share of full swing. The fundamental of a square wave goes as
 * sin(pi*duty), so BUZZER_VOL_MAX is as loud as this element gets and halving
 * the number is not halving the sound: 50% -> 10% is about 10 dB. */
#define BUZZER_VOL_MIN 1
#define BUZZER_VOL_MAX 50

/* Applies at once and stays in RAM — one NVS write per knob detent is what
 * buzzer_save_volume() exists to avoid. Clamped to the range above. */
void buzzer_set_volume(uint8_t pct);
uint8_t buzzer_get_volume(void);

/* Persists whatever buzzer_set_volume() last took. */
void buzzer_save_volume(void);
