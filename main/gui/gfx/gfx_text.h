#pragma once

// Text drawn with u8g2 fonts. u8g2 is used as a glyph decoder only: its output is
// routed straight into a gfx_canvas_t at the style's gray level, so every string
// can have its own brightness without a compositing pass.
//
// Vertical placement is always the baseline. Anything that needs a string
// centred in a box computes it from gfx_font_metrics().

#include "gfx_canvas.h"
#include "gfx_fonts.h"

typedef enum {
    GFX_LEFT = 0,   // x is the left edge
    GFX_CENTER,     // x is the horizontal centre
    GFX_RIGHT,      // x is the right edge
} gfx_halign_t;

typedef struct {
    const uint8_t *font;
    gfx_level_t    level;
    gfx_halign_t   align;
} gfx_text_style_t;

typedef struct {
    int8_t ascent;       // above the baseline, "A" and taller
    int8_t descent;      // below the baseline, negative
    int8_t cap;          // height of a capital A
    int8_t line_height;  // suggested baseline-to-baseline distance
    int8_t bbx_w, bbx_h; // the font's own bounding box; the only usable size for
                         // an icon font, whose ascent is derived from a letter
                         // it does not have
} gfx_font_metrics_t;

// Strings are UTF-8. Returns the advance width, so a line can be assembled from
// runs of different styles.
int gfx_text(gfx_canvas_t *c, int x, int baseline, const gfx_text_style_t *st, const char *s);
int gfx_textf(gfx_canvas_t *c, int x, int baseline, const gfx_text_style_t *st, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

/* The same, over a filled box: the string's advance width by the font's line
 * box, so two strings in one font get plates of equal height whatever letters
 * they happen to contain. */
int gfx_text_bg(gfx_canvas_t *c, int x, int baseline, const gfx_text_style_t *st,
                gfx_level_t bg, const char *s);
int gfx_textf_bg(gfx_canvas_t *c, int x, int baseline, const gfx_text_style_t *st,
                 gfx_level_t bg, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));

/* One glyph by code point, for icon fonts whose glyphs are picked by number
 * rather than typed. */
int  gfx_glyph(gfx_canvas_t *c, int x, int baseline, const gfx_text_style_t *st, unsigned cp);
// The same, optionally flipped left-to-right within its own advance width.
int  gfx_glyph_m(gfx_canvas_t *c, int x, int baseline, const gfx_text_style_t *st,
                 unsigned cp, bool mirror);
int  gfx_glyph_w(const gfx_text_style_t *st, unsigned cp);

int  gfx_text_w(const gfx_text_style_t *st, const char *s);
void gfx_font_metrics(const uint8_t *font, gfx_font_metrics_t *m);
