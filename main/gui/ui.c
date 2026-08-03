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

void ui_degree(gfx_canvas_t *c, int x, int top)
{
    gfx_px(c, x + 1, top,     GFX_FULL);
    gfx_px(c, x,     top + 1, GFX_FULL);
    gfx_px(c, x + 2, top + 1, GFX_FULL);
    gfx_px(c, x + 1, top + 2, GFX_FULL);
}

void ui_signal(gfx_canvas_t *c, int right, int bottom, int bars)
{
    for (int i = 0; i < 5; i++) {
        int h = UI_SIGNAL_H - 4 + i;   // 2..6, all standing on the same row
        gfx_vline(c, right - 2 * (4 - i), bottom - h + 1, h,
                  i < bars ? GFX_FULL : GFX_DIM, GFX_SOLID, 0);
    }
}

void ui_battery(gfx_canvas_t *c, int right, int bottom, int pct)
{
    int x = right - UI_BATT_W + 1;
    int y = bottom - UI_BATT_H + 1;

    // Shell, then the terminal nub on its right; the fill sits inside the walls.
    gfx_rect(c, (gfx_rect_t){ (int16_t)x, (int16_t)y, UI_BATT_W - 1, UI_BATT_H },
             GFX_DIM, GFX_NONE, GFX_SOLID);
    gfx_vline(c, right, y + 1, UI_BATT_H - 2, GFX_DIM, GFX_SOLID, 0);

    int inner = UI_BATT_W - 3;
    int fill  = (pct * inner + 50) / 100;
    if (fill > 0) {
        gfx_rect(c, (gfx_rect_t){ (int16_t)(x + 1), (int16_t)(y + 1), (int16_t)fill, UI_BATT_H - 2 },
                 GFX_NONE, GFX_FULL, GFX_SOLID);
    }
}

void ui_render(gfx_canvas_t *c, const ui_model_t *m)
{
    screen_now(c, m);
}
