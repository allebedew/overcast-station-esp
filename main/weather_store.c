#include "weather_store.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"

#define NVS_NAMESPACE  "weather_loc"
#define NVS_KEY_LIST   "list"
#define NVS_KEY_ACTIVE "active"

static const char *TAG = "weather_store";

static weather_location_t s_locations[WEATHER_MAX_LOCATIONS];
static int s_count;
static int s_active = -1;
static SemaphoreHandle_t s_lock;

static void save(void)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
    if (s_count > 0) {
        ESP_ERROR_CHECK(nvs_set_blob(h, NVS_KEY_LIST, s_locations,
                                     s_count * sizeof(weather_location_t)));
    } else {
        nvs_erase_key(h, NVS_KEY_LIST);
    }
    ESP_ERROR_CHECK(nvs_set_i8(h, NVS_KEY_ACTIVE, (int8_t)s_active));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}

void weather_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        size_t size = sizeof(s_locations);
        esp_err_t r = nvs_get_blob(h, NVS_KEY_LIST, s_locations, &size);
        if (r == ESP_OK && size % sizeof(weather_location_t) == 0) {
            s_count = size / sizeof(weather_location_t);
        } else if (r == ESP_OK) {
            /* Records saved in an incompatible layout: drop them. */
            nvs_erase_key(h, NVS_KEY_LIST);
            nvs_commit(h);
            ESP_LOGW(TAG, "Discarded saved locations in old format");
        }
        int8_t active = -1;
        nvs_get_i8(h, NVS_KEY_ACTIVE, &active);
        s_active = active;
        nvs_close(h);
    }

    /* keep the active index within the loaded list */
    if (s_active < 0 || s_active >= s_count) {
        s_active = s_count ? 0 : -1;
    }
    ESP_LOGI(TAG, "%d location(s), active %d", s_count, s_active);
}

esp_err_t weather_store_add(const char *name, float lat, float lon)
{
    if (!name || !name[0] || strlen(name) > 32 ||
        !isfinite(lat) || !isfinite(lon) ||
        lat < -90 || lat > 90 || lon < -180 || lon > 180) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i;
    for (i = 0; i < s_count && strcmp(s_locations[i].name, name) != 0; i++) {}
    if (i == s_count) {
        if (s_count == WEATHER_MAX_LOCATIONS) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_NO_MEM;
        }
        s_count++;
    }
    strlcpy(s_locations[i].name, name, sizeof(s_locations[i].name));
    s_locations[i].lat = lat;
    s_locations[i].lon = lon;
    save();
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Location \"%s\" saved (%d total)", name, s_count);
    return ESP_OK;
}

esp_err_t weather_store_remove(int idx)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (idx < 0 || idx >= s_count) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    memmove(&s_locations[idx], &s_locations[idx + 1],
            (s_count - idx - 1) * sizeof(weather_location_t));
    s_count--;
    /* keep the active selection valid after the shift */
    if (s_active == idx) {
        s_active = s_count ? 0 : -1;
    } else if (s_active > idx) {
        s_active--;
    }
    save();
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Location removed (%d left)", s_count);
    return ESP_OK;
}

int weather_store_count(void)
{
    return s_count;
}

bool weather_store_get(int idx, weather_location_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (idx < 0 || idx >= s_count) {
        xSemaphoreGive(s_lock);
        return false;
    }
    *out = s_locations[idx];
    xSemaphoreGive(s_lock);
    return true;
}

int weather_store_get_active(void)
{
    return s_active;
}

esp_err_t weather_store_set_active(int idx)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (idx < 0 || idx >= s_count) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    bool changed = s_active != idx;
    s_active = idx;
    if (changed) {
        save();
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool weather_store_get_active_location(weather_location_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_active >= 0 && s_active < s_count;
    if (ok) {
        *out = s_locations[s_active];
    }
    xSemaphoreGive(s_lock);
    return ok;
}
