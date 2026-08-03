#pragma once

// The u8g2 fonts we actually link. Regenerate gfx_fonts.c with
// tools/extract_fonts.py after editing the list there.

#include <stdint.h>

#include "u8g2.h"

typedef struct {
    const char    *name;
    const uint8_t *font;
} gfx_font_entry_t;

// Every linked font, in the order listed in tools/extract_fonts.py. Exists so the
// host simulator can sweep all of them; firmware code names fonts directly.
extern const gfx_font_entry_t gfx_font_table[];
extern const int gfx_font_count;
