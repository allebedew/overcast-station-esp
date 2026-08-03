#pragma once

#include "gfx_canvas.h"
#include "gfx_text.h"
#include "ui_model.h"

/* The layer between the drawing framework and the station's data: styles, a
 * vertical layout cursor and the screens themselves.
 *
 * Immediate mode — a frame is a function of the model and nothing else. No
 * widget objects, no invalidation: what is on the panel is the result of the
 * last ui_render(), and a full flush costs less than tracking what changed. */

extern const gfx_text_style_t UI_TEXT;
extern const gfx_text_style_t UI_BOLD;

/* Blank rows between one element's last inked row and the next one's first. */
#define UI_GAP 3

/* Vertical layout. Baselines are error-prone to compute by hand, so a row is
 * asked for by style and the cursor keeps the arithmetic. */
typedef struct {
    int16_t y;
} ui_cursor_t;

/* Returns the baseline to draw on and advances past the row. */
int  ui_row(ui_cursor_t *cur, const gfx_text_style_t *st);
void ui_gap(ui_cursor_t *cur, int px);

/* Full-width dashed separator, with the gap above and below it. */
void ui_rule(gfx_canvas_t *c, ui_cursor_t *cur);

/* A degree sign, drawn rather than typed: no linked font carries U+00B0, and
 * 04b_03 has no Latin-1 variant upstream at all. `top` is the cap line of the
 * text it follows. */
#define UI_DEG_W 3
void ui_degree(gfx_canvas_t *c, int x, int top);

/* One frame. Does not flush — the caller owns the panel. */
void ui_render(gfx_canvas_t *c, const ui_model_t *m);

void screen_now(gfx_canvas_t *c, const ui_model_t *m);
