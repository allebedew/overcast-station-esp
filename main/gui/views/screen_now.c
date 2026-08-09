#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chart.h"
#include "gfx_canvas.h"
#include "screen_now.h"
#include "ui.h"

/* The main screen, built up one element at a time. The widgets below are here
 * rather than in ui.c because nothing else draws them yet; the first screen that
 * wants one is the reason to move it up. */

/* Full-width dashed separator. Drawn at the cursor, not below a gap of its own:
 * how far it sits from the row above depends on whether that row has
 * descenders, which only the caller knows. */
static void rule(gfx_canvas_t *c, ui_cursor_t *cur)
{
    gfx_hline(c, 0, cur->y, GFX_W, GFX_DIM, 0x55, 0);
    ui_gap(cur, 1 + UI_GAP);   // past the rule's own row, then the gap
}

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

/* Signal strength: bars of rising height, the ones set in `mask` at full
 * brightness and the rest dim. `right` is the anchor the way UI_RX is for text —
 * one past the last inked column — and the bars stand one row above `baseline`.
 * A mask rather than a count because the connecting animation lights a single
 * bar somewhere in the middle. */
#define SIG_BARS 4
#define SIG_W (2 * SIG_BARS - 1)   /* 1 px bars, 1 px apart */
#define SIG_H 5

/* One step of the connecting animation, in ms: a single bar walks to the far end
 * and back, so a full sweep is 2 * SIG_BARS - 2 of these. */
#define SIG_STEP_MS 200

/* RSSI to lit bars. The thresholds are the usual client-side ones: -55 dBm and
 * up is as good as the link ever gets indoors, below -75 it starts dropping
 * rates. No link is no bars, and an associated but weak link still shows one.
 * While an attempt is on the air one bar sweeps back and forth instead. */
static unsigned sig_mask(ui_link_t link, int rssi, uint32_t anim_ms)
{
    if (link == UI_LINK_CONNECTING) {
        unsigned step = anim_ms / SIG_STEP_MS % (2 * SIG_BARS - 2);
        unsigned i    = step < SIG_BARS ? step : 2 * SIG_BARS - 2 - step;
        return 1u << i;
    }
    if (link != UI_LINK_UP) { return 0; }

    int lit = rssi >= -55 ? 4
            : rssi >= -65 ? 3
            : rssi >= -75 ? 2
                          : 1;
    return (1u << lit) - 1;
}

static void bars(gfx_canvas_t *c, int right, int baseline, unsigned mask)
{
    for (int i = 0; i < SIG_BARS; i++) {
        int h = SIG_H - SIG_BARS + 1 + i;   // 2..SIG_H
        gfx_vline(c, right - 1 - 2 * (SIG_BARS - 1 - i), baseline - h, h,
                  (mask >> i) & 1 ? GFX_FULL : GFX_DIM, GFX_SOLID, 0);
    }
}

/* While the SoftAP is up the bars have nothing to say — the station is not
 * associated with anything — so an inverted "AP" stands in their place. Same
 * anchor as bars(): `right` is one past the last inked column. 1 px of plate on
 * either side of the glyphs, which gfx_text_bg does not add. */
#define AP_PAD 1

static void ap_badge(gfx_canvas_t *c, int right, int baseline)
{
    gfx_text_style_t st = UI_TEXT_R;
    st.level = GFX_OFF;

    gfx_font_metrics_t fm;
    gfx_font_metrics(st.font, &fm);
    int w = gfx_text_w(&st, "AP") + 2 * AP_PAD;

    gfx_rect(c, (gfx_rect_t){ (int16_t)(right - w), (int16_t)(baseline - fm.ascent - 1),
                              (int16_t)w, (int16_t)(fm.ascent + 1) },
             GFX_NONE, GFX_FULL, GFX_SOLID);
    gfx_text(c, right - AP_PAD, baseline, &st, "AP");
}

/* Three dim dots with one lit, walking to the end and back -- the same gesture
 * the connecting link makes with its bars, shown where the reading's age is
 * while a fetch is running. Anchored and sized like the text it replaces --
 * `right` one past the last column, `w` the advance width the age would have
 * had -- so what is beside it does not move when the animation comes and goes.
 *
 * A function of `anim_ms` alone: the screen is redrawn from the model and keeps
 * no state between frames. */
#define DOTS     3
#define DOT_DX   3   /* dot pitch, px: "1h" is only eight columns wide */

