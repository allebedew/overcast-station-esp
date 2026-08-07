#include "settings.h"

#include "esp_log.h"
#include "nvs.h"

#include "buzzer.h"
#include "chart.h"
#include "climate.h"
#include "gfx_target.h"
#include "history.h"

#define NVS_NAMESPACE "settings"

static const char *TAG = "settings";

typedef struct {
    setting_desc_t desc;
    nvs_type_t type; /* how the value is stored -- see the table below */
} entry_t;

/* The storage type is per setting and must not change: NVS rejects a read of
 * the wrong type, so a widened field would silently reset every station in the
 * field to the default. Ranges come from the modules that own the hardware. */
static const entry_t s_tab[SETTING_COUNT] = {
    [SETTING_LED_BRIGHTNESS] = {
        { .key = "led_bright", .api = "led_brightness", .name = "LED brightness",
          .def = 4, .min = 1, .max = 255 }, NVS_TYPE_U8 },
    [SETTING_BUZZER_VOLUME] = {
        { .key = "buzz_vol", .api = "buzzer_volume", .name = "Buzzer volume",
          .def = 10, .min = BUZZER_VOL_MIN, .max = BUZZER_VOL_MAX },
        NVS_TYPE_U8 },
    [SETTING_DISPLAY_ON] = {
        { .key = "disp_on", .api = "display_on", .name = "Display on",
          .def = 1, .min = 0, .max = 1, .as_bool = true }, NVS_TYPE_U8 },
    [SETTING_DISPLAY_BRIGHT] = {
        { .key = "disp_bright", .api = "display_brightness",
          .name = "Display brightness", .def = GFX_BRIGHTNESS_DEFAULT,
          .min = 0, .max = GFX_BRIGHTNESS_MAX }, NVS_TYPE_U8 },
    [SETTING_ALTITUDE_M] = {
        { .key = "altitude_m", .api = "altitude", .name = "Site altitude",
          .def = 0, .min = CLIMATE_ALTITUDE_MIN, .max = CLIMATE_ALTITUDE_MAX },
        NVS_TYPE_I32 },
    /* The state the LD2450 is meant to hold, carried into it by the radar task
     * whenever this changes. It is also the only account of that state there
     * is: the module keeps Bluetooth in its own flash and cannot be asked. */
    [SETTING_RADAR_BT_OFF] = {
        { .key = "radar_bt_off", .api = "radar_bt_off",
          .name = "Radar Bluetooth off", .def = 1, .min = 0, .max = 1,
          .as_bool = true }, NVS_TYPE_U8 },
    /* What the knob left the chart on. Off the HTTP API: it is where the panel
     * stands, not something to drive a station by. */
    [SETTING_CHART_Q] = {
        { .key = "chart_q", .api = NULL, .name = "Chart quantity",
          .def = HISTORY_Q_TEMP, .min = 0, .max = HISTORY_Q_COUNT - 1 },
        NVS_TYPE_U8 },
    [SETTING_CHART_RANGE] = {
        { .key = "chart_range", .api = NULL, .name = "Chart range",
          .def = CHART_RANGE_1M, .min = 0, .max = CHART_RANGE_COUNT - 1 },
        NVS_TYPE_U8 },
};

/* Where a changed value has to land besides NVS — see settings_on_change(). */
static settings_apply_fn s_apply[SETTING_COUNT];

/* Written by whoever changes a setting, read by the display and the buzzer
 * tasks; aligned 32-bit words, so a reader sees the old or the new value and
 * never a mix. */
static volatile int32_t s_value[SETTING_COUNT];

static int32_t clamp(const setting_desc_t *d, int32_t v)
{
    if (v < d->min) {
        return d->min;
    }
    return v > d->max ? d->max : v;
}

void settings_init(void)
{
    nvs_handle_t h;
    bool open = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK;

    for (setting_id_t id = 0; id < SETTING_COUNT; id++) {
        const entry_t *e = &s_tab[id];
        int32_t value = e->desc.def;
        if (open) {
            if (e->type == NVS_TYPE_U8) {
                uint8_t u8;
                if (nvs_get_u8(h, e->desc.key, &u8) == ESP_OK) {
                    value = u8;
                }
            } else {
                nvs_get_i32(h, e->desc.key, &value);
            }
        }
        s_value[id] = clamp(&e->desc, value);
    }

    if (open) {
        nvs_close(h);
    }
    ESP_LOGI(TAG, "%d setting(s) loaded", SETTING_COUNT);
}

void settings_on_change(setting_id_t id, settings_apply_fn fn)
{
    if (id < SETTING_COUNT) {
        s_apply[id] = fn;
    }
}

const setting_desc_t *settings_desc(setting_id_t id)
{
    return id < SETTING_COUNT ? &s_tab[id].desc : NULL;
}

int32_t settings_get(setting_id_t id)
{
    return id < SETTING_COUNT ? s_value[id] : 0;
}

esp_err_t settings_set(setting_id_t id, int32_t value)
{
    if (id >= SETTING_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const entry_t *e = &s_tab[id];
    value = clamp(&e->desc, value);
    if (value == s_value[id]) {
        return ESP_OK;
    }
    s_value[id] = value;

    /* Before the write, not after it: the value is live either way, and a full
     * flash is no reason for the station to keep acting on the old one. */
    if (s_apply[id]) {
        s_apply[id](value);
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s not saved: %s", e->desc.key, esp_err_to_name(err));
        return err;
    }
    err = e->type == NVS_TYPE_U8 ? nvs_set_u8(h, e->desc.key, (uint8_t)value)
                                 : nvs_set_i32(h, e->desc.key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s not saved: %s", e->desc.key, esp_err_to_name(err));
    }
    return err;
}
