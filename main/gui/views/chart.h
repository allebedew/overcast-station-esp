#pragma once

#include "gfx_canvas.h"
#include "history.h"

/* Declared rather than included: ui.h reaches back here for the window, and the
 * cursor is only ever passed by pointer. */
typedef struct ui_cursor ui_cursor_t;

/* The chart's window. Which ring it is decimated out of, by how much and what
 * the badge under the plot says are one row of a table rather than three
 * constants: they only ever make sense together. */
typedef enum {
    CHART_RANGE_1M,
    CHART_RANGE_5M,
    CHART_RANGE_1H,
    CHART_RANGE_1D,
    CHART_RANGE_COUNT,
} chart_range_t;

typedef struct {
    history_tier_t tier;
    uint8_t        stride;
    const char    *label;
} chart_range_def_t;

extern const chart_range_def_t CHART_RANGES[CHART_RANGE_COUNT];

/* Long enough for the widest window; the screen plots the newest of them. */
#define CHART_SERIES_MAX 64

/* The station's chart, as one element: a plot area inset from the side edges
 * with the ends of its scale and the window it covers labelled on a row under
 * it. Drawn at the cursor, like the screen's other elements, and leaves the
 * standard gap below.
 *
 * A widget and nothing more — the series and what it is are passed in, so it
 * neither samples the history nor knows how the two got picked. `q` is what the
 * values are, which is what decides how they are labelled and whether the
 * columns are placed linearly or by decade; `range` only labels the badge, and
 * `range_hl` plates that badge — how to draw it, not what the knob is doing.
 *
 * One value per column, oldest on the left; a longer series keeps its newest
 * values, a shorter one is pushed to the right edge. NAN is a gap and draws
 * nothing — history records them, and a zero in their place would flatten the
 * rest. */
void chart_draw(gfx_canvas_t *c, ui_cursor_t *cur, const float *v, int n,
                history_quantity_t q, chart_range_t range, bool range_hl);

/* The quantity's short name, for a caller that has to say which one is up.
 * Nothing on the panel spells it out yet. */
const char *chart_quantity_name(history_quantity_t q);
