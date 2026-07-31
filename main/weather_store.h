#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Stored weather locations (NVS namespace "weather_loc"): named coordinates
 * plus the index of the one displayed. The web UI geocodes place names and
 * sends coordinates already numeric. All functions are thread-safe. */

#define WEATHER_MAX_LOCATIONS 10

typedef struct {
    char name[33]; /* display name, UTF-8 */
    float lat;     /* latitude, degrees (-90..90) */
    float lon;     /* longitude, degrees (-180..180) */
} weather_location_t;

/* Loads the list from NVS. It starts empty, and no weather is fetched until
 * the user adds a location. */
void weather_store_init(void);

/* Adds a location, or updates one with the same name. ESP_ERR_NO_MEM when the
 * list is full, ESP_ERR_INVALID_ARG on a bad name or coordinates. */
esp_err_t weather_store_add(const char *name, float lat, float lon);

/* Removes idx, keeping the active selection on a valid entry. */
esp_err_t weather_store_remove(int idx);

int weather_store_count(void);

/* Copies the location at idx; false when idx is outside the list. */
bool weather_store_get(int idx, weather_location_t *out);

/* Index of the active location, or -1 when the list is empty. */
int weather_store_get_active(void);

/* Selects the active location; ESP_ERR_INVALID_ARG if idx is out of range. */
esp_err_t weather_store_set_active(int idx);

/* Copies the active location atomically; false when the list is empty. */
bool weather_store_get_active_location(weather_location_t *out);