static void dots(gfx_canvas_t *c, int right, int baseline, int w, uint32_t anim_ms)
{
    gfx_font_metrics_t fm;
    gfx_font_metrics(UI_TEXT.font, &fm);
    // A row above the cap's middle: level with the x-height the dim location
    // name beside it is drawn at.
    int y = baseline - fm.cap / 2 - 1;

    // Centred in the age's box, and left-aligned in it when the width is odd.
    int x0 = right - w + (w - (DOT_DX * (DOTS - 1) + 1)) / 2;

    unsigned step = anim_ms / SIG_STEP_MS % (2 * DOTS - 2);
    unsigned lit  = step < DOTS ? step : 2 * DOTS - 2 - step;

    for (unsigned i = 0; i < DOTS; i++) {
        gfx_px(c, x0 + (int)i * DOT_DX, y, i == lit ? GFX_FULL : GFX_DIM);
    }
}

/* Ping-pong scroll for a run wider than the box it has to sit in: `over` px of
 * overhang become an offset that walks a pixel every SCROLL_STEP_MS. The rest at
 * home is the long one — that is the reading's own position, and the run is only
 * pulled aside to show what did not fit; at the far end it waits just long
 * enough to read the tail. A function of `anim_ms` alone, like the other moving
 * parts — the screen keeps no state between frames. */
#define SCROLL_STEP_MS 60
#define SCROLL_HOME_MS 15000
#define SCROLL_END_MS  2000

