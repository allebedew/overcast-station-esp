#include "ui.h"

const gfx_text_style_t UI_TEXT = { u8g2_font_04b_03_tr,     GFX_FULL, GFX_LEFT };
const gfx_text_style_t UI_BOLD = { u8g2_font_resoledbold_tr, GFX_FULL, GFX_LEFT };

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
    gfx_hline(c, 0, cur->y, GFX_W, GFX_FULL, 0x55, 0);
    ui_gap(cur, 1 + UI_GAP);   // past the rule's own row, then the gap
}

void ui_degree(gfx_canvas_t *c, int x, int top)
{
    gfx_px(c, x + 1, top,     GFX_FULL);
    gfx_px(c, x,     top + 1, GFX_FULL);
    gfx_px(c, x + 2, top + 1, GFX_FULL);
    gfx_px(c, x + 1, top + 2, GFX_FULL);
}

void ui_render(gfx_canvas_t *c, const ui_model_t *m)
{
    screen_now(c, m);
}
