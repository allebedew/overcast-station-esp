#pragma once

#include <stdint.h>

/* Helpers over the "settings" NVS namespace. */

uint8_t settings_get_u8(const char *key, uint8_t def);

void settings_set_u8(const char *key, uint8_t value);

uint32_t settings_get_u32(const char *key, uint32_t def);

void settings_set_u32(const char *key, uint32_t value);

int32_t settings_get_i32(const char *key, int32_t def);

void settings_set_i32(const char *key, int32_t value);