static int scroll_off(uint32_t anim_ms, int over)
{
    if (over <= 0) {
        return 0;
    }
    uint32_t home  = SCROLL_HOME_MS / SCROLL_STEP_MS;
    uint32_t end   = SCROLL_END_MS / SCROLL_STEP_MS;
    uint32_t walk  = (uint32_t)over;
    uint32_t cycle = home + walk + end + walk;
    uint32_t t     = anim_ms / SCROLL_STEP_MS % cycle;

    if (t < home) {                          // resting at home
        return 0;
    }
    if (t < home + walk) {                   // walking to the end
        return (int)(t - home);
    }
    if (t < home + walk + end) {             // resting at the end
        return over;
    }
    return over - (int)(t - (home + walk + end));   // walking back
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
 * sign that ends on it. `label` may be NULL.
 *
 * `bg` plates the number, and the number alone: the label stays dim and outside
 * it, or a marked row reads as a block of ink rather than as a value. The
 * degree sign is a plain pixel run and keeps whatever the plate left behind. */
static void value(gfx_canvas_t *c, int x, int baseline, const gfx_text_style_t *st,
                  const char *s, const char *label, bool deg, gfx_level_t bg)
{
    gfx_text_style_t ls = *st;
    ls.level = GFX_DIM;

    if (st->align == GFX_RIGHT) {
        if (deg) { x -= DEG_W + 1; }
        int w = gfx_text_bg(c, x, baseline, st, bg, s);
        if (deg) { degree(c, x + 1, baseline, st); }
        if (label) { gfx_text(c, x - w - 3, baseline, &ls, label); }
    } else {
        int w = gfx_text_bg(c, x, baseline, st, bg, s);
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
                    const char *label, bool deg, gfx_level_t bg)
{
    char b[16];
    if (ok) {
        snprintf(b, sizeof(b), fmt, v);
    } else {
        snprintf(b, sizeof(b), "%s", na);
    }
    value(c, x, baseline, st, b, label, deg, bg);
}

/* The sensor rows are where the chart's quantity is picked, so while the knob
 * is on that field the one being plotted wears a plate. Dew point is on no
 * chart and never marked. */
static gfx_level_t q_bg(const ui_state_t *s, history_quantity_t q)
{
    return (s->focus == UI_FOCUS_CHART_Q && s->set.chart_q == q) ? GFX_HL
                                                                 : GFX_NONE;
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
    snprintf(cond, sizeof(cond), "%s", m->out_ok ? m->out_cond : "--");
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

    // Precipitation rate, inverted at the far end of the temperature's row: the
    // only marked run on the screen that is not a knob field, so the plate reads
    // as "it is raining" rather than as a selection — and dry weather shows
    // nothing at all. 1 px of plate on either side of the digits, which
    // gfx_text_bg does not add.
    if (m->out_ok && m->out.precip_mmh > 0) {
        gfx_text_style_t ps = UI_TEXT_R;
        ps.level = GFX_OFF;

        char precip[8];
        snprintf(precip, sizeof(precip), "%.1f", m->out.precip_mmh);
        int pw     = gfx_text_w(&ps, precip);
        int p_base = baseline - 1;

        gfx_rect(c, (gfx_rect_t){ (int16_t)(UI_RX - pw - 2 * AP_PAD),
                                  (int16_t)(p_base - cm.ascent - 1),
                                  (int16_t)(pw + 2 * AP_PAD), (int16_t)(cm.ascent + 2) },
                 GFX_NONE, GFX_FULL, GFX_SOLID);
        gfx_text(c, UI_RX - AP_PAD, p_base, &ps, precip);
    }

    ui_gap(cur, fm.ascent + UI_GAP + cm.line_height);
}

/* Rain over the next 24 hours: one column an hour, the leftmost the hour running
 * now. Every column is the same height and it is the brightness that carries the
 * probability, over the dim2..full range; a dry hour, an hour no forecast
 * reaches and the whole strip without data stay at dim. A 1 px column at a 2 px
 * pitch, broken by a wider gap at midnight, 06:00, noon and 18:00 local, is what
 * lets all 24 fit side by side on a 64 px panel and still be read against the
 * time of day. The strip is a group narrower on the hours where the first of
 * those falls on the first column: worth it for gaps that mean an hour rather
 * than an offset. */
#define RAIN_HOURS WEATHER_API_FORECAST_HOURS
#define RAIN_H     3                              /* column height, px */
#define RAIN_DX    2                              /* column pitch: 1 px inked, 1 px apart */
#define RAIN_GROUP 6                              /* hours between the wider gaps */
#define RAIN_GAP   3                              /* extra advance at a group boundary, px */
#define RAIN_X0    3                              /* left margin, px */

static void wx_rain(gfx_canvas_t *c, ui_cursor_t *cur, const ui_model_t *m)
{
    // The series starts at the hour it was fetched in, which is not the hour it
    // is being drawn in: skip the ones already gone, so the first column stays
    // the current hour as the reading ages.
    int skip = 0;
    if (m->out_ok && m->now && m->out.hour_count > 0) {
        skip = (int)(((m->now + m->utc_off_s) - m->out.hour_start) / 3600);
        if (skip < 0) { skip = 0; }
    }

    // Local hour of the first column, so the gaps land on the wall clock. An
    // unsynced clock leaves it 0 and the groups fall back to plain sixes.
    int hour0 = m->now ? (int)(((m->now + m->utc_off_s) / 3600) % 24) : 0;

    int x = RAIN_X0;
    for (int i = 0; i < RAIN_HOURS; i++) {
        if (i && (hour0 + i) % RAIN_GROUP == 0) {
            x += RAIN_GAP;
        }
        int h = skip + i;

        int prob = m->out_ok && h < m->out.hour_count && m->out.hour_prob_pct[h] > 0
                       ? m->out.hour_prob_pct[h]
                       : 0;
        // Probability spans dim2..full, clear of the dim a dry hour -- and an
        // hour no forecast reaches -- is drawn at.
        int level = prob ? GFX_DIM2 + (GFX_FULL - GFX_DIM2) * prob / 100 : GFX_DIM;

        gfx_vline(c, x, cur->y, RAIN_H, (gfx_level_t)level, GFX_SOLID, 0);
        x += RAIN_DX;
    }

    ui_gap(cur, RAIN_H + UI_GAP);
}

/* One row per day: weekday, the day's low, a bar, the day's high. The bar's dim
 * body is the range of the whole forecast and is the same on every row, so the
 * lit segment — that day's low to high, mapped into it — can be compared across
 * rows by position alone.
 *
 * Columns: the weekday letter, either temperature, one digit of precipitation
 * and a one-pixel cloud column on the far right, the rest for the bar. Each temperature column is as wide as
 * the widest value it has to show — a single minus sign anywhere in it costs
 * every row a character — and the two are measured apart, so the bar gives up
 * only what is actually needed. */
#define FC_WD_W  3                    /* one 3x5im letter */
#define FC_WD_WEEKEND ((gfx_level_t)8)   /* the rest of the weekdays stay GFX_DIM */
#define FC_LEVEL_MIN 8                   /* 1% of rain; 0% is GFX_DIM */
#define FC_P_W   3
#define FC_C_W   1                        /* the cloud column is one pixel wide */
#define FC_C_X   UI_RX                    /* right edge of the cloud column */
#define FC_P_X   (FC_C_X - FC_C_W - 3)    /* right edge of the precipitation column */
#define FC_HI_X  (FC_P_X - FC_P_W - 3)    /* right edge of the high column */

static void wx_forecast(gfx_canvas_t *c, ui_cursor_t *cur, const ui_model_t *m)
{
    const weather_api_day_t *d = m->out.days;
    const int n = m->out_ok ? m->out.day_count : 0;

    float lo = 0, hi = 0;
    for (int i = 0; i < n; i++) {
        if (i == 0 || d[i].temp_min_c < lo) { lo = d[i].temp_min_c; }
        if (i == 0 || d[i].temp_max_c > hi) { hi = d[i].temp_max_c; }
    }
    float span = hi - lo;

    // Formatted up front: the column widths are a property of the whole block,
    // so every row has to be known before the first one can be placed.
    char lo_s[WEATHER_API_FORECAST_DAYS][8], hi_s[WEATHER_API_FORECAST_DAYS][8];
    int  lo_w = 0, hi_w = 0;
    for (int i = 0; i < WEATHER_API_FORECAST_DAYS; i++) {
        if (i < n) {
            snprintf(lo_s[i], sizeof(lo_s[i]), "%d", (int)lroundf(d[i].temp_min_c));
            snprintf(hi_s[i], sizeof(hi_s[i]), "%d", (int)lroundf(d[i].temp_max_c));
        } else {
            snprintf(lo_s[i], sizeof(lo_s[i]), "--");
            snprintf(hi_s[i], sizeof(hi_s[i]), "--");
        }
        int w = gfx_text_w(&UI_TINY, lo_s[i]);
        if (w > lo_w) { lo_w = w; }
        w = gfx_text_w(&UI_TINY, hi_s[i]);
        if (w > hi_w) { hi_w = w; }
    }

    const int lo_x  = FC_WD_W + 3 + lo_w;   /* right edge of the low column */
    const int bar_x = lo_x + 2;
    const int bar_w = FC_HI_X - hi_w - 2 - bar_x;

    // The block keeps its full height whether or not the fetch succeeded, so an
    // empty row is the day's columns dashed out and the bar left unlit.
    for (int i = 0; i < WEATHER_API_FORECAST_DAYS; i++) {
        int baseline = ui_row(cur, &UI_TINY);
        int top      = baseline - 4;   // the digits ink from here to the baseline
        bool ok      = i < n;

        // The date already carries the location's offset, so gmtime_r() gives
        // its calendar day; the column holds the first letter of the weekday.
        char wd[8]  = "-";
        int  wday   = -1;
        if (ok) {
            struct tm dt;
            gmtime_r(&d[i].date, &dt);
            strftime(wd, sizeof(wd), "%a", &dt);
            wd[1] = '\0';
            wday  = dt.tm_wday;
        }
        gfx_text_style_t wd_st = UI_TINY;
        wd_st.level = (wday == 0 || wday == 6) ? FC_WD_WEEKEND : GFX_DIM;
        gfx_text(c, 0, baseline, &wd_st, wd);
        gfx_px(c, FC_WD_W + 1, baseline - 3, GFX_DIM);   // parts the letter from the low

        gfx_text(c, lo_x,    baseline, &UI_TINY_R, lo_s[i]);
        gfx_text(c, FC_HI_X, baseline, &UI_TINY_R, hi_s[i]);

        // Probability in tens, one digit wide: 95% and up share the 9. It also
        // reads without the digit, off the brightness alone — a dry day stays
        // GFX_DIM and any chance at all jumps clear of it before ramping up.
        int p = ok ? d[i].precip_prob_pct : -1;
        gfx_text_style_t p_st = UI_TINY_R;
        p_st.level = GFX_DIM;
        if (p < 0) {
            gfx_text(c, FC_P_X, baseline, &p_st, "-");
        } else {
            if (p > 0) {
                p_st.level = (gfx_level_t)(FC_LEVEL_MIN
                                           + p * (GFX_FULL - FC_LEVEL_MIN) / 100);
            }
            int dig = (p + 5) / 10;
            gfx_textf(c, FC_P_X, baseline, &p_st, "%d", dig > 9 ? 9 : dig);
        }
        // Cloud cover as a one-pixel-wide column instead of a digit, centred on
        // the separator pixels' row: the clear sky sets both its height — 1, 3
        // or 5 px, a third of the scale each — and its brightness over 1..15, so
        // a sunny day is a tall bright column and an overcast one a single faint
        // pixel. No data is that pixel dimmed instead.
        int clear = ok && d[i].cloud_pct >= 0 ? 100 - d[i].cloud_pct : -1;
        if (clear < 0) {
            gfx_px(c, FC_C_X - 1, baseline - 3, GFX_DIM);
        } else {
            int h = clear >= 67 ? 5 : clear >= 34 ? 3 : 1;
            gfx_level_t lv = (gfx_level_t)(1 + clear * (GFX_FULL - 1) / 100);
            gfx_vline(c, FC_C_X - 1, baseline - 3 - h / 2, h, lv, 0xFF, 0);
        }
        gfx_px(c, FC_HI_X + 1, baseline - 3, GFX_DIM);   // tells the two numbers apart
        gfx_px(c, FC_P_X + 1,  baseline - 3, GFX_DIM);

        gfx_checker(c, (gfx_rect_t){ (int16_t)bar_x, (int16_t)top, (int16_t)bar_w, 3 },
                    GFX_DIM, GFX_NONE, 1);
        if (!ok) {
            continue;
        }

        int x0 = span > 0 ? (int)lroundf((d[i].temp_min_c - lo) / span * (bar_w - 1)) : 0;
        int x1 = span > 0 ? (int)lroundf((d[i].temp_max_c - lo) / span * (bar_w - 1))
                          : bar_w - 1;

        for (int x = x0 + (x0 & 1); x <= x1; x += 2) {
            gfx_vline(c, bar_x + x, top, 3, GFX_FULL, 0xFF, 0);
        }
    }
}

/* Daylight over the day: the bar's dim body is midnight to midnight and the lit
 * checker is sunrise to sunset, with a pointer over the hour it is now, the two
 * crossings beneath its ends, and between them the sun's elevation and the wait
 * for the next crossing, each in turn. All of it from sun.c by way of the
 * model — no forecast is involved. */
#define SUN_BAR_H   3
#define SUN_SWAP_MS 5000   /* how long each of the two centre readings holds */
#define DAY_S       86400

/* A UTC stamp to seconds since the location's local midnight. */
static int sun_local_s(time_t t, int32_t utc_off_s)
{
    long v = (long)((t + utc_off_s) % DAY_S);
    return (int)(v < 0 ? v + DAY_S : v);
}

/* A second of the day to its column on the bar. */
static int sun_x(int s)
{
    return s * (GFX_W - 1) / DAY_S;
}

/* Sunrise and sunset as seconds since local midnight, or false when the day has
 * no crossings — a polar day fills the bar, a polar night leaves it empty. */
static bool sun_span(const ui_model_t *m, int *rise_s, int *set_s)
{
    if (!m->sun.day_ok || m->sun.day.state != SUN_RISES) {
        return false;
    }
    *rise_s = sun_local_s(m->sun.day.rise, m->utc_off_s);
    *set_s  = sun_local_s(m->sun.day.set,  m->utc_off_s);
    return *rise_s < *set_s;   // a day running past midnight is not one span
}

/* One checkered stretch of the bar. gfx_checker anchors its first set square to
 * the rect, so a stretch starting on an even column would come out a phase off
 * the rest of the bar; swapping the roles of the two levels shifts it back
 * without moving the rect, which is what keeps the daylight edge on its second.
 */
static void sun_fill(gfx_canvas_t *c, int x, int w, int y, gfx_level_t lv)
{
    bool flip = (x & 1) == 0;
    gfx_checker(c, (gfx_rect_t){ (int16_t)x, (int16_t)y, (int16_t)w, SUN_BAR_H },
                flip ? GFX_NONE : lv, flip ? lv : GFX_NONE, 1);
}

/* The scale itself, drawn from `y` down. The pointer hangs 2 px above the bar,
 * so the caller owes it that much room. */
static void sun_scale(gfx_canvas_t *c, int y, const ui_model_t *m)
{
    int rise_s = 0, set_s = 0;
    bool span = sun_span(m, &rise_s, &set_s);
    if (!span && m->sun.day_ok && m->sun.day.state == SUN_POLAR_DAY) {
        rise_s = 0;
        set_s  = DAY_S - 1;
        span   = true;
    }

    sun_fill(c, 0, GFX_W, y, GFX_DIM);
    if (span) {
        int x0 = sun_x(rise_s);
        int x1 = sun_x(set_s);
        sun_fill(c, x0, x1 - x0 + 1, y, GFX_FULL);
    }

    // A solid tick every six hours, cleared either side, so the eye can count
    // quarters off the bar. Each takes the level of the stretch it falls in, or
    // it would read as daylight in the night and back.
    for (int q = DAY_S / 4; q < DAY_S; q += DAY_S / 4) {
        bool lit = span && q >= rise_s && q <= set_s;
        int  qx  = sun_x(q);
        gfx_vline(c, qx - 1, y, SUN_BAR_H, GFX_OFF, GFX_SOLID, 0);
        gfx_vline(c, qx + 1, y, SUN_BAR_H, GFX_OFF, GFX_SOLID, 0);
        gfx_vline(c, qx,     y, SUN_BAR_H, lit ? GFX_FULL : GFX_DIM, GFX_SOLID, 0);
    }

    // Where the sun is now: a pointer hanging over the bar, five columns for
    // three, narrowing by a pixel a side per row down to the apex on the bar's
    // top row. Each row is cleared two columns wider than it inks, or the fill
    // it stands in swallows the steps and the shape stops reading as a pointer;
    // below the apex one cleared row parts it from the rest of the bar.
    struct tm tm;
    if (!local_tm(m, &tm)) {
        return;
    }
    int x = sun_x(tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);

    for (int r = 0; r < 3; r++) {
        int half = 2 - r;
        gfx_hline(c, x - half - 2, y - 2 + r, 2 * half + 5, GFX_OFF, GFX_SOLID, 0);
        gfx_hline(c, x - half,     y - 2 + r, 2 * half + 1, GFX_FULL, GFX_SOLID, 0);
    }
    gfx_hline(c, x - 1, y + 1, 3, GFX_OFF, GFX_SOLID, 0);
}

/* H:MM in the tiny face, whose own colon costs a whole character cell and reads
 * heavy beside digits this small. The halves are drawn either side of a colon of
 * two single pixels instead. `pad` zero-fills the hour, for a clock time rather
 * than a span; a negative `s` is dashed out. Honours the style's alignment. */
#define HHMM_COLON_W 3

static void tiny_hhmm(gfx_canvas_t *c, int x, int baseline,
                      const gfx_text_style_t *st, int s, bool pad)
{
    char h[3], m[3];
    if (s < 0) {
        snprintf(h, sizeof(h), "--");
        snprintf(m, sizeof(m), "--");
    } else {
        snprintf(h, sizeof(h), pad ? "%02d" : "%d", s / 3600 % 24);
        snprintf(m, sizeof(m), "%02d", s / 60 % 60);
    }

    gfx_text_style_t ls = *st;
    ls.align = GFX_LEFT;

    int w = gfx_text_w(&ls, h) + HHMM_COLON_W + gfx_text_w(&ls, m);
    if (st->align == GFX_RIGHT) {
        x -= w;
    } else if (st->align == GFX_CENTER) {
        x -= w / 2;
    }

    int hw = gfx_text(c, x, baseline, &ls, h);
    gfx_px(c, x + hw + 1, baseline - 2, st->level);
    gfx_px(c, x + hw + 1, baseline - 4, st->level);
    gfx_text(c, x + hw + HHMM_COLON_W, baseline, &ls, m);
}

static void sun_bar(gfx_canvas_t *c, ui_cursor_t *cur, const ui_model_t *m)
{
    // The scale takes the first row; its pointer hangs 2 px above the bar and
    // borrows them from the gap the rule above left.
    sun_scale(c, cur->y + 1, m);
    ui_gap(cur, 1 + SUN_BAR_H + 3);

    int rise_s = -1, set_s = -1;
    sun_span(m, &rise_s, &set_s);

    gfx_text_style_t ts = UI_TINY;

    int baseline = ui_row(cur, &UI_TINY);
    tiny_hhmm(c, 0, baseline, &ts, rise_s, true);
    ts.align = GFX_RIGHT;
    tiny_hhmm(c, UI_RX, baseline, &ts, set_s, true);

    // Between the two times, under the stretch of bar it spans: the wait for the
    // next crossing, and every other SUN_SWAP_MS the sun's elevation. One slot
    // for both because the row has no width for two. The elevation is placed by
    // hand — the degree sign is a pixel run of its own, outside the text.
    ts.align = GFX_CENTER;
    if (m->anim_ms / SUN_SWAP_MS % 2) {
        char b[8];
        snprintf(b, sizeof(b), m->sun.elev_ok ? "%+d" : "--",
                 (int)lround(m->sun.elev_deg));
        int x  = (UI_RX - (gfx_text_w(&UI_TINY, b) + 1 + DEG_W)) / 2;
        int tw = gfx_text(c, x, baseline, &UI_TINY, b);
        degree(c, x + tw + 1, baseline, &UI_TINY);
    } else {
        tiny_hhmm(c, UI_RX / 2, baseline, &ts,
                  m->sun.next_ok && m->sun.next_s > 0 ? m->sun.next_s : -1, false);
    }
}

// Built up element by element: what is drawn here reads the model, the blocks
// still commented out are roughed-in layout waiting for theirs.
void screen_now(gfx_canvas_t *c, const ui_model_t *m, const ui_state_t *s)
{
    // Background fill

    gfx_clear(c, GFX_OFF);
    gfx_checker(c, (gfx_rect_t){ 0, 0, GFX_W, GFX_H }, (gfx_level_t)1, GFX_NONE, 1);

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
    if (m->ap) {
        ap_badge(c, UI_RX, baseline);
    } else {
        bars(c, UI_RX, baseline, sig_mask(m->link, m->rssi, m->anim_ms));
    }
    // battery(c, UI_RX - SIG_W - 3, baseline, 0);

    char age[8];
    age_str(age, sizeof(age), m->out.age_s);

    baseline = ui_row(&cur, &UI_TEXT);
    gfx_text_style_t age_st = UI_TEXT_R;
    age_st.level = GFX_DIM;
    // Measured either way: while the fetch runs the dots stand in the age's own
    // box, so the location beside them keeps the width it had.
    int aw = gfx_text_w(&age_st, age);
    if (m->out_fetching) {
        dots(c, GFX_W, baseline, aw, m->anim_ms);
    } else {
        gfx_text(c, GFX_W, baseline, &age_st, age);
    }
    gfx_push(c, (gfx_rect_t){ 0, 0, (int16_t)(UI_RX - aw - 2), GFX_H });
    gfx_text_style_t loc_st = UI_TEXT;
    // Dim ink would vanish into the plate, so a selected name goes full level.
    bool loc_sel = s->focus == UI_FOCUS_LOC;
    loc_st.level = loc_sel ? GFX_FULL : GFX_DIM;
    gfx_text_bg(c, 0, baseline, &loc_st, loc_sel ? GFX_HL : GFX_NONE,
                m->loc[0] ? m->loc : "--");
    gfx_pop(c);
    rule(c, &cur);

    // Conditions with Icon

    wx_now(c, &cur, m);
    rule(c, &cur);

    // Condition Labels

    bool ok = m->out_ok;

    baseline = ui_row(&cur, &UI_TEXT);
    reading(c, 0, baseline, &UI_TEXT, ok, "%.1f", "--.-", m->out.feels_c, "FL", true,
            GFX_NONE);
    reading(c, UI_RX, baseline, &UI_TEXT_R, ok, "%.1f", "---.-",
            m->out.pressure_msl_hpa, NULL, false, GFX_NONE);

    baseline = ui_row(&cur, &UI_TEXT);
    reading(c, 0, baseline, &UI_TEXT, ok, "%.0f%%", "--%", m->out.humidity_pct,
            NULL, false, GFX_NONE);
    // Speed and gusts as one range, in the units the API reports.
    if (ok) {
        gfx_textf(c, UI_RX, baseline, &UI_TEXT_R, "%s%.0f-%.0f",
                  weather_api_wind_dir_str(m->out.wind_dir_deg),
                  m->out.wind_kmh, m->out.gust_kmh);
    } else {
        gfx_text(c, UI_RX, baseline, &UI_TEXT_R, "---");
    }

    baseline = ui_row(&cur, &UI_TEXT);
    reading(c, 0, baseline, &UI_TEXT, ok && m->out.uvi >= 0, "%.1f", "--.-",
            m->out.uvi, "UV", false, GFX_NONE);
    reading(c, UI_RX, baseline, &UI_TEXT_R, ok, "%.0f%%", "--%",
            (double)m->out.cloud_pct, "CL", false, GFX_NONE);

    rule(c, &cur);

    // Rain, 24 h

    wx_rain(c, &cur, m);
    rule(c, &cur);

    // Forecast

    wx_forecast(c, &cur, m);
    rule(c, &cur);

    // Sensor Labels

    const climate_t *cl = &m->climate;

    baseline = ui_row(&cur, &UI_TEXT);
    reading(c, 0, baseline, &UI_TEXT, cl->temp_ok, "%.2f", "--.--", cl->temp_c,
            NULL, true, q_bg(s, HISTORY_Q_TEMP));
    reading(c, UI_RX, baseline, &UI_TEXT_R, cl->press_ok, "%.3f", "---.---",
            cl->press_msl_hpa, NULL, false, q_bg(s, HISTORY_Q_PRESS));

    baseline = ui_row(&cur, &UI_TEXT);
    reading(c, 0, baseline, &UI_TEXT, cl->rh_ok, "%.1f%%", "--.-%", cl->rh_pct,
            NULL, false, q_bg(s, HISTORY_Q_RH));
    reading(c, UI_RX, baseline, &UI_TEXT_R, cl->co2_ok, "%.0f", "---",
            (double)cl->co2_ppm, "CO2", false, q_bg(s, HISTORY_Q_CO2));

    baseline = ui_row(&cur, &UI_TEXT);
    char lx[8] = "--";
    if (cl->lux_ok) {
        ui_lux_str(lx, sizeof(lx), cl->lux);
    }
    value(c, 0, baseline, &UI_TEXT, lx, "Lx", false, q_bg(s, HISTORY_Q_LUX));
    reading(c, UI_RX, baseline, &UI_TEXT_R, cl->rh_ok, "%.1f", "--.-",
            cl->dew_c, "DW", true, GFX_NONE);

    rule(c, &cur);

    // Brightness, pinned to the bottom corner: it is a knob field and needs a
    // readout to be edited by. 3x5im has no descender, so the baseline is its
    // last row, and it clears the burn-in shift's reserve above the edge.
    int bw = gfx_textf_bg(c, UI_RX, GFX_H - 1 - GFX_SHIFT_MAX, &UI_TINY_R,
                          s->focus == UI_FOCUS_BRIGHT ? GFX_HL : GFX_NONE, "%u",
                          s->set.bright);

    // The burn-in offset the frame is currently drawn at, left of it: read-only,
    // so it stays off the plate the focus draws under the brightness.
    gfx_textf(c, UI_RX - bw - 2, GFX_H - 1 - GFX_SHIFT_MAX, &UI_TINY_R, "+%d",
              gfx_shift(c));

    // Chart

    chart_draw(c, &cur, m->chart, m->chart_n, s->set.chart_q,
               s->set.chart_range, s->focus == UI_FOCUS_CHART_RANGE);
    rule(c, &cur);

    // Zambretti: the 3 h tendency, then the wording it selects. The tendency is
    // a figure beside the words rather than a reading of its own, so it goes in
    // the tiny face; the wording keeps the text face and, since even the
    // shortened forms run past the row in the widest cases, scrolls back and
    // forth inside whatever the number left of it.

    baseline = ui_row(&cur, &UI_TEXT);
    if (!m->zb.ok) {
        // Nothing to word yet, so a single dash where the tendency goes rather
        // than a second one saying the same about the wording.
        gfx_text(c, 0, baseline, &UI_TINY, "--");
    } else {
        char zb[8];
        snprintf(zb, sizeof(zb), "%+.1f", m->zb.delta_3h_hpa);
        int zw = gfx_text(c, 0, baseline, &UI_TINY, zb) + 3;

        gfx_text_style_t z_st = UI_TEXT;
        z_st.level = GFX_DIM2;
        const char *zt = zambretti_code_short(m->zb.code);
        int off = scroll_off(m->anim_ms, gfx_text_w(&z_st, zt) - (UI_RX - zw));

        // Inside the viewport x counts from its own left edge, so the run starts
        // at the offset itself rather than at zw.
        gfx_push(c, (gfx_rect_t){ (int16_t)zw, 0, (int16_t)(UI_RX - zw), GFX_H });
        gfx_text(c, -off, baseline, &z_st, zt);
        gfx_pop(c);
    }
    rule(c, &cur);

    // Sun Position

    sun_bar(c, &cur, m);
    rule(c, &cur);


    /*
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
