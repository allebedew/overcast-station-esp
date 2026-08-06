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

/* RSSI to lit bars. The thresholds are the usual client-side ones: -55 dBm and
 * up is as good as the link ever gets indoors, below -75 it starts dropping
 * rates. No link is no bars, and an associated but weak link still shows one. */
static int sig_bars(bool link, int rssi)
{
    if (!link)       { return 0; }
    if (rssi >= -55) { return 4; }
    if (rssi >= -65) { return 3; }
    if (rssi >= -75) { return 2; }
    return 1;
}

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

/* Local time of the weather location: the clock runs in UTC and the offset is
 * applied by hand, so gmtime_r() over the shifted stamp gives local fields.
 * False before the first SNTP sync, when there is no time to show at all. */
static bool local_tm(const ui_model_t *m, struct tm *out)
{
    if (m->now == 0) {
        return false;
    }
    time_t t = m->now + m->utc_off_s;
    gmtime_r(&t, out);
    return true;
}

/* How old a reading is, in minutes or hours — the fetch runs hourly, so
 * seconds would be noise and days say no more than a large hour count. A
 * negative age is a fetch that has never succeeded. */
static void age_str(char *buf, size_t n, int32_t s)
{
    if (s < 0) {
        snprintf(buf, n, "--");
    } else if (s < 3600) {
        snprintf(buf, n, "%dm", (int)(s / 60));
    } else {
        snprintf(buf, n, "%dh", (int)(s / 3600));
    }
}

/* An already formatted value on a row, with its optional dim `label` and degree
 * sign. The label sits on the side facing the middle of the screen — past the
 * run for a left-aligned column, before it for a right-aligned one — so the two
 * columns read as a pair rather than as four runs. The sign stays with the
 * number, and a right-aligned run gives up its width at the anchor so it is the
 * sign that ends on it. `label` may be NULL. */
static void value(gfx_canvas_t *c, int x, int baseline, const gfx_text_style_t *st,
                  const char *s, const char *label, bool deg)
{
    gfx_text_style_t ls = *st;
    ls.level = GFX_DIM;

    if (st->align == GFX_RIGHT) {
        if (deg) { x -= DEG_W + 1; }
        int w = gfx_text(c, x, baseline, st, s);
        if (deg) { degree(c, x + 1, baseline, st); }
        if (label) { gfx_text(c, x - w - 3, baseline, &ls, label); }
    } else {
        int w = gfx_text(c, x, baseline, st, s);
        int r = x + w;
        if (deg) {
            degree(c, r + 1, baseline, st);
            r += DEG_W + 1;
        }
        if (label) { gfx_text(c, r + 3, baseline, &ls, label); }
    }
}

/* A reading: `fmt` over `v`, or `na` when its source has nothing to report —
 * every quantity on this screen carries such a flag, so the check belongs here
 * rather than around every draw. `na` spells out the empty form ("--.-") so the
 * row keeps the width of its digits. */
static void reading(gfx_canvas_t *c, int x, int baseline, const gfx_text_style_t *st,
                    bool ok, const char *fmt, const char *na, double v,
                    const char *label, bool deg)
{
    char b[16];
    if (ok) {
        snprintf(b, sizeof(b), fmt, v);
    } else {
        snprintf(b, sizeof(b), "%s", na);
    }
    value(c, x, baseline, st, b, label, deg);
}

/* Illuminance spans five decades and the row has three characters for it:
 * tenths below 1 lx, whole lux up to 1000, thousands above. The stored
 * resolution is 0.1 lx throughout — this row is the one that drops digits. */
