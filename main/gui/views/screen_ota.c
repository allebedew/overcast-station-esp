#include "screen_ota.h"

#include <stdio.h>

#include "gfx_canvas.h"
#include "ui.h"

/* Where the block starts, picked so it sits roughly in the middle of the
 * panel: the whole screen is these three rows and nothing else. */
#define OTA_TOP 100

#define BAR_MARGIN 2
#define BAR_H      13   /* the outer frame; the fill is derived from it */

/* How far the fill sits inside the frame: its 1 px border and a 1 px gap. */
#define BAR_INSET 2

void screen_ota(gfx_canvas_t *c, size_t received, size_t total)
{
    gfx_clear(c, GFX_OFF);
    gfx_checker(c, (gfx_rect_t){ 0, 0, GFX_W, GFX_H }, (gfx_level_t)1, GFX_NONE, 1);

    int pct = total ? (int)(100 * (uint64_t)received / total) : 0;

    ui_cursor_t cur = { OTA_TOP };

    gfx_text(c, UI_RX / 2, ui_row(&cur, &UI_TEXT_C), &UI_TEXT_C, "Updating...");

    ui_gap(&cur, 3);

    gfx_rect_t bar = { BAR_MARGIN, cur.y, GFX_W - 2 * BAR_MARGIN, BAR_H };
    gfx_rect(c, bar, GFX_DIM, GFX_NONE, GFX_SOLID);
    int fill_w = (bar.w - 2 * BAR_INSET) * pct / 100;
    if (fill_w > 0) {
        gfx_rect(c, (gfx_rect_t){ (int16_t)(bar.x + BAR_INSET),
                                  (int16_t)(bar.y + BAR_INSET),
                                  (int16_t)fill_w, BAR_H - 2 * BAR_INSET },
                 GFX_NONE, GFX_FULL, GFX_SOLID);
    }

    /* The percentage rides inside the bar, drawn twice against the same
     * baseline and split at the edge of the fill: dark ink on the filled side,
     * lit on the empty one, so a digit the fill runs into inverts mid-glyph. */
    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%d%%", pct);
    int split = bar.x + BAR_INSET + fill_w;

    gfx_font_metrics_t fm;
    gfx_font_metrics(UI_TEXT_C.font, &fm);
    int baseline = bar.y + (BAR_H + fm.cap) / 2;

    gfx_text_style_t dark = UI_TEXT_C;
    dark.level = GFX_OFF;
    gfx_push(c, (gfx_rect_t){ 0, 0, (int16_t)split, GFX_H });
    gfx_text(c, UI_RX / 2, baseline, &dark, pct_str);
    gfx_pop(c);

    gfx_push(c, (gfx_rect_t){ (int16_t)split, 0, (int16_t)(GFX_W - split), GFX_H });
    gfx_text(c, UI_RX / 2 - split, baseline, &UI_TEXT_C, pct_str);
    gfx_pop(c);

    ui_gap(&cur, BAR_H + 6);

    gfx_textf(c, UI_RX / 2, ui_row(&cur, &UI_TEXT_C), &UI_TEXT_C, "%u / %u kB",
              (unsigned)(received / 1024), (unsigned)(total / 1024));
}
