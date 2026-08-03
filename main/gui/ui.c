#include <math.h>

#include "ui.h"
#include "gfx_canvas.h"

const gfx_text_style_t UI_TEXT   = { u8g2_font_04b_03_tr,      GFX_FULL, GFX_LEFT  };
const gfx_text_style_t UI_TEXT_C = { u8g2_font_04b_03_tr,      GFX_FULL, GFX_CENTER };
const gfx_text_style_t UI_TEXT_R = { u8g2_font_04b_03_tr,      GFX_FULL, GFX_RIGHT };
const gfx_text_style_t UI_BOLD   = { u8g2_font_resoledbold_tr, GFX_FULL, GFX_LEFT  };
const gfx_text_style_t UI_TINY   = { u8g2_font_3x5im_tr,       GFX_FULL, GFX_LEFT  };
const gfx_text_style_t UI_TINY_R = { u8g2_font_3x5im_tr,       GFX_FULL, GFX_RIGHT };

int ui_row(ui_cursor_t *cur, const gfx_text_style_t *st)
{
    gfx_font_metrics_t m;
    gfx_font_metrics(st->font, &m);

    int baseline = cur->y + m.ascent;
    cur->y = (int16_t)(cur->y + m.line_height);
    return baseline;
}

void ui_gap(ui_cursor_t *cur, int px)
{
    cur->y = (int16_t)(cur->y + px);
}

// Drawn at the cursor, not below a gap of its own: how far it sits from the row
// above depends on whether that row has descenders, which only the caller knows.
void ui_rule(gfx_canvas_t *c, ui_cursor_t *cur)
{
    gfx_hline(c, 0, cur->y, GFX_W, GFX_DIM, 0x55, 0);
    ui_gap(cur, 1 + UI_GAP);   // past the rule's own row, then the gap
}

void ui_degree(gfx_canvas_t *c, int x, int baseline, const gfx_text_style_t *st)
{
    gfx_font_metrics_t m;
    gfx_font_metrics(st->font, &m);
    int top = baseline - m.cap;

    gfx_px(c, x + 1, top,     GFX_FULL);
    gfx_px(c, x,     top + 1, GFX_FULL);
    gfx_px(c, x + 2, top + 1, GFX_FULL);
    gfx_px(c, x + 1, top + 2, GFX_FULL);
}

void ui_signal(gfx_canvas_t *c, int right, int baseline, int bars)
{
    for (int i = 0; i < UI_SIGNAL_BARS; i++) {
        int h = UI_SIGNAL_H - UI_SIGNAL_BARS + 1 + i;   // 2..UI_SIGNAL_H
        gfx_vline(c, right - 1 - 2 * (UI_SIGNAL_BARS - 1 - i), baseline - h, h,
                  i < bars ? GFX_FULL : GFX_DIM, GFX_SOLID, 0);
    }
}

void ui_battery(gfx_canvas_t *c, int right, int baseline, int pct)
{
    if (pct < 0)   { pct = 0; }
    if (pct > 100) { pct = 100; }

    int x     = right - UI_BATT_W;
    int y     = baseline - UI_BATT_H;
    int shell = UI_BATT_W - 1;   // the nub is the last of the UI_BATT_W columns

    // The charge is the outline itself lit over the first `fill` columns; the
    // inside stays empty, so at this size the level reads off the length of a
    // wall rather than an area two pixels tall. Rounding down keeps the nub --
    // the last column -- for a true 100%, and any charge at all is worth one.
    int fill = pct == 100 ? UI_BATT_W : pct * UI_BATT_W / 100;
    if (fill == 0 && pct > 0) {
        fill = 1;
    }
    int walls = fill < shell ? fill : shell;

    gfx_rect(c, (gfx_rect_t){ (int16_t)x, (int16_t)y, (int16_t)shell, UI_BATT_H },
             GFX_DIM, GFX_NONE, GFX_SOLID);

    // A single column cannot hold a partial level, so the shell's right wall and
    // the nub are each either reached or not.
    gfx_vline(c, x + shell - 1, y + 1, UI_BATT_H - 2,
              fill >= shell ? GFX_FULL : GFX_DIM, GFX_SOLID, 0);
    gfx_vline(c, right - 1, y + 1, UI_BATT_H - 2,
              fill >= UI_BATT_W ? GFX_FULL : GFX_DIM, GFX_SOLID, 0);

    if (walls > 0) {
        gfx_hline(c, x, y,                 walls, GFX_FULL, GFX_SOLID, 0);
        gfx_hline(c, x, y + UI_BATT_H - 1, walls, GFX_FULL, GFX_SOLID, 0);
        gfx_vline(c, x, y, UI_BATT_H, GFX_FULL, GFX_SOLID, 0);
    }
}

/* The fill under the line, brightest at the top of the box and fading down.
 * CHART_GRAD 0 leaves the line bare; CHART_GRAD_CHECKER inks every other pixel
 * instead of every one, thinning the fill on top of the fade. */
#define CHART_GRAD         1
#define CHART_GRAD_CHECKER 1
#define CHART_GRAD_TOP     6
#define CHART_GRAD_BOT     0

void ui_chart(gfx_canvas_t *c, ui_cursor_t *cur, const float *v, int n,
              float *lo_out, float *hi_out)
{
    int top = cur->y;
    ui_gap(cur, UI_CHART_H + UI_GAP);

    if (n > UI_CHART_MAX) {
        v += n - UI_CHART_MAX;
        n  = UI_CHART_MAX;
    }

    float lo = 0, hi = 0;
    bool  any = false;
    for (int i = 0; i < n; i++) {
        if (!isfinite(v[i])) { continue; }
        if (!any || v[i] < lo) { lo = v[i]; }
        if (!any || v[i] > hi) { hi = v[i]; }
        any = true;
    }
    if (lo_out) { *lo_out = any ? lo : NAN; }
    if (hi_out) { *hi_out = any ? hi : NAN; }
    if (!any) { return; }

    float span   = hi - lo;
    int   bottom = top + UI_CHART_H - 1;
    int   x      = UI_CHART_X + UI_CHART_W - n;   // newest value on the right edge

    for (int i = 0; i < n; i++) {
        if (!isfinite(v[i])) { continue; }
        int y = span > 0
                    ? bottom - (int)lroundf((v[i] - lo) / span * (UI_CHART_H - 1))
                    : top + (UI_CHART_H - 1) / 2;

#if CHART_GRAD
        // Anchored to the box, not to the line, so the columns add up to one
        // gradient instead of a wedge under every point.
        for (int r = y + 1; r <= bottom; r++) {
#if CHART_GRAD_CHECKER
            // Anchored to the box's own corner, so the pattern does not shift
            // with where the series happens to start.
            if ((((x + i - UI_CHART_X) + (r - top)) & 1) != 0) { continue; }
#endif
            gfx_px(c, x + i, r,
                   (gfx_level_t)(CHART_GRAD_TOP - (r - top) * (CHART_GRAD_TOP - CHART_GRAD_BOT)
                                                      / (UI_CHART_H - 1)));
        }
#endif
        gfx_px(c, x + i, y, GFX_FULL);
    }
}

void ui_render(gfx_canvas_t *c, const ui_model_t *m)
{
    screen_now(c, m);
}
