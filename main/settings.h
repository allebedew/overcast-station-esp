#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Every persistent scalar setting the station has, in one table: key, default
 * and range live in settings.c, not in the module that happens to use the
 * value. Values are cached in RAM at init, so a get is a read, not an NVS
 * lookup -- the buzzer asks for its volume on every note. */

typedef enum {
    SETTING_LED_BRIGHTNESS,
    SETTING_BUZZER_VOLUME,
    SETTING_DISPLAY_ON,
    SETTING_DISPLAY_BRIGHT,
    SETTING_ALTITUDE_M,
    SETTING_RADAR_BT_OFF,
    SETTING_CHART_Q,
    SETTING_CHART_RANGE,
    SETTING_COUNT,
} setting_id_t;

typedef struct {
    const char *key;  /* NVS key, at most 15 characters */
    const char *api;  /* JSON key; NULL keeps the setting off the HTTP API. Kept
                       * apart from the NVS key, which is capped at 15
                       * characters and must not become a public contract */
    const char *name; /* label for a settings list */
    int32_t def;
    int32_t min;
    int32_t max;
    bool as_bool;     /* carried over the API as true/false, not as a number */
} setting_desc_t;

/* Loads every setting into the cache. Call once, after NVS is up and before
 * the modules that read their settings at init. */
void settings_init(void);

/* Applies a changed value wherever it is kept in RAM. Runs on the task that
 * called settings_set(), so it may only touch RAM — hardware is driven by
 * whoever owns it, which is why the panel polls its two settings instead of
 * registering here. Registered by the owning module in its own init. */
typedef void (*settings_apply_fn)(int32_t value);

void settings_on_change(setting_id_t id, settings_apply_fn fn);

const setting_desc_t *settings_desc(setting_id_t id);

int32_t settings_get(setting_id_t id);

/* Clamps to the setting's range and writes NVS only on a real change. */
esp_err_t settings_set(setting_id_t id, int32_t value);
