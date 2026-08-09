#include "gfx_canvas.h"

#include <string.h>

static inline bool in_clip(const gfx_viewport_t *v, int ax, int ay)
{
    return ax >= v->clip.x && ax < v->clip.x + v->clip.w &&
           ay >= v->clip.y && ay < v->clip.y + v->clip.h;
}

static inline void put(gfx_canvas_t *c, int ax, int ay, gfx_level_t level)
{
    uint8_t *b = &c->buf[ax][ay >> 1];
    if (ay & 1) {
        *b = (uint8_t)((*b & 0xF0) | (uint8_t)level);
    } else {
        *b = (uint8_t)((*b & 0x0F) | (uint8_t)(level << 4));
    }
}

void gfx_init(gfx_canvas_t *c)
{
    memset(c, 0, sizeof(*c));
    c->depth = 0;
    c->vp[0] = (gfx_viewport_t){ .ox = 0, .oy = 0, .clip = { 0, 0, GFX_W, GFX_H } };
}

void gfx_set_shift(gfx_canvas_t *c, int dy)
{
    if (dy < 0)              { dy = 0; }
    if (dy > GFX_SHIFT_MAX)  { dy = GFX_SHIFT_MAX; }
    c->vp[0].oy = (int16_t)dy;
}

int gfx_shift(const gfx_canvas_t *c)
{
    return c->vp[0].oy;
}

void gfx_clear(gfx_canvas_t *c, gfx_level_t level)
{
    if (level == GFX_NONE) {
        return;
    }
    memset(c->buf, (uint8_t)(level << 4) | (uint8_t)level, sizeof(c->buf));
}

bool gfx_push(gfx_canvas_t *c, gfx_rect_t box)
{
    const gfx_viewport_t *parent = &c->vp[c->depth];

    // Overflowing the stack must not corrupt state, so the deepest viewport is
    // reused: drawing keeps working, it is just no longer nested any further.
    if (c->depth + 1 < GFX_VP_DEPTH) {
        c->depth++;
    }

    gfx_viewport_t *v = &c->vp[c->depth];
    v->ox = (int16_t)(parent->ox + box.x);
    v->oy = (int16_t)(parent->oy + box.y);

    int x0 = v->ox > parent->clip.x ? v->ox : parent->clip.x;
    int y0 = v->oy > parent->clip.y ? v->oy : parent->clip.y;
    int x1 = v->ox + box.w;
    int y1 = v->oy + box.h;
    if (x1 > parent->clip.x + parent->clip.w) x1 = parent->clip.x + parent->clip.w;
    if (y1 > parent->clip.y + parent->clip.h) y1 = parent->clip.y + parent->clip.h;

    v->clip = (gfx_rect_t){ (int16_t)x0, (int16_t)y0,
                            (int16_t)(x1 > x0 ? x1 - x0 : 0),
                            (int16_t)(y1 > y0 ? y1 - y0 : 0) };
    return v->clip.w > 0 && v->clip.h > 0;
}

void gfx_pop(gfx_canvas_t *c)
{
    if (c->depth > 0) {
        c->depth--;
    }
}

gfx_rect_t gfx_bounds(const gfx_canvas_t *c)
{
    const gfx_viewport_t *v = &c->vp[c->depth];
    return (gfx_rect_t){ (int16_t)(v->clip.x - v->ox), (int16_t)(v->clip.y - v->oy),
                         v->clip.w, v->clip.h };
}

void gfx_px(gfx_canvas_t *c, int x, int y, gfx_level_t level)
{
    if (level == GFX_NONE) {
        return;
    }
    const gfx_viewport_t *v = &c->vp[c->depth];
    int ax = x + v->ox, ay = y + v->oy;
    if (in_clip(v, ax, ay)) {
        put(c, ax, ay, level);
    }
}

void gfx_hline(gfx_canvas_t *c, int x, int y, int len, gfx_level_t level, uint8_t dash, uint8_t phase)
{
    if (level == GFX_NONE || len <= 0 || dash == 0) {
        return;
    }
    const gfx_viewport_t *v = &c->vp[c->depth];
    int ay = y + v->oy;
    if (ay < v->clip.y || ay >= v->clip.y + v->clip.h) {
        return;
    }

    int ax0 = x + v->ox;
    int ax1 = ax0 + len;
    if (ax0 < v->clip.x) ax0 = v->clip.x;
    if (ax1 > v->clip.x + v->clip.w) ax1 = v->clip.x + v->clip.w;

    for (int ax = ax0; ax < ax1; ax++) {
        // The pattern is anchored to local coordinates so that clipping a line
        // does not shift its dashes.
        int local = ax - v->ox;
        if (dash & (uint8_t)(1u << ((unsigned)(local + phase) & 7u))) {
            put(c, ax, ay, level);
        }
    }
}

void gfx_vline(gfx_canvas_t *c, int x, int y, int len, gfx_level_t level, uint8_t dash, uint8_t phase)
{
    if (level == GFX_NONE || len <= 0 || dash == 0) {
        return;
    }
    const gfx_viewport_t *v = &c->vp[c->depth];
    int ax = x + v->ox;
    if (ax < v->clip.x || ax >= v->clip.x + v->clip.w) {
        return;
    }

    int ay0 = y + v->oy;
    int ay1 = ay0 + len;
    if (ay0 < v->clip.y) ay0 = v->clip.y;
    if (ay1 > v->clip.y + v->clip.h) ay1 = v->clip.y + v->clip.h;

    for (int ay = ay0; ay < ay1; ay++) {
        int local = ay - v->oy;
        if (dash & (uint8_t)(1u << ((unsigned)(local + phase) & 7u))) {
            put(c, ax, ay, level);
        }
    }
}

void gfx_checker(gfx_canvas_t *c, gfx_rect_t r, gfx_level_t level, gfx_level_t bg, int cell)
{
    if (r.w <= 0 || r.h <= 0 || cell < 0) {
        return;
    }
    if (cell == 0) {
        for (int y = 0; y < r.h; y++) {
            gfx_hline(c, r.x, r.y + y, r.w, level, GFX_SOLID, 0);
        }
        return;
    }
    // Anchored to the rect, so the top-left square is always a set one.
    for (int y = 0; y < r.h; y++) {
        for (int x = 0; x < r.w; x += cell) {
            int len = r.w - x < cell ? r.w - x : cell;
            gfx_level_t l = ((x / cell + y / cell) & 1) ? bg : level;
            gfx_hline(c, r.x + x, r.y + y, len, l, GFX_SOLID, 0);
        }
    }
}

void gfx_rect(gfx_canvas_t *c, gfx_rect_t r, gfx_level_t stroke, gfx_level_t fill, uint8_t dash)
{
    if (r.w <= 0 || r.h <= 0) {
        return;
    }

    if (fill != GFX_NONE) {
        for (int y = 0; y < r.h; y++) {
            gfx_hline(c, r.x, r.y + y, r.w, fill, GFX_SOLID, 0);
        }
    }

    if (stroke != GFX_NONE) {
        gfx_hline(c, r.x, r.y, r.w, stroke, dash, 0);
        gfx_vline(c, r.x, r.y, r.h, stroke, dash, 0);
        if (r.h > 1) {
            gfx_hline(c, r.x, r.y + r.h - 1, r.w, stroke, dash, 0);
        }
        if (r.w > 1) {
            gfx_vline(c, r.x + r.w - 1, r.y, r.h, stroke, dash, 0);
        }
    }
}
