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

/* A degree sign, drawn rather than typed: no linked font carries U+00B0, and
 * 04b_03 has no Latin-1 variant upstream at all. Sits on the cap line of the run
 * it follows, so it takes that run's baseline and style. */
#define UI_DEG_W 3
void ui_degree(gfx_canvas_t *c, int x, int baseline, const gfx_text_style_t *st);

/* Signal strength: bars of rising height, the leftmost `bars` of them lit and
 * the rest dim. `right` is the anchor the way UI_RX is for text — one past the
 * last inked column — and the bars stand one row above `baseline`. */
#define UI_SIGNAL_BARS 4
#define UI_SIGNAL_W (2 * UI_SIGNAL_BARS - 1)   /* 1 px bars, 1 px apart */
#define UI_SIGNAL_H 5
void ui_signal(gfx_canvas_t *c, int right, int baseline, int bars);

/* Battery: a dim shell filled to `pct` at full brightness. Same anchor as
 * ui_signal(), so the two line up on one row. */
#define UI_BATT_W 7
#define UI_BATT_H 4
void ui_battery(gfx_canvas_t *c, int right, int baseline, int pct);

/* Chart: a plot area inset from the side edges. Drawn at the cursor, like
 * ui_rule(), and leaves the standard gap below it.
 *
 * One value per column, oldest on the left; a longer series keeps its newest
 * values, a shorter one is pushed to the right edge. NAN is a gap and draws
 * nothing — history records them, and a zero in their place would flatten the
 * rest. The vertical range is the series' own min..max, so the plot fills the
 * height and shows shape, not absolute level. That range is handed back through
 * lo_out/hi_out — NAN when the series holds no finite value — so a caller that
 * labels the chart does not scan the series again. Either may be NULL. */
#define UI_CHART_X 2
#define UI_CHART_W (GFX_W - 2 * UI_CHART_X)   /* 60 */
#define UI_CHART_H 24
#define UI_CHART_MAX UI_CHART_W
void ui_chart(gfx_canvas_t *c, ui_cursor_t *cur, const float *v, int n,
              float *lo_out, float *hi_out);

/* One frame. Does not flush — the caller owns the panel. */
void ui_render(gfx_canvas_t *c, const ui_model_t *m);

void screen_now(gfx_canvas_t *c, const ui_model_t *m);
