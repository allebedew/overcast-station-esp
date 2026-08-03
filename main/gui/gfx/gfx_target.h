#pragma once

// The one seam between drawing and output. On the station this is implemented by
// the SSD1322 transport; on the host it writes a PNG. Nothing above this layer
// knows which one it is talking to.

#include "gfx_canvas.h"

// The canvas layout already matches the panel's, so a full present is a copy of
// all 8 KB -- 6.6 ms over SPI at 10 MHz.
void gfx_present(const gfx_canvas_t *c);

// Partial present. Columns here are the panel's rows, so the range is free of the
// 4-pixel granularity that applies to the panel's own column addressing.
void gfx_present_cols(const gfx_canvas_t *c, int x0, int x1);
