#include "ui.h"

#include <stdio.h>

#include "gfx_canvas.h"
#include "gfx_text.h"
#include "screen_now.h"

const gfx_text_style_t UI_TEXT   = { u8g2_font_04b_03_tr,      GFX_FULL, GFX_LEFT  };
const gfx_text_style_t UI_TEXT_C = { u8g2_font_04b_03_tr,      GFX_FULL, GFX_CENTER };
const gfx_text_style_t UI_TEXT_R = { u8g2_font_04b_03_tr,      GFX_FULL, GFX_RIGHT };
const gfx_text_style_t UI_BOLD   = { u8g2_font_resoledbold_tr, GFX_FULL, GFX_LEFT  };
const gfx_text_style_t UI_TINY   = { u8g2_font_3x5im_tr,       GFX_FULL, GFX_LEFT  };
const gfx_text_style_t UI_TINY_C = { u8g2_font_3x5im_tr,       GFX_FULL, GFX_CENTER };
const gfx_text_style_t UI_TINY_R = { u8g2_font_3x5im_tr,       GFX_FULL, GFX_RIGHT };
const gfx_text_style_t UI_MICRO_R = { u8g2_font_micro_tr,      GFX_FULL, GFX_RIGHT };

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

// Tenths below 1 lx, whole lux up to 1000, thousands above; the stored
// resolution is 0.1 lx throughout.
void ui_lux_str(char *buf, size_t n, float lx)
{
    if (lx < 1.0f) {
        snprintf(buf, n, "%.1f", lx);
    } else if (lx < 1000.0f) {
        snprintf(buf, n, "%.0f", lx);
    } else {
        snprintf(buf, n, "%.1fk", lx / 1000.0f);
    }
}

void ui_render(gfx_canvas_t *c, const ui_model_t *m, const ui_state_t *s)
{
    screen_now(c, m, s);
}
