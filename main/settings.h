#pragma once

#include <stdint.h>

/* Хелперы над NVS-неймспейсом "settings". */

uint8_t settings_get_u8(const char *key, uint8_t def);

void settings_set_u8(const char *key, uint8_t value);
