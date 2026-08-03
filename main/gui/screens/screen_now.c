#include <ctype.h>
#include <stdio.h>
#include <string.h>

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

void screen_now(gfx_canvas_t *c, const ui_model_t *m)
{
    gfx_clear(c, GFX_OFF);

    ui_cursor_t cur = { 0 };

    char clock[8] = "--:--";
    if (m->now != 0) {
        time_t    local = m->now + m->utc_off_s;
        struct tm tm;
        gmtime_r(&local, &tm);
        snprintf(clock, sizeof(clock), "%02d:%02d", tm.tm_hour, tm.tm_min);
    }

    int baseline = ui_row(&cur, &UI_TEXT);
    gfx_text(c, 0, baseline, &UI_TEXT, clock);

    // Measured from the digits, not from the row: nothing in this font reaches
    // the baseline row itself, and the descenders the row reserves for cannot
    // occur in a clock.
    cur.y = (int16_t)(baseline + UI_GAP);
    ui_rule(c, &cur);

    // Outside: the icon on the left edge, the temperature and the conditions
    // stacked to its right. The icon is the tallest of the three and sets the
    // height of the whole block.
    char temp[8] = "--.-";
    if (m->out_ok) {
        snprintf(temp, sizeof(temp), "%.1f", (double)m->out.temp_c);
    }
    char cond[16] = "";
    if (m->out_ok && m->out_cond) {
        for (size_t i = 0; i < sizeof(cond) - 1 && m->out_cond[i]; i++) {
            cond[i] = (char)toupper((unsigned char)m->out_cond[i]);
        }
    }

    gfx_font_metrics_t fm, cm;
    gfx_font_metrics(UI_BOLD.font, &fm);
    gfx_font_metrics(UI_TEXT.font, &cm);

    const uint8_t *icon_font = NULL;
    unsigned       icon_cp   = 0;
    int            icon_dy   = 0;
    bool icon = m->out_ok && wx_icon(m->out.weather_code, &icon_font, &icon_cp, &icon_dy);

    gfx_text_style_t is = { icon_font, GFX_FULL, GFX_LEFT };
    int iw  = icon ? gfx_glyph_w(&is, icon_cp) : 0;
    int top = cur.y;
    int x   = icon ? iw + UI_GAP : 0;

    if (icon) {
        gfx_glyph(c, 0, top + WX_ICON_H + icon_dy, &is, icon_cp);
    }

    baseline = top + fm.ascent;
    int tw = gfx_text(c, x, baseline, &UI_BOLD, temp);
    if (m->out_ok) {
        ui_degree(c, x + tw + 1, top);
    }
    gfx_text(c, x, baseline + UI_GAP + cm.ascent, &UI_TEXT, cond);

    ui_gap(&cur, WX_ICON_H + UI_GAP);
    ui_rule(c, &cur);
}
