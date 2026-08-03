#include "gfx_text.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "u8g2.h"

// u8g2 is driven without a display or a buffer: only the font decoder runs, and
// its output arrives here as horizontal/vertical runs through draw_l90. The
// decoder keeps state in one static u8g2_t, so all text must be drawn from a
// single task.
static struct {
    u8g2_t         u8g2;
    gfx_canvas_t  *canvas;
    gfx_level_t    level;
    const uint8_t *font;
    bool           ready;
} g;

static void update_dimension(u8g2_t *u8g2)
{
    u8g2->width            = GFX_W;
    u8g2->height           = GFX_H;
    u8g2->pixel_buf_width  = GFX_W;
    u8g2->pixel_buf_height = GFX_H;
    u8g2->pixel_curr_row   = 0;
    u8g2->buf_y0           = 0;
    u8g2->buf_y1           = GFX_H;
}

static void update_page_win(u8g2_t *u8g2)
{
    // The whole canvas is always "the current page"; real clipping is the
    // canvas's job, this window only keeps u8g2's own arithmetic in range.
    u8g2->user_x0 = 0;
    u8g2->user_x1 = GFX_W;
    u8g2->user_y0 = 0;
    u8g2->user_y1 = GFX_H;
    u8g2->is_page_clip_window_intersection = 1;
}

static void draw_l90(u8g2_t *u8g2, u8g2_uint_t x, u8g2_uint_t y, u8g2_uint_t len, uint8_t dir)
{
    if (u8g2->draw_color == 0 || g.canvas == NULL) {
        return;
    }
    if (dir == 0) {
        gfx_hline(g.canvas, (int16_t)x, (int16_t)y, (int)len, g.level, GFX_SOLID, 0);
    } else {
        gfx_vline(g.canvas, (int16_t)x, (int16_t)y, (int)len, g.level, GFX_SOLID, 0);
    }
}

static const u8g2_cb_t gfx_u8g2_cb = { update_dimension, update_page_win, draw_l90 };

static void ensure_setup(void)
{
    if (g.ready) {
        return;
    }
    memset(&g.u8g2, 0, sizeof(g.u8g2));

    g.u8g2.cb            = &gfx_u8g2_cb;
    g.u8g2.font          = NULL;
    g.u8g2.draw_color    = 1;
    g.u8g2.font_decode.is_transparent = 1;   // background pixels are never emitted
    g.u8g2.font_decode.dir = 0;
    g.u8g2.font_height_mode = 0;
    g.u8g2.bitmap_transparency = 0;
    g.u8g2.is_auto_page_clear = 0;

    // u8g2_SetMaxClipWindow() lives in the parts of u8g2 we do not vendor.
    g.u8g2.clip_x0 = 0;
    g.u8g2.clip_x1 = GFX_W;
    g.u8g2.clip_y0 = 0;
    g.u8g2.clip_y1 = GFX_H;

    update_dimension(&g.u8g2);
    update_page_win(&g.u8g2);
    u8g2_SetFontPosBaseline(&g.u8g2);
    u8g2_SetFontMode(&g.u8g2, 1);

    g.ready = true;
}

static void select_font(const uint8_t *font)
{
    ensure_setup();
    if (g.font != font) {
        u8g2_SetFont(&g.u8g2, font);
        g.font = font;
    }
}

static int align_x(int x, const gfx_text_style_t *st, int width)
{
    switch (st->align) {
    case GFX_CENTER: return x - width / 2;
    case GFX_RIGHT:  return x - width;
    default:         return x;
    }
}

int gfx_text_w(const gfx_text_style_t *st, const char *s)
{
    select_font(st->font);
    return (int)u8g2_GetUTF8Width(&g.u8g2, s);
}

int gfx_text(gfx_canvas_t *c, int x, int baseline, const gfx_text_style_t *st, const char *s)
{
    select_font(st->font);
    int w = (int)u8g2_GetUTF8Width(&g.u8g2, s);

    if (st->level != GFX_NONE) {
        g.canvas = c;
        g.level  = st->level;
        u8g2_DrawUTF8(&g.u8g2, (u8g2_uint_t)align_x(x, st, w), (u8g2_uint_t)baseline, s);
        g.canvas = NULL;
    }
    return w;
}

int gfx_textf(gfx_canvas_t *c, int x, int baseline, const gfx_text_style_t *st, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return gfx_text(c, x, baseline, st, buf);
}

// u8g2 decodes UTF-8, so a code point goes back through the same path.
static void encode(unsigned cp, char out[5])
{
    int n = 0;
    if (cp < 0x80) {
        out[n++] = (char)cp;
    } else if (cp < 0x800) {
        out[n++] = (char)(0xC0 | (cp >> 6));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[n++] = (char)(0xE0 | (cp >> 12));
        out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    }
    out[n] = '\0';
}

int gfx_glyph(gfx_canvas_t *c, int x, int baseline, const gfx_text_style_t *st, unsigned cp)
{
    char s[5];
    encode(cp, s);
    return gfx_text(c, x, baseline, st, s);
}

int gfx_glyph_w(const gfx_text_style_t *st, unsigned cp)
{
    char s[5];
    encode(cp, s);
    return gfx_text_w(st, s);
}

void gfx_font_metrics(const uint8_t *font, gfx_font_metrics_t *m)
{
    select_font(font);
    m->ascent      = g.u8g2.font_ref_ascent;
    m->descent     = g.u8g2.font_ref_descent;
    m->cap         = g.u8g2.font_info.ascent_A;
    m->line_height = (int8_t)(m->ascent - m->descent + 1);
    m->bbx_w       = g.u8g2.font_info.max_char_width;
    m->bbx_h       = g.u8g2.font_info.max_char_height;
}
