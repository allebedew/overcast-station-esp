#include "chart.h"

#include <math.h>
#include <stdio.h>

#include "ui.h"

/* Every window is 60 columns wide, one per plotted pixel, so the stride is the
 * tier's slot count over 60 -- not a free choice. */
const chart_range_def_t CHART_RANGES[CHART_RANGE_COUNT] = {
    [CHART_RANGE_1M] = { HISTORY_5M,  1, "1m" },
    [CHART_RANGE_5M] = { HISTORY_5M,  5, "5m" },
    [CHART_RANGE_1H] = { HISTORY_1H, 12, "1h" },
    [CHART_RANGE_1D] = { HISTORY_1D, 24, "1d" },
};

/* The plot area itself, and the vertical scale it maps the series onto.
 *
 * The scale is the series' own min..max, so the line fills the height and shows
 * shape rather than absolute level. That range is handed back through
 * lo_out/hi_out — NAN when the series holds no finite value — so the labels do
 * not scan the series again. Either may be NULL.
 *
 * `log` places the columns by decade instead, for illuminance: linearly, one
 * daylit reading puts a whole evening of the same window on one row. The labels
 * stay in the real units, so only the mapping changes.
 *
 * `min_span` is the narrowest scale the box will hold, in mapped units — a
 * series flatter than that is centred in it rather than stretched over the
 * height, which is what keeps sensor noise from drawing as weather. The range
 * handed back is then the scale's, not the series', so the labels and the edges
 * of the box agree. */
#define CHART_X 2
#define CHART_W (GFX_W - 2 * CHART_X)   /* 60 */
#define CHART_H 24
#define CHART_MAX CHART_W

/* The fill under the line, brightest at the top of the box and fading down.
 * CHART_GRAD 0 leaves the line bare; CHART_GRAD_CHECKER inks every other pixel
 * instead of every one, thinning the fill on top of the fade. */
#define CHART_GRAD         1
#define CHART_GRAD_CHECKER 1
#define CHART_GRAD_TOP     6
#define CHART_GRAD_BOT     0

/* Darkness reads as a true 0.0 lx, which has no logarithm; a tenth of the
 * sensor's own resolution is as far down as the axis can mean anything. */
#define CHART_LOG_FLOOR 0.01f

static float chart_map(float v, bool log)
{
    return log ? log10f(v < CHART_LOG_FLOOR ? CHART_LOG_FLOOR : v) : v;
}

static void chart_plot(gfx_canvas_t *c, ui_cursor_t *cur, const float *v, int n,
                       bool log, float min_span, float *lo_out, float *hi_out)
{
    int top = cur->y;
    ui_gap(cur, CHART_H + UI_GAP);

    if (n > CHART_MAX) {
        v += n - CHART_MAX;
        n  = CHART_MAX;
    }

    float lo = 0, hi = 0;
    bool  any = false;
    for (int i = 0; i < n; i++) {
        if (!isfinite(v[i])) { continue; }
        if (!any || v[i] < lo) { lo = v[i]; }
        if (!any || v[i] > hi) { hi = v[i]; }
        any = true;
    }
    if (!any) {
        if (lo_out) { *lo_out = NAN; }
        if (hi_out) { *hi_out = NAN; }
        return;
    }

    float base = chart_map(lo, log);
    float span = chart_map(hi, log) - base;
    if (span < min_span) {
        base -= (min_span - span) / 2;   // the series keeps the middle of the box
        span  = min_span;
    }
    if (lo_out) { *lo_out = log ? powf(10.0f, base) : base; }
    if (hi_out) { *hi_out = log ? powf(10.0f, base + span) : base + span; }

    int bottom = top + CHART_H - 1;
    int x      = CHART_X + CHART_W - n;   // newest value on the right edge

    for (int i = 0; i < n; i++) {
        if (!isfinite(v[i])) { continue; }
        int y = span > 0
                    ? bottom - (int)lroundf((chart_map(v[i], log) - base) / span
                                            * (CHART_H - 1))
                    : top + (CHART_H - 1) / 2;

#if CHART_GRAD
        // Anchored to the box, not to the line, so the columns add up to one
        // gradient instead of a wedge under every point.
        for (int r = y + 1; r <= bottom; r++) {
#if CHART_GRAD_CHECKER
            // Anchored to the box's own corner, so the pattern does not shift
            // with where the series happens to start.
            if ((((x + i - CHART_X) + (r - top)) & 1) != 0) { continue; }
#endif
            gfx_px(c, x + i, r,
                   (gfx_level_t)(CHART_GRAD_TOP - (r - top) * (CHART_GRAD_TOP - CHART_GRAD_BOT)
                                                      / (CHART_H - 1)));
        }
#endif
        gfx_px(c, x + i, y, GFX_FULL);
    }
}

/* How each quantity labels its axis. The formats drop a digit against the
 * readings on the rest of the screen: the ends of a scale say how far the line
 * travelled, and the two of them share a 64 px row with the window badge.
 * Illuminance needs no format of its own — ui_lux_str() already fits five
 * decades into three characters — and is the one quantity placed by decade
 * rather than linearly. */
static const struct {
    const char *name;       /* nothing on the panel says it yet; see the log */
    const char *fmt;        /* NULL: ui_lux_str() */
    bool        log;
    float       min_span;   /* decades where log; see chart_plot() */
} CHART_STYLE[HISTORY_Q_COUNT] = {
    [HISTORY_Q_TEMP]  = { "temp",  "%.1f", false, 0.5f  },
    [HISTORY_Q_RH]    = { "rh",    "%.0f", false, 10.0f },
    [HISTORY_Q_PRESS] = { "press", "%.1f", false, 4.0f  },
    [HISTORY_Q_CO2]   = { "co2",   "%.0f", false, 200.0f },
    [HISTORY_Q_LUX]   = { "lux",   NULL,   true,  1.0f  },
};

const char *chart_quantity_name(history_quantity_t q)
{
    return CHART_STYLE[q].name;
}

static void chart_label(char *buf, size_t n, history_quantity_t q, float v)
{
    if (CHART_STYLE[q].fmt) {
        snprintf(buf, n, CHART_STYLE[q].fmt, v);
    } else {
        ui_lux_str(buf, n, v);
    }
}

void chart_draw(gfx_canvas_t *c, ui_cursor_t *cur, const float *v, int n,
                history_quantity_t q, chart_range_t range, bool range_hl)
{
    float lo, hi;
    chart_plot(c, cur, v, n, CHART_STYLE[q].log, CHART_STYLE[q].min_span,
               &lo, &hi);

    char b[12];
    int  baseline = ui_row(cur, &UI_TINY);
    if (isfinite(lo)) {
        chart_label(b, sizeof(b), q, lo);
        gfx_text(c, CHART_X, baseline, &UI_TINY, b);
    }

    gfx_text_bg(c, UI_RX/2, baseline, &UI_TINY_C, range_hl ? GFX_HL : GFX_NONE,
                CHART_RANGES[range].label);

    if (isfinite(hi)) {
        chart_label(b, sizeof(b), q, hi);
        gfx_text(c, UI_RX - CHART_X, baseline, &UI_TINY_R, b);
    }
}
