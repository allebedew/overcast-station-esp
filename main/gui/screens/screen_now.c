#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_canvas.h"
#include "ui.h"

/* The main screen, built up one element at a time. */

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
    ui_degree(c, x + tw + 1, baseline, &UI_BOLD);
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

    float v[UI_CHART_MAX];
    for (int i = 0; i < UI_CHART_MAX; i++) {
        v[i] = sinf(i * 0.2f) * 3.0f + i * 0.05f;
    }

    int baseline = ui_row(cur, &UI_TEXT);
    gfx_text_bg(c, 0,  baseline, &UI_TEXT, GFX_HL, "Press");
    gfx_text_bg(c, UI_RX, baseline, &UI_TEXT_R, GFX_HL, "5m");

    float lo, hi;
    ui_chart(c, cur, v, UI_CHART_MAX, &lo, &hi);

    baseline = ui_row(cur, &UI_TINY);
    if (isfinite(lo)) {
        gfx_textf(c, UI_CHART_X, baseline, &UI_TINY, "%.1f", lo);
    }
    if (isfinite(hi)) {
        gfx_textf(c, UI_RX - UI_CHART_X, baseline, &UI_TINY_R, "%.1f", hi);
    }
}

// на этом экране пока накидывает текст и графику без привязки к модели
// чтобы посмотреть как это будет выглядеть
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
    ui_signal(c, UI_RX, baseline, 2);
    ui_battery(c, UI_RX - UI_SIGNAL_W - 3, baseline, 0);

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
    ui_degree(c, fl + 1, baseline, &UI_TEXT);
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
    ui_degree(c, rt + 1, baseline, &UI_TEXT);
    gfx_text(c, UI_RX, baseline, &UI_TEXT_R, "1017.744");

    baseline = ui_row(&cur, &UI_TEXT);
    gfx_text(c, 0,     baseline, &UI_TEXT,   "69.9%");
    gfx_text(c, UI_RX, baseline, &UI_TEXT_R, "CO2 1264");

    baseline = ui_row(&cur, &UI_TEXT);
    gfx_text(c, 0,     baseline, &UI_TEXT,   "Lx 183");
    gfx_text(c, UI_RX - UI_DEG_W - 1, baseline, &UI_TEXT_R, "DW 14.4");
    ui_degree(c, UI_RX - UI_DEG_W, baseline, &UI_TEXT);

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
