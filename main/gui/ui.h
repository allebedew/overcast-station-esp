#pragma once

#include "gfx_canvas.h"
#include "gfx_text.h"
#include "ui_model.h"
#include "ui_state.h"

/* The layer between the drawing framework and the station's data: styles, a
 * vertical layout cursor and the screens themselves.
 *
 * Immediate mode — a frame is a function of the model and nothing else. No
 * widget objects, no invalidation: what is on the panel is the result of the
 * last ui_render(), and a full flush costs less than tracking what changed. */

extern const gfx_text_style_t UI_TEXT;
extern const gfx_text_style_t UI_TEXT_C;   /* the same, centred */
extern const gfx_text_style_t UI_TEXT_R;   /* the same, right-aligned */
extern const gfx_text_style_t UI_BOLD;

/* Narrower than UI_TEXT at the same height, for rows that have to fit more. */
extern const gfx_text_style_t UI_TINY;
extern const gfx_text_style_t UI_TINY_R;

/* x for a right-aligned run: the advance width the alignment subtracts includes
 * the blank column the font leaves after the last glyph, so the anchor is the
 * width itself, not the last column. */
#define UI_RX GFX_W

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

/* One frame. Does not flush — the caller owns the panel. */
void ui_render(gfx_canvas_t *c, const ui_model_t *m);

void screen_now(gfx_canvas_t *c, const ui_model_t *m);

/* The panel's own drive settings under the encoder, over a ramp of every gray
 * level. A tuning screen, not part of the station's UI, and the first one to
 * own its state and its input instead of leaving them to the caller. */
void       screen_panel(gfx_canvas_t *c);
ui_event_t screen_panel_input(const encoder_input_t *in);

/* Every field on one line, for the log — the only way a session's findings
 * leave the device. */
void screen_panel_format(char *buf, int n);

/* Scratch scene for panel experiments, model-free by design. */
void screen_test(gfx_canvas_t *c);
