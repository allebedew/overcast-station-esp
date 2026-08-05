#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_canvas.h"
#include "ui.h"

/* The main screen, built up one element at a time. The widgets below are here
 * rather than in ui.c because nothing else draws them yet; the first screen that
 * wants one is the reason to move it up. */

/* A degree sign, drawn rather than typed: no linked font carries U+00B0, and
 * 04b_03 has no Latin-1 variant upstream at all. Sits on the cap line of the run
 * it follows, so it takes that run's baseline and style. */
#define DEG_W 3

static void degree(gfx_canvas_t *c, int x, int baseline, const gfx_text_style_t *st)
{
    gfx_font_metrics_t m;
    gfx_font_metrics(st->font, &m);
    int top = baseline - m.cap;

    gfx_px(c, x + 1, top,     GFX_FULL);
    gfx_px(c, x,     top + 1, GFX_FULL);
    gfx_px(c, x + 2, top + 1, GFX_FULL);
    gfx_px(c, x + 1, top + 2, GFX_FULL);
}

/* Signal strength: bars of rising height, the leftmost `lit` of them at full
 * brightness and the rest dim. `right` is the anchor the way UI_RX is for text —
 * one past the last inked column — and the bars stand one row above `baseline`. */
#define SIG_BARS 4
#define SIG_W (2 * SIG_BARS - 1)   /* 1 px bars, 1 px apart */
#define SIG_H 5

static void bars(gfx_canvas_t *c, int right, int baseline, int lit)
{
    for (int i = 0; i < SIG_BARS; i++) {
        int h = SIG_H - SIG_BARS + 1 + i;   // 2..SIG_H
        gfx_vline(c, right - 1 - 2 * (SIG_BARS - 1 - i), baseline - h, h,
                  i < lit ? GFX_FULL : GFX_DIM, GFX_SOLID, 0);
    }
}

/* Battery: a dim shell filled to `pct` at full brightness. Same anchor as
 * bars(), so the two line up on one row. */
#define BATT_W 7
#define BATT_H 4

static void battery(gfx_canvas_t *c, int right, int baseline, int pct)
{
    if (pct < 0)   { pct = 0; }
    if (pct > 100) { pct = 100; }

    int x     = right - BATT_W;
    int y     = baseline - BATT_H;
    int shell = BATT_W - 1;   // the nub is the last of the BATT_W columns

    // The charge is the outline itself lit over the first `fill` columns; the
    // inside stays empty, so at this size the level reads off the length of a
    // wall rather than an area two pixels tall. Rounding down keeps the nub --
    // the last column -- for a true 100%, and any charge at all is worth one.
    int fill = pct == 100 ? BATT_W : pct * BATT_W / 100;
    if (fill == 0 && pct > 0) {
        fill = 1;
    }
    int walls = fill < shell ? fill : shell;

    gfx_rect(c, (gfx_rect_t){ (int16_t)x, (int16_t)y, (int16_t)shell, BATT_H },
             GFX_DIM, GFX_NONE, GFX_SOLID);

    // A single column cannot hold a partial level, so the shell's right wall and
    // the nub are each either reached or not.
    gfx_vline(c, x + shell - 1, y + 1, BATT_H - 2,
              fill >= shell ? GFX_FULL : GFX_DIM, GFX_SOLID, 0);
    gfx_vline(c, right - 1, y + 1, BATT_H - 2,
              fill >= BATT_W ? GFX_FULL : GFX_DIM, GFX_SOLID, 0);

    if (walls > 0) {
        gfx_hline(c, x, y,              walls, GFX_FULL, GFX_SOLID, 0);
        gfx_hline(c, x, y + BATT_H - 1, walls, GFX_FULL, GFX_SOLID, 0);
        gfx_vline(c, x, y, BATT_H, GFX_FULL, GFX_SOLID, 0);
    }
}

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