static void lux_str(char *buf, size_t n, float lx)
{
    if (lx < 1.0f) {
        snprintf(buf, n, "%.1f", lx);
    } else if (lx < 1000.0f) {
        snprintf(buf, n, "%.0f", lx);
    } else {
        snprintf(buf, n, "%.1fk", lx / 1000.0f);
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
 * from unifont_t_77 instead. Both are 16x16. The table is filled in code by
 * code, so a code not in it yet — and no reading at all — falls back to the
 * first entry, the sun. */
#define WX_ICON_H 16

static const struct {
    int            code;
    const uint8_t *font;
    unsigned       cp;
    int8_t         dy;    /* baseline shift; the two fonts hang their glyphs
                           * differently and only the drawing shows by how much */
} WX_ICONS[] = {
    { 0,  u8g2_font_unifont_t_weather,   46, -3 },   /* clear sky: the sun */
    { 71, u8g2_font_unifont_t_77,      9924, -2 },   /* snow, all three rates */
    { 73, u8g2_font_unifont_t_77,      9924, -2 },
    { 75, u8g2_font_unifont_t_77,      9924, -2 },
};

static void wx_icon(int code, const uint8_t **font, unsigned *cp, int *dy)
{
    size_t hit = 0;
    for (size_t i = 1; i < sizeof(WX_ICONS) / sizeof(WX_ICONS[0]); i++) {
        if (WX_ICONS[i].code == code) {
            hit = i;
            break;
        }
    }
    *font = WX_ICONS[hit].font;
    *cp   = WX_ICONS[hit].cp;
    *dy   = WX_ICONS[hit].dy;
}

/* Current conditions as one block: the icon on the left edge, the temperature
 * and the conditions stacked to its right. The two text rows set the height of
 * the block; a taller icon hangs past the cursor. */
static void wx_now(gfx_canvas_t *c, ui_cursor_t *cur, const ui_model_t *m)
{
    char cond[16];

    if (m->out_ok) {
        size_t i = 0;
        for (; m->out_cond[i] && i < sizeof(cond) - 1; i++) {
            cond[i] = (char)toupper((unsigned char)m->out_cond[i]);
        }
        cond[i] = '\0';
    } else {
        snprintf(cond, sizeof(cond), "--");
    }
    const int code = m->out_ok ? m->out.weather_code : -1;

    gfx_font_metrics_t fm, cm;
    gfx_font_metrics(UI_BOLD.font, &fm);
    gfx_font_metrics(UI_TEXT.font, &cm);

    const uint8_t *icon_font;
    unsigned       icon_cp;
    int            icon_dy;
    wx_icon(code, &icon_font, &icon_cp, &icon_dy);

    gfx_text_style_t is = { icon_font, GFX_FULL, GFX_LEFT };
    int top = cur->y;
    int x   = gfx_glyph_w(&is, icon_cp) + UI_GAP;

    gfx_glyph(c, 0, top + WX_ICON_H + icon_dy, &is, icon_cp);

    // Whole degrees carry the reading and stay bold; the tenth is a detail and
    // drops to the text face. Split off the formatted string so rounding and the
    // sign are decided once, in printf.
    char temp[12];
    snprintf(temp, sizeof(temp), m->out_ok ? "%.1f" : "--.-", m->out.temp_c);
    char *frac = strchr(temp, '.');
    if (frac) {
        *frac++ = '\0';
    }

    int baseline = top + fm.ascent;
    int tw = gfx_text(c, x, baseline, &UI_BOLD, temp);
    if (frac) {
        tw += 1 + gfx_textf(c, x + tw + 1, baseline, &UI_TEXT, ".%s", frac);
    }
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

// Built up element by element: what is drawn here reads the model, the blocks
// still commented out are roughed-in layout waiting for theirs.
void screen_now(gfx_canvas_t *c, const ui_model_t *m, const ui_state_t *s)
{
    // Background fill

    gfx_clear(c, GFX_OFF);
    // gfx_checker(c, (gfx_rect_t){ 0, 0, GFX_W, GFX_H }, (gfx_level_t)1, GFX_NONE, 1);

    ui_cursor_t cur = { 0 };

    // Status bar

    struct tm tm;
    char day[4]   = "--";
    char hhmm[6]  = "--:--";
    if (local_tm(m, &tm)) {
        strftime(day,  sizeof(day),  "%a",    &tm);
        strftime(hhmm, sizeof(hhmm), "%H:%M", &tm);
    }

    int baseline = ui_row(&cur, &UI_TEXT);
    gfx_text(c, 0, baseline, &UI_TEXT, day);
    gfx_text(c, UI_RX/2, baseline, &UI_TEXT_C, hhmm);
    bars(c, UI_RX, baseline, sig_bars(m->link, m->rssi));
    // battery(c, UI_RX - SIG_W - 3, baseline, 0);

    char age[8];
    age_str(age, sizeof(age), m->out.age_s);

    baseline = ui_row(&cur, &UI_TEXT);
    gfx_text_style_t age_st = UI_TEXT_R;
    age_st.level = GFX_DIM;
    int aw = gfx_text(c, GFX_W, baseline, &age_st, age);
    gfx_push(c, (gfx_rect_t){ 0, 0, (int16_t)(UI_RX - aw - 2), GFX_H });
    gfx_text_bg(c, 0, baseline, &UI_TEXT, GFX_NONE, m->loc[0] ? m->loc : "--");
    gfx_pop(c);
    ui_rule(c, &cur);

    // Conditions with Icon

    wx_now(c, &cur, m);
    ui_rule(c, &cur);

    // Condition Labels

    bool ok = m->out_ok;

    baseline = ui_row(&cur, &UI_TEXT);
    reading(c, 0, baseline, &UI_TEXT, ok, "%.1f", "--.-", m->out.feels_c, "FL", true);
    reading(c, UI_RX, baseline, &UI_TEXT_R, ok, "%.1f", "---.-",
            m->out.pressure_msl_hpa, NULL, false);

    baseline = ui_row(&cur, &UI_TEXT);
    reading(c, 0, baseline, &UI_TEXT, ok, "%.0f%%", "--%", m->out.humidity_pct,
            NULL, false);
    // Speed and gusts as one range, in the units the API reports.
    if (ok) {
        gfx_textf(c, UI_RX, baseline, &UI_TEXT_R, "%s%.0f-%.0f",
                  weather_api_wind_dir_str(m->out.wind_dir_deg),
                  m->out.wind_kmh, m->out.gust_kmh);
    } else {
        gfx_text(c, UI_RX, baseline, &UI_TEXT_R, "---");
    }

    baseline = ui_row(&cur, &UI_TEXT);
    reading(c, 0, baseline, &UI_TEXT, ok, "%.1f", "--.-", m->out.uvi, "UV", false);
    reading(c, UI_RX, baseline, &UI_TEXT_R, ok, "%.0f%%", "--%",
            (double)m->out.cloud_pct, "CL", false);

    ui_rule(c, &cur);
/*
    // Forecast

    wx_forecast(c, &cur, m);
    ui_rule(c, &cur);

    // Sensor Labels
*/
    const climate_t *cl = &m->climate;

    baseline = ui_row(&cur, &UI_TEXT);
    reading(c, 0, baseline, &UI_TEXT, cl->temp_ok, "%.2f", "--.--", cl->temp_c,
            NULL, true);
    reading(c, UI_RX, baseline, &UI_TEXT_R, cl->press_ok, "%.3f", "---.---",
            cl->press_msl_hpa, NULL, false);

    baseline = ui_row(&cur, &UI_TEXT);
    reading(c, 0, baseline, &UI_TEXT, cl->rh_ok, "%.1f%%", "--.-%", cl->rh_pct,
            NULL, false);
    reading(c, UI_RX, baseline, &UI_TEXT_R, cl->co2_ok, "%.0f", "---",
            (double)cl->co2_ppm, "CO2", false);

    baseline = ui_row(&cur, &UI_TEXT);
    char lx[8] = "--";
    if (cl->lux_ok) {
        lux_str(lx, sizeof(lx), cl->lux);
    }
    value(c, 0, baseline, &UI_TEXT, lx, "Lx", false);
    reading(c, UI_RX, baseline, &UI_TEXT_R, cl->rh_ok, "%.1f", "--.-",
            cl->dew_c, "DW", true);

    ui_rule(c, &cur);

    // Brightness, pinned to the bottom corner: the knob has no other feedback
    // while the panel shows this screen. micro_tr has no descender, so the
    // baseline is the last row.
    gfx_textf(c, UI_RX, GFX_H - 1, &UI_MICRO_R, "%u", s->bright);
/*
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
    // baseline, which is what puts the last row on GFX_H - 1. Drawn once and
    // kept: rolled per frame it would be a different animal ten times a second.
    static unsigned cp;
    if (cp == 0) {
        cp = 0x20 + (unsigned)(rand() % 99);
    }

    const gfx_text_style_t zoo = { u8g2_font_unifont_t_animals, GFX_FULL, GFX_CENTER };
    gfx_glyph(c, GFX_W / 2, GFX_H - 2, &zoo, cp);

    */
}
