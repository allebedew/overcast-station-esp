#pragma once

// Drawing surface for the SSD1322 panel. Knows about pixels and nothing about
// the station's data: layout and widgets live a layer above and depend on this
// header, never the other way round.

#include <stdbool.h>
#include <stdint.h>

// The panel is 256x64 and is mounted rotated, so everything above this layer
// works in portrait. Which physical edge ends up on top is decided once by the
// SSD1322 re-map command, not here.
#define GFX_W 64
#define GFX_H 256

#define GFX_VP_DEPTH 8

// 0 = off, 15 = full brightness. GFX_NONE means "leave these pixels alone".
typedef int8_t gfx_level_t;
#define GFX_NONE  ((gfx_level_t)-1)
#define GFX_OFF   ((gfx_level_t)0)
#define GFX_HL    ((gfx_level_t)4)   // background plate a full-level string stays readable on
#define GFX_DIM   ((gfx_level_t)3)
#define GFX_DIM2  ((gfx_level_t)8)   // between dim and full, for a secondary run
#define GFX_FULL  ((gfx_level_t)15)

// Repeating 8-pixel dash mask; bit 0 is the first pixel of the pattern.
#define GFX_SOLID ((uint8_t)0xFF)

typedef struct {
    int16_t x, y, w, h;
} gfx_rect_t;

typedef struct {
    int16_t    ox, oy;   // local origin in absolute coordinates
    gfx_rect_t clip;     // absolute, already intersected with every parent
} gfx_viewport_t;

typedef struct {
    // Column-major, two vertically adjacent pixels per byte, high nibble = even y.
    // After the 90 degree rotation this is byte-identical to the SSD1322's own
    // layout, so flushing is a copy with no transpose.
    // Aligned because it is handed to SPI DMA as-is, without a bounce buffer.
    uint8_t        buf[GFX_W][GFX_H / 2] __attribute__((aligned(4)));
    gfx_viewport_t vp[GFX_VP_DEPTH];
    uint8_t        depth;
} gfx_canvas_t;

void gfx_init(gfx_canvas_t *c);
void gfx_clear(gfx_canvas_t *c, gfx_level_t level);

// Burn-in shift: the whole frame is drawn dy rows further down than its layout
// puts it, 0..GFX_SHIFT_MAX. It clips at the bottom rather than wrapping, so
// layouts keep that many rows free there; the top just gains a blank strip.
// Sets the root viewport, so it may only be called between frames.
#define GFX_SHIFT_MAX 5
void gfx_set_shift(gfx_canvas_t *c, int dy);
int  gfx_shift(const gfx_canvas_t *c);

// Every primitive takes local coordinates and is clipped to the current viewport.
// gfx_push returns false when the box is entirely clipped away -- drawing through
// it is still safe, so the return value is only worth checking to skip work.
bool       gfx_push(gfx_canvas_t *c, gfx_rect_t box);
void       gfx_pop(gfx_canvas_t *c);
gfx_rect_t gfx_bounds(const gfx_canvas_t *c);

void gfx_px(gfx_canvas_t *c, int x, int y, gfx_level_t level);

// phase shifts the dash pattern, so lines of one grid can be kept in step with
// each other no matter where each of them starts.
void gfx_hline(gfx_canvas_t *c, int x, int y, int len, gfx_level_t level, uint8_t dash, uint8_t phase);
void gfx_vline(gfx_canvas_t *c, int x, int y, int len, gfx_level_t level, uint8_t dash, uint8_t phase);

// fill covers the whole rect, stroke is then drawn on its 1px border; either may
// be GFX_NONE.
void gfx_rect(gfx_canvas_t *c, gfx_rect_t r, gfx_level_t stroke, gfx_level_t fill, uint8_t dash);

// Checkerboard of cell x cell squares: level for the top-left square and its
// diagonal, bg for the other one (GFX_NONE leaves those pixels alone). A cell of
// 1 is the finest halftone the panel can hold, 0 means a solid `level` fill; the
// pattern is anchored to the rect, not to the screen, so two rects never
// disagree about where a square starts.
void gfx_checker(gfx_canvas_t *c, gfx_rect_t r, gfx_level_t level, gfx_level_t bg, int cell);