static void chart(gfx_canvas_t *c, ui_cursor_t *cur, const float *v, int n,
                  float *lo_out, float *hi_out)
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
    if (lo_out) { *lo_out = any ? lo : NAN; }
    if (hi_out) { *hi_out = any ? hi : NAN; }
    if (!any) { return; }

    float span   = hi - lo;
    int   bottom = top + CHART_H - 1;
    int   x      = CHART_X + CHART_W - n;   // newest value on the right edge

    for (int i = 0; i < n; i++) {
        if (!isfinite(v[i])) { continue; }
        int y = span > 0
                    ? bottom - (int)lroundf((v[i] - lo) / span * (CHART_H - 1))
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

/* WMO code to a glyph. unifont_t_weather re-encodes its icons into the ASCII
 * range, so '.' is the sun there; the snowman it has no glyph for and comes
 * from unifont_t_77 instead. Both are 16x16. A code that is not here draws no
 * icon. */
#define WX_ICON_H 16

static const struct {
    int            code;
    const uint8_t *font;
    unsigned       cp;
    int8_t         dy;    /* baseline shift; the two fonts hang their glyphs
                           * differently and only the drawing shows by how much */
} WX_ICONS[] = {
    { 0,  u8g2_font_unifont_t_weather,   46, -2 },   /* clear sky: the sun */
    { 71, u8g2_font_unifont_t_77,      9924, -2 },   /* snow, all three rates */
    { 73, u8g2_font_unifont_t_77,      9924, -2 },
    { 75, u8g2_font_unifont_t_77,      9924, -2 },
};

static bool wx_icon(int code, const uint8_t **font, unsigned *cp, int *dy)
{
    for (size_t i = 0; i < sizeof(WX_ICONS) / sizeof(WX_ICONS[0]); i++) {
        if (WX_ICONS[i].code == code) {
            *font = WX_ICONS[i].font;
            *cp   = WX_ICONS[i].cp;
            *dy   = WX_ICONS[i].dy;
            return true;
        }
    }
    return false;
}

/* Current conditions as one block: the icon on the left edge, the temperature
 * and the conditions stacked to its right. The two text rows set the height of
 * the block; a taller icon hangs past the cursor. */
static void wx_now(gfx_canvas_t *c, ui_cursor_t *cur, const ui_model_t *m)
{
    (void)m;   // placeholders for now

    const char *temp = "28.4";
    const char *cond = "CLEAR SKY";
    const int   code = 0;

    gfx_font_metrics_t fm, cm;
    gfx_font_metrics(UI_BOLD.font, &fm);
    gfx_font_metrics(UI_TEXT.font, &cm);

    const uint8_t *icon_font = NULL;
    unsigned       icon_cp   = 0;
    int            icon_dy   = 0;
    bool icon = wx_icon(code, &icon_font, &icon_cp, &icon_dy);

    gfx_text_style_t is = { icon_font, GFX_FULL, GFX_LEFT };
    int top = cur->y;
    int x   = icon ? gfx_glyph_w(&is, icon_cp) + UI_GAP : 0;

    if (icon) {
        gfx_glyph(c, 0, top + WX_ICON_H + icon_dy, &is, icon_cp);
    }

    int baseline = top + fm.ascent;
    int tw = gfx_text(c, x, baseline, &UI_BOLD, temp);
    degree(c, x + tw + 1, baseline, &UI_BOLD);
    gfx_text(c, x, baseline + UI_GAP + cm.ascent, &UI_TEXT, cond);

    ui_gap(cur, fm.ascent + UI_GAP + cm.line_height);
}

/* One row per day: weekday, the day's low, a bar, the day's high. The bar's dim
 * body is the range of the whole forecast and is the same on every row, so the
 * lit segment — that day's low to high, mapped into it — can be compared across
 * rows by position alone.
 *
 * Columns: 7 px for the weekday, then two characters' worth for either
 * temperature, one digit of precipitation on the far right, the rest for the
 * bar. */
#define FC_T_W   7
#define FC_P_W   3
#define FC_P_X   (UI_RX - FC_P_W - 3)   /* right edge of the high column */
#define FC_BAR_X (7 + 3 + FC_T_W)
#define FC_BAR_W (FC_P_X - FC_T_W - 2 - FC_BAR_X)

static void wx_forecast(gfx_canvas_t *c, ui_cursor_t *cur, const ui_model_t *m)
{
    (void)m;   // placeholders for now

    static const struct { const char *day; int lo, hi, pr; } DAYS[] = {
        { "Mo", -3, 12, 3 }, { "Tu", 1, 17, 8 }, { "We", -5, 9, 0 },
        { "Th",  2, 14, 5 }, { "Fr", 6, 19, 9 },
    };
    const size_t n = sizeof(DAYS) / sizeof(DAYS[0]);

    int lo = DAYS[0].lo, hi = DAYS[0].hi;
    for (size_t i = 1; i < n; i++) {
        if (DAYS[i].lo < lo) { lo = DAYS[i].lo; }
        if (DAYS[i].hi > hi) { hi = DAYS[i].hi; }
    }
    int span = hi - lo;

    for (size_t i = 0; i < n; i++) {
        int baseline = ui_row(cur, &UI_TINY);
        gfx_text(c, 0, baseline, &UI_TINY, DAYS[i].day);
        gfx_textf(c, FC_BAR_X - 2, baseline, &UI_TINY_R, "%d", DAYS[i].lo);
        gfx_textf(c, FC_P_X, baseline, &UI_TINY_R, "%d", DAYS[i].hi);
        gfx_textf(c, UI_RX,  baseline, &UI_TINY_R, "%d", DAYS[i].pr);
        gfx_px(c, FC_P_X + 1, baseline - 3, GFX_DIM);   // tells the two numbers apart

        // Centred on the digits, which ink from baseline - 4 to the baseline.
        int top = baseline - 4;
        gfx_checker(c, (gfx_rect_t){ FC_BAR_X, (int16_t)top, FC_BAR_W, 3 }, GFX_DIM, GFX_NONE, 1);

        int x0 = span ? (DAYS[i].lo - lo) * (FC_BAR_W - 1) / span : 0;
        int x1 = span ? (DAYS[i].hi - lo) * (FC_BAR_W - 1) / span : FC_BAR_W - 1;
        gfx_checker(c, (gfx_rect_t){ (int16_t)(FC_BAR_X + x0), (int16_t)top,
                                     (int16_t)(x1 - x0 + 1), 3 },
                    GFX_FULL, 12, 0);
    }
}

/* The chart between two tiny rows: the quantity and the window it covers above,
 * the range it was scaled to below — the low on the left, the high on the right,
 * at the ends of the box the plot maps them to. */
static void wx_chart(gfx_canvas_t *c, ui_cursor_t *cur, const ui_model_t *m)
{
    (void)m;   // placeholders for now

    float v[CHART_MAX];
    for (int i = 0; i < CHART_MAX; i++) {
        v[i] = sinf(i * 0.2f) * 3.0f + i * 0.05f;
    }

    int baseline = ui_row(cur, &UI_TEXT);
    gfx_text_bg(c, 0,  baseline, &UI_TEXT, GFX_HL, "Press");
    gfx_text_bg(c, UI_RX, baseline, &UI_TEXT_R, GFX_HL, "5m");

    float lo, hi;
    chart(c, cur, v, CHART_MAX, &lo, &hi);

    baseline = ui_row(cur, &UI_TINY);
    if (isfinite(lo)) {
        gfx_textf(c, CHART_X, baseline, &UI_TINY, "%.1f", lo);
    }
    if (isfinite(hi)) {
        gfx_textf(c, UI_RX - CHART_X, baseline, &UI_TINY_R, "%.1f", hi);
    }
}

// Text and graphics roughed in without the model behind them yet, to see how it
// will look.
void screen_now(gfx_canvas_t *c, const ui_model_t *m)
{
    // Background fill

    gfx_clear(c, GFX_OFF);
    // gfx_checker(c, (gfx_rect_t){ 0, 0, GFX_W, GFX_H }, (gfx_level_t)1, GFX_NONE, 1);

    ui_cursor_t cur = { 0 };

    // Status bar

    int baseline = ui_row(&cur, &UI_TEXT);
    gfx_text(c, 0, baseline, &UI_TEXT, "Mon");
    gfx_text(c, UI_RX/2, baseline, &UI_TEXT_C, "11:52");
    bars(c, UI_RX, baseline, 2);
    battery(c, UI_RX - SIG_W - 3, baseline, 0);

    baseline = ui_row(&cur, &UI_TEXT);
    gfx_text_bg(c, 0, baseline, &UI_TEXT, GFX_HL, "Abcdefg");
    gfx_text(c, GFX_W, baseline, &UI_TEXT_R, "1m");
    ui_rule(c, &cur);

    // Conditions with Icon

    wx_now(c, &cur, m);
    ui_rule(c, &cur);

    // Condition Labels

    baseline = ui_row(&cur, &UI_TEXT);
    int fl = gfx_text(c, 0, baseline, &UI_TEXT, "FL 32.1");
    degree(c, fl + 1, baseline, &UI_TEXT);
    gfx_text(c, UI_RX, baseline, &UI_TEXT_R, "1017.1");

    baseline = ui_row(&cur, &UI_TEXT);
    gfx_text(c, 0,     baseline, &UI_TEXT,   "52%");
    gfx_text(c, UI_RX, baseline, &UI_TEXT_R, "W12-20");

    baseline = ui_row(&cur, &UI_TEXT);
    gfx_text(c, 0,     baseline, &UI_TEXT,   "UV 6.3");
    gfx_text(c, UI_RX, baseline, &UI_TEXT_R, "CL 0%");

    ui_rule(c, &cur);

    // Forecast

    wx_forecast(c, &cur, m);
    ui_rule(c, &cur);

    // Sensor Labels

    baseline = ui_row(&cur, &UI_TEXT);
    int rt = gfx_text(c, 0, baseline, &UI_TEXT, "25.63");
    degree(c, rt + 1, baseline, &UI_TEXT);
    gfx_text(c, UI_RX, baseline, &UI_TEXT_R, "1017.744");

    baseline = ui_row(&cur, &UI_TEXT);
    gfx_text(c, 0,     baseline, &UI_TEXT,   "69.9%");
    gfx_text(c, UI_RX, baseline, &UI_TEXT_R, "CO2 1264");

    baseline = ui_row(&cur, &UI_TEXT);
    gfx_text(c, 0,     baseline, &UI_TEXT,   "Lx 183");
    gfx_text(c, UI_RX - DEG_W - 1, baseline, &UI_TEXT_R, "DW 14.4");
    degree(c, UI_RX - DEG_W, baseline, &UI_TEXT);

    ui_rule(c, &cur);

    // Chart

    wx_chart(c, &cur, m);
    ui_rule(c, &cur);

    // Sun Position

    baseline = ui_row(&cur, &UI_TEXT);
    gfx_text(c, 0,     baseline, &UI_TEXT,   "--ZAMBRETTI--");
    ui_rule(c, &cur);

    // Sun Position

    baseline = ui_row(&cur, &UI_TEXT);
    gfx_text(c, 0,     baseline, &UI_TEXT,   "--SUN--");
    ui_rule(c, &cur);

    // A random animal pinned to the bottom edge. unifont_t_animals carries one
    // unbroken run of glyphs from 0x20, and its ink sits one row below the
    // baseline, which is what puts the last row on GFX_H - 1
    const gfx_text_style_t zoo = { u8g2_font_unifont_t_animals, GFX_FULL, GFX_CENTER };
    gfx_glyph(c, GFX_W / 2, GFX_H - 2, &zoo, 0x20 + (unsigned)(rand() % 99));
}
