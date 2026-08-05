#include "gfx_canvas.h"
#include "ui.h"

/* Scratch scene for panel experiments. Rewritten freely and referenced only by
 * gui.c and the host simulator, so nothing above depends on what it draws. */
void screen_test(gfx_canvas_t *c)
{
    ui_cursor_t cur = { 4 };

    gfx_clear(c, GFX_OFF);

    const gfx_text_style_t st = { UI_TEXT_C.font, 1, GFX_CENTER };
    gfx_text(c, GFX_W / 2, ui_row(&cur, &st), &st, "Hello, world!");

    const gfx_text_style_t st1 = { UI_TEXT_C.font, GFX_FULL, GFX_CENTER };
    gfx_text(c, GFX_W / 2, ui_row(&cur, &st), &st1, "Hello, world!");
}
