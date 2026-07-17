#include "settings.h"

#include "nvs.h"

#define NVS_NAMESPACE "settings"

uint8_t settings_get_u8(const char *key, uint8_t def)
{
    nvs_handle_t h;
    uint8_t value = def;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &value);
        nvs_close(h);
    }
    return value;
}

void settings_set_u8(const char *key, uint8_t value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, value);
        nvs_commit(h);
        nvs_close(h);
    }
}
