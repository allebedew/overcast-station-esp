#pragma once

// The one seam between drawing and output. On the station this is implemented by
// the SSD1322 transport; on the host it writes a PNG. Nothing above this layer
// knows which one it is talking to.

#include "gfx_canvas.h"

// The canvas layout already matches the panel's, so a full present is a copy of
// all 8 KB -- 8.2 ms over SPI at the 8 MHz the transport runs.
void gfx_present(const gfx_canvas_t *c);

// Partial present. Columns here are the panel's rows, so the range is free of the
// 4-pixel granularity that applies to the panel's own column addressing.
void gfx_present_cols(const gfx_canvas_t *c, int x0, int x1);

// How hard the panel is driven, 0..GFX_BRIGHTNESS_MAX. Everything else that
// scales the same current -- the contrast ceiling and the pre-charge -- is
// settled in the transport's init sequence, and this is what is left to turn.
// Ignored on the host.
void gfx_set_brightness(uint8_t level);

// 16 steps, because that is what the SSD1322's master current has. They are not
// evenly spaced in perceived brightness: the step from 0 to 1 doubles the
// current, the one from 14 to 15 adds 7%.
#define GFX_BRIGHTNESS_MAX     15
#define GFX_BRIGHTNESS_DEFAULT 0

// The rest of the drive settings, for screen_panel and nothing else: the ceiling
// the brightness scales down from, and the pre-charge, which lights the pixel
// outside the phase the drive current controls and sets the floor brightness
// cannot go below. Their values were found on the panel and are what the
// transport comes up at; these exist so they can be found again on another one.
void gfx_set_contrast(uint8_t contrast);
void gfx_set_precharge(uint8_t phase2, uint8_t second, uint8_t voltage);

#define GFX_CONTRAST_DEFAULT 0xFF
#define GFX_PHASE2_DEFAULT   9    // 3..15 DCLK
#define GFX_PRECHG2_DEFAULT  1    // 0..15 DCLK, and 0 is not the dim end
#define GFX_VPRECHG_DEFAULT  5    // 0..31, 0.20..0.60 * VCC
