// Host harness for the drawing framework: renders the scenes below to PNGs so
// fonts, alignment and primitives can be judged without the panel existing.
//
//   make && make run   ->  sim/out/*.png

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gfx_canvas.h"
#include "gfx_target.h"
#include "gfx_text.h"
#include "sim_png.h"
#include "ui.h"

// The host half of gfx_target.h: a PNG has no drive level. gfx_present() is not
// implemented here at all -- sim_write_png() takes the canvas directly.
void gfx_set_brightness(uint8_t level)
{
    (void)level;
}

void gfx_set_contrast(uint8_t contrast)
{
    (void)contrast;
}

void gfx_set_precharge(uint8_t phase2, uint8_t second, uint8_t voltage)
{
    (void)phase2;
    (void)second;
    (void)voltage;
}

static const uint8_t *F_TINY  = u8g2_font_4x6_tr;
static const uint8_t *F_TEXT  = u8g2_font_6x10_mr;
static const uint8_t *F_CAP   = u8g2_font_micro_tr;   // captions in the scenes themselves

// --- every gray level, to check the nibble packing and the ramp -------------

static void scene_levels(gfx_canvas_t *c)
{
    gfx_clear(c, GFX_OFF);

    const gfx_text_style_t lbl = { F_TINY, GFX_FULL, GFX_LEFT };
    gfx_text(c, 1, 6, &lbl, "LEVELS 0-15");

    for (int i = 0; i < 16; i++) {
        int y = 10 + i * 9;
        gfx_rect(c, (gfx_rect_t){ 1, y, 40, 8 }, GFX_NONE, (gfx_level_t)i, GFX_SOLID);
        gfx_textf(c, 62, y + 6, &(gfx_text_style_t){ F_TINY, GFX_FULL, GFX_RIGHT }, "%d", i);
    }

    // Two adjacent levels in one byte: the packing must not bleed across pixels.
    gfx_text(c, 1, 168, &lbl, "PACKING");
    for (int y = 0; y < 24; y++) {
        gfx_hline(c, 1, 172 + y, 62, (y & 1) ? GFX_FULL : (gfx_level_t)3, GFX_SOLID, 0);
    }

    gfx_text(c, 1, 210, &lbl, "DIM VS FULL");
    gfx_text(c, 1, 220, &(gfx_text_style_t){ F_TEXT, GFX_DIM, GFX_LEFT }, "PRESSURE");
    gfx_text(c, 1, 230, &(gfx_text_style_t){ F_TEXT, GFX_FULL, GFX_LEFT }, "1013 hPa");
}

// --- icon fonts: no letters, so they get a glyph grid instead of a specimen ---

static void utf8_encode(unsigned cp, char out[5])
{
    int n = 0;
    if (cp < 0x80) {
        out[n++] = (char)cp;
    } else if (cp < 0x800) {
        out[n++] = (char)(0xC0 | (cp >> 6));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[n++] = (char)(0xE0 | (cp >> 12));
        out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    }
    out[n] = '\0';
}

// Not a property of the data: open_iconic and unifont both carry letters too,
// so the intent has to come from the name.
static bool is_icon_font(const char *name)
{
    return strstr(name, "iconic") != NULL || strstr(name, "unifont") != NULL;
}

// gfx_font_table is in link order; specimens and the width table read far better
// ordered by cap height, which is what the eye compares when picking a font.
// Icon fonts have no capital A to measure, so they go last.
static int by_cap(const void *a, const void *b)
{
    const gfx_font_entry_t *ea = &gfx_font_table[*(const int *)a];
    const gfx_font_entry_t *eb = &gfx_font_table[*(const int *)b];

    int ia = is_icon_font(ea->name), ib = is_icon_font(eb->name);
    if (ia != ib) {
        return ia - ib;
    }

    gfx_font_metrics_t ma, mb;
    gfx_font_metrics(ea->font, &ma);
    gfx_font_metrics(eb->font, &mb);
    if (ma.cap != mb.cap) {
        return ma.cap - mb.cap;
    }
    return ma.line_height - mb.line_height;
}

static const gfx_font_entry_t *font_at(int i)
{
    static int  order[64];
    static bool sorted;

    if (!sorted) {
        assert(gfx_font_count <= (int)(sizeof(order) / sizeof(order[0])));
        for (int k = 0; k < gfx_font_count; k++) {
            order[k] = k;
        }
        qsort(order, (size_t)gfx_font_count, sizeof(order[0]), by_cap);
        sorted = true;
    }
    return &gfx_font_table[order[i]];
}

// u8g2 encodes glyphs in 16 bits, so the whole space can just be swept: asking
// for a width is the only way to find out what a font actually carries.
static int collect_glyphs(const uint8_t *font, unsigned *out, int max)
{
    const gfx_text_style_t st = { font, GFX_FULL, GFX_LEFT };
    int n = 0;
    for (unsigned cp = 0x20; cp <= 0xFFFF && n < max; cp++) {
        char s[5];
        utf8_encode(cp, s);
        if (gfx_text_w(&st, s) > 0) {
            out[n++] = cp;
        }
    }
    return n;
}

static int glyph_page(gfx_canvas_t *c, const uint8_t *font, const unsigned *cps, int count, int first)
{
    gfx_clear(c, GFX_OFF);

    gfx_font_metrics_t m;
    gfx_font_metrics(font, &m);

    const gfx_text_style_t st = { font, GFX_FULL, GFX_LEFT };
    // An icon font's ascent is derived from a letter it may not have, so the
    // cell comes from the bounding box instead.
    const int cell_w = m.bbx_w + 4;
    const int cell_h = m.bbx_h + 3;
    const int cols = GFX_W / cell_w;

    int i = first;
    for (int row = 0; (row + 1) * cell_h <= GFX_H && i < count; row++) {
        for (int col = 0; col < cols && i < count; col++, i++) {
            char s[5];
            utf8_encode(cps[i], s);
            // Clipped to its cell so an oversized glyph cannot scribble over
            // its neighbours.
            gfx_push(c, (gfx_rect_t){ (int16_t)(col * cell_w + 2), (int16_t)(row * cell_h + 1),
                                      (int16_t)m.bbx_w, (int16_t)m.bbx_h });
            gfx_text(c, 0, m.bbx_h - 2, &st, s);
            gfx_pop(c);
        }
    }
    return i;
}

// --- specimen of every linked font, paginated over as many canvases as needed -

// The strings the choice actually hinges on: the longest labels the station has
// and the values that sit next to them.
static const char *SAMPLES[] = { "Humidity 78%", "-3.2 1013 4.2" };

static int font_page(gfx_canvas_t *c, int first)
{
    gfx_clear(c, GFX_OFF);

    const gfx_text_style_t name = { u8g2_font_04b_03_tr, GFX_DIM, GFX_LEFT };
    gfx_font_metrics_t     mn;
    gfx_font_metrics(name.font, &mn);

    int top = 1, i = first;

    for (; i < gfx_font_count; i++) {
        const gfx_font_entry_t *e = font_at(i);
        if (is_icon_font(e->name)) {
            continue;   // rendered as a glyph grid instead
        }

        gfx_font_metrics_t m;
        gfx_font_metrics(e->font, &m);

        int block = mn.line_height + 2 * m.line_height + 5;
        if (top + block > GFX_H) {
            break;
        }

        gfx_text(c, 1, top + mn.ascent, &name, e->name);
        top += mn.line_height;
        for (size_t s = 0; s < sizeof(SAMPLES) / sizeof(SAMPLES[0]); s++) {
            const gfx_text_style_t body = { e->font, GFX_FULL, GFX_LEFT };
            gfx_text(c, 0, top + m.ascent, &body, SAMPLES[s]);
            top += m.line_height;
        }
        top += 5;
    }
    return i;
}

// --- label/value pairings, judged the way they will actually be read ---------

static void scene_pairs(gfx_canvas_t *c)
{
    gfx_clear(c, GFX_OFF);

    struct { const uint8_t *label, *value; const char *tag; } combos[] = {
        { u8g2_font_micro_tr,     u8g2_font_6x10_mr,        "micro + 6x10"      },
        { u8g2_font_4x6_tr,       u8g2_font_resoledbold_tr, "4x6 + resoled"     },
        { u8g2_font_3x5im_tr,     u8g2_font_micropixel_tr,  "3x5im + micropixel"},
        { u8g2_font_04b_03_tr,    u8g2_font_6x10_mr,        "04b03 + 6x10"      },
    };

    int top = 2;
    for (size_t i = 0; i < sizeof(combos) / sizeof(combos[0]); i++) {
        const gfx_text_style_t tag = { F_CAP, (gfx_level_t)4, GFX_LEFT };
        const gfx_text_style_t lbl = { combos[i].label, GFX_DIM, GFX_LEFT };
        const gfx_text_style_t val = { combos[i].value, GFX_FULL, GFX_LEFT };
        const gfx_text_style_t unit = { combos[i].label, GFX_DIM, GFX_LEFT };

        gfx_font_metrics_t ml, mv;
        gfx_font_metrics(combos[i].label, &ml);
        gfx_font_metrics(combos[i].value, &mv);

        gfx_text(c, 1, top + 5, &tag, combos[i].tag);
        top += 7;

        const char *rows[][2] = {
            { "TEMPERATURE", "-3.2" },
            { "HUMIDITY",    "78"   },
            { "PRESSURE",    "1013" },
        };
        for (size_t r = 0; r < 3; r++) {
            gfx_text(c, 1, top + ml.ascent, &lbl, rows[r][0]);
            int base = top + ml.ascent + 1 + mv.ascent;
            int x = 1 + gfx_text(c, 1, base, &val, rows[r][1]);
            gfx_text(c, x + 2, base, &unit, r == 0 ? "C" : (r == 1 ? "%" : "hPa"));
            top = base - mv.descent + 2;
        }
        top += 4;
    }
}

// --- alignment and metrics --------------------------------------------------

static void scene_align(gfx_canvas_t *c)
{
    gfx_clear(c, GFX_OFF);

    const gfx_text_style_t lbl = { F_TINY, GFX_DIM, GFX_LEFT };
    const int mid = GFX_W / 2;

    gfx_text(c, 1, 6, &lbl, "ALIGN");

    // The anchor is drawn as a dashed rule so it is obvious what x means.
    struct { gfx_halign_t a; int x; const char *tag; } cases[] = {
        { GFX_LEFT,   2,        "left"   },
        { GFX_CENTER, mid,      "center" },
        { GFX_RIGHT,  GFX_W - 2, "right"  },
    };

    int y = 16;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        gfx_vline(c, cases[i].x, y, 26, (gfx_level_t)4, 0x55, 0);
        gfx_text(c, 1, y + 6, &lbl, cases[i].tag);
        gfx_text(c, cases[i].x, y + 16, &(gfx_text_style_t){ F_TEXT, GFX_FULL, cases[i].a }, "12.3");
        gfx_text(c, cases[i].x, y + 25, &(gfx_text_style_t){ F_TEXT, GFX_FULL, cases[i].a }, "-3");
        y += 34;
    }

    // Baseline vs the box a caller would centre in, from the metrics.
    gfx_font_metrics_t m;
    gfx_font_metrics(F_TEXT, &m);

    gfx_text(c, 1, y + 6, &lbl, "METRICS");
    y += 12;

    gfx_rect(c, (gfx_rect_t){ 1, y, 62, 20 }, (gfx_level_t)5, GFX_NONE, GFX_SOLID);
    int baseline = y + (20 - (m.ascent - m.descent)) / 2 + m.ascent;
    gfx_hline(c, 2, baseline, 60, (gfx_level_t)3, 0x11, 0);
    gfx_text(c, mid, baseline, &(gfx_text_style_t){ F_TEXT, GFX_FULL, GFX_CENTER }, "Yj-3.2");

    // Mixed styles on one line, using the returned advance.
    y += 30;
    gfx_text(c, 1, y + 6, &lbl, "RUNS");
    y += 14;
    int x = 2;
    x += gfx_text(c, x, y, &(gfx_text_style_t){ F_TEXT, GFX_FULL, GFX_LEFT }, "21.4");
    gfx_text(c, x, y, &(gfx_text_style_t){ F_TINY, GFX_DIM, GFX_LEFT }, "°C");
}

// --- primitives, dashes, viewports -----------------------------------------

static void scene_primitives(gfx_canvas_t *c)
{
    gfx_clear(c, GFX_OFF);

    const gfx_text_style_t lbl = { F_TINY, GFX_DIM, GFX_LEFT };

    gfx_text(c, 1, 6, &lbl, "DASHES");
    uint8_t masks[] = { GFX_SOLID, 0x55, 0x33, 0x0F, 0x11, 0x07 };
    for (size_t i = 0; i < sizeof(masks); i++) {
        gfx_hline(c, 2, 10 + (int)i * 5, 60, GFX_FULL, masks[i], 0);
    }

    // Same mask, different phase: the two rules stay in step where it matters.
    gfx_text(c, 1, 48, &lbl, "PHASE");
    gfx_hline(c, 2, 52, 60, GFX_FULL, 0x0F, 0);
    gfx_hline(c, 2, 55, 60, GFX_FULL, 0x0F, 4);

    gfx_text(c, 1, 68, &lbl, "RECTS");
    gfx_rect(c, (gfx_rect_t){ 2, 72, 28, 20 }, GFX_FULL, GFX_NONE, GFX_SOLID);
    gfx_rect(c, (gfx_rect_t){ 34, 72, 28, 20 }, GFX_NONE, (gfx_level_t)5, GFX_SOLID);
    gfx_rect(c, (gfx_rect_t){ 2, 96, 28, 20 }, GFX_FULL, (gfx_level_t)4, GFX_SOLID);
    gfx_rect(c, (gfx_rect_t){ 34, 96, 28, 20 }, GFX_FULL, (gfx_level_t)3, 0x55);

    // A viewport clips its contents and shifts the origin: the string and the
    // rule below are drawn from 0,0 and must not escape the box.
    gfx_text(c, 1, 132, &lbl, "VIEWPORT");
    gfx_rect(c, (gfx_rect_t){ 2, 136, 40, 22 }, (gfx_level_t)4, GFX_NONE, 0x55);
    if (gfx_push(c, (gfx_rect_t){ 3, 137, 38, 20 })) {
        gfx_text(c, 0, 8, &(gfx_text_style_t){ F_TEXT, GFX_FULL, GFX_LEFT }, "CLIPPED!!");
        gfx_hline(c, -10, 12, 100, (gfx_level_t)8, GFX_SOLID, 0);
        gfx_rect(c, (gfx_rect_t){ 20, 14, 40, 40 }, GFX_FULL, GFX_NONE, GFX_SOLID);
    }
    gfx_pop(c);

    // Nested viewports, to prove clips intersect rather than replace.
    gfx_text(c, 1, 172, &lbl, "NESTED");
    gfx_rect(c, (gfx_rect_t){ 2, 176, 60, 30 }, (gfx_level_t)4, GFX_NONE, 0x55);
    gfx_push(c, (gfx_rect_t){ 3, 177, 58, 28 });
    gfx_push(c, (gfx_rect_t){ 10, 4, 20, 20 });
    gfx_rect(c, (gfx_rect_t){ -30, -30, 100, 100 }, GFX_NONE, (gfx_level_t)9, GFX_SOLID);
    gfx_pop(c);
    gfx_pop(c);

    // Single pixels at the four corners: catches an off-by-one in the packing.
    gfx_px(c, 0, 0, GFX_FULL);
    gfx_px(c, GFX_W - 1, 0, GFX_FULL);
    gfx_px(c, 0, GFX_H - 1, GFX_FULL);
    gfx_px(c, GFX_W - 1, GFX_H - 1, GFX_FULL);
}

// --- invariants that are cheap to get wrong and expensive to debug on glass ---

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static uint8_t level_at(const gfx_canvas_t *c, int x, int y)
{
    uint8_t b = c->buf[x][y >> 1];
    return (y & 1) ? (b & 0x0F) : (b >> 4);
}

static void selftest(void)
{
    static gfx_canvas_t c;

    // Nibble packing: even y in the high nibble, odd y in the low one, and the
    // two must not disturb each other.
    gfx_init(&c);
    gfx_px(&c, 0, 0, GFX_FULL);
    check(c.buf[0][0] == 0xF0, "even y goes to the high nibble");
    gfx_px(&c, 0, 1, (gfx_level_t)3);
    check(c.buf[0][0] == 0xF3, "odd y goes to the low nibble");

    // Every corner is reachable and nothing wraps around an edge.
    gfx_init(&c);
    gfx_px(&c, GFX_W - 1, GFX_H - 1, GFX_FULL);
    check(level_at(&c, GFX_W - 1, GFX_H - 1) == 15, "bottom-right corner");
    check(level_at(&c, 0, 0) == 0, "corner did not wrap");
    gfx_px(&c, -1, 0, GFX_FULL);
    gfx_px(&c, 0, GFX_H, GFX_FULL);
    check(level_at(&c, GFX_W - 1, 0) == 0, "negative x is dropped, not wrapped");

    // A viewport shifts the origin and clips; a child clip intersects the parent.
    gfx_init(&c);
    gfx_push(&c, (gfx_rect_t){ 10, 10, 4, 4 });
    gfx_px(&c, 0, 0, GFX_FULL);
    gfx_px(&c, 4, 0, GFX_FULL);           // one past the right edge
    gfx_pop(&c);
    check(level_at(&c, 10, 10) == 15, "viewport origin");
    check(level_at(&c, 14, 10) == 0, "viewport clips its own width");

    gfx_init(&c);
    gfx_push(&c, (gfx_rect_t){ 10, 10, 20, 20 });
    gfx_push(&c, (gfx_rect_t){ 0, 0, 40, 40 });   // wider than the parent
    gfx_hline(&c, 0, 0, 40, GFX_FULL, GFX_SOLID, 0);
    gfx_pop(&c);
    gfx_pop(&c);
    check(level_at(&c, 29, 10) == 15, "nested clip keeps the parent's edge");
    check(level_at(&c, 30, 10) == 0, "nested clip intersects, not replaces");

    // The dash pattern is anchored to local coordinates, so clipping a line must
    // not shift its dashes.
    gfx_init(&c);
    gfx_hline(&c, 0, 0, GFX_W, GFX_FULL, 0x55, 0);
    gfx_push(&c, (gfx_rect_t){ 0, 1, GFX_W, 1 });
    gfx_hline(&c, -8, 0, GFX_W + 8, GFX_FULL, 0x55, 0);
    gfx_pop(&c);
    bool same = true;
    for (int x = 0; x < GFX_W; x++) {
        same = same && level_at(&c, x, 0) == level_at(&c, x, 1);
    }
    check(same, "dash phase survives clipping");

    // Text: alignment is derived from the measured width.
    gfx_init(&c);
    const gfx_text_style_t st = { F_TEXT, GFX_FULL, GFX_RIGHT };
    int w = gfx_text_w(&st, "1013");
    check(w > 0, "text width is measured");
    check(gfx_text(&c, GFX_W, 10, &st, "1013") == w, "draw returns the advance");
}

// --- what actually decides the font: does the text fit in 64 px --------------

// --- the real screen, over a hand-filled model ---------------------------------

// No sensors on the host, so the snapshot is written out here. Values are the
// widest plausible ones -- a negative outdoor temperature, four-digit CO2 -- so
// the layout is judged at its worst, not at its prettiest.
// weather_api.c is firmware-only (HTTP, NVS); the screen needs just this one
// pure function out of it, so the host restates it.
const char *weather_api_wind_dir_str(int deg)
{
    static const char *const PTS[] = { "N", "E", "S", "W" };
    int d = ((deg % 360) + 360) % 360;
    return PTS[((d + 45) / 90) % 4];
}

// Rendered twice: once with every source answering, once with none of them --
// no clock, no link, no location, no fetch, no sensors. The empty frame is the
// one the station actually comes up with, and the layout has to survive a
// column of dashes as well as it survives the widest readings.
static void screen_now_scene(gfx_canvas_t *c, bool have_data)
{
    static ui_model_t m;
    memset(&m, 0, sizeof(m));

    m.out_cond  = "";   // ui_model_refresh() never leaves this NULL
    m.out.age_s = -1;

    // The knob's own state, which on the panel ui_state_init() sets: the chart
    // is drawn from what it has picked.
    ui_state_t st = { .bright = 0, .on = true, .focus = UI_FOCUS_CHART_Q,
                      .chart_q = HISTORY_Q_TEMP, .chart_range = CHART_RANGE_1M };

    if (!have_data) {
        ui_render(c, &m, &st);
        return;
    }

    m.now       = 1754200000;   // 2025-08-03 07:06 UTC
    m.utc_off_s = 3 * 3600;

    m.climate = (climate_t){
        .temp_ok = true,  .temp_c   = 21.4f,
        .rh_ok   = true,  .rh_pct   = 48.3f,  .dew_c = 10.2f,
        .co2_ok  = true,  .co2_ppm  = 1284,
        .press_ok = true, .press_msl_hpa = 1013.2f,
        .lux_ok  = true,  .lux      = 12345.0f,
    };

    m.out_ok   = true;
    m.out      = (weather_api_data_t){ .temp_c = 25.8f, .feels_c = -7.1f, .wind_kmh = 12.0f,
                                       .gust_kmh = 20.0f, .humidity_pct = 35.0f,
                                       .pressure_msl_hpa = 1018.0f, .cloud_pct = 100,
                                       .uvi = 5.25f, .wind_dir_deg = 265,
                                       .weather_code = 0 };
    m.out_cond = "Clear sky";   // the longest weather_api_code_short() returns
    m.out.age_s = 63 * 60;

    // Five days from m.now's local midnight, the same convention weather_api.c
    // stores: a GMT stamp already shifted by the location's offset. The spread
    // is wider than a real week so the bar has something to map, and the last
    // day's probability is missing to draw that column's dash.
    m.out.day_count = WEATHER_API_FORECAST_DAYS;
    static const struct { float lo, hi; int prob; } FC[] = {
        { -12.4f, 3.2f, 100 }, { -4.0f, 11.5f, 45 }, { 0.4f, 19.0f, 0 },
        {  8.1f, 27.4f,  20 }, { 24.0f, 34.2f, -1 },
    };
    time_t midnight = (m.now + m.utc_off_s) / 86400 * 86400;
    for (int i = 0; i < m.out.day_count; i++) {
        m.out.days[i] = (weather_api_day_t){
            .date            = midnight + i * 86400,
            .temp_min_c      = FC[i].lo,
            .temp_max_c      = FC[i].hi,
            .precip_prob_pct = FC[i].prob,
            .weather_code    = 0,
        };
    }

    // The chart's series, which on the panel comes out of the history rings:
    // a drifting wave, so both the shape and the axis labels have something to
    // show. One gap, to draw the break a dead sensor leaves.
    // A quiet room over a minute of 1 s samples: a tenth of a degree of drift,
    // well inside the mode's minimum span, so the scene shows the line centred
    // rather than the noise stretched over the whole box.
    m.chart_n = CHART_SERIES_MAX;
    for (int i = 0; i < m.chart_n; i++) {
        m.chart[i] = 25.5f + sinf(i * 0.2f) * 0.04f + i * 0.001f;
    }
    m.chart[m.chart_n / 3] = NAN;

    snprintf(m.loc, sizeof(m.loc), "Vozdvizhenka");   // a name long enough to crowd the age
    m.link = true;
    m.rssi = -68;

    st.bright = 12;   // the knob's own state, shown in the corner

    ui_render(c, &m, &st);
}

static void scene_screen_now(gfx_canvas_t *c)
{
    screen_now_scene(c, true);
}

static void scene_screen_now_empty(gfx_canvas_t *c)
{
    screen_now_scene(c, false);
}

static void measure(void)
{
    static const char *probes[] = {
        "TEMPERATURE", "FEELS LIKE", "HUMIDITY", "PRESSURE",
        "-3.2 C", "1013 hPa", "4.2 m/s",
    };

    printf("%-26s %3s %3s %3s %4s", "font", "cap", "asc", "dsc", "line");
    for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
        printf(" %11s", probes[p]);
    }
    printf("\n");

    for (int i = 0; i < gfx_font_count; i++) {
        const gfx_font_entry_t *e = font_at(i);
        gfx_font_metrics_t m;
        gfx_font_metrics(e->font, &m);
        printf("%-26s %3d %3d %3d %4d", e->name, m.cap, m.ascent, m.descent, m.line_height);

        for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
            const gfx_text_style_t st = { e->font, GFX_FULL, GFX_LEFT };
            int w = gfx_text_w(&st, probes[p]);
            // Zero width means the font has no glyphs for it at all; over 64 px
            // means the string cannot be shown on one line.
            if (w == 0) {
                printf(" %11s", "n/a");
            } else {
                printf(" %10d%c", w, w > GFX_W ? '!' : ' ');
            }
        }
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    static gfx_canvas_t canvas;
    const int scale = argc > 1 ? atoi(argv[1]) : 4;

    // So a re-run draws different random picks (the bottom animal) instead of
    // rand()'s fixed default sequence. The pid is in there because two runs
    // within the same second would otherwise get the same seed.
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    selftest();
    measure();
    const struct {
        const char *name;
        void (*fn)(gfx_canvas_t *);
    } scenes[] = {
        { "levels",     scene_levels },
        { "pairs",      scene_pairs },
        { "align",      scene_align },
        { "primitives", scene_primitives },
        { "screen_now",       scene_screen_now },
        { "screen_now_empty", scene_screen_now_empty },
    };

    char path[128];
    for (size_t i = 0; i < sizeof(scenes) / sizeof(scenes[0]); i++) {
        gfx_init(&canvas);
        scenes[i].fn(&canvas);

        snprintf(path, sizeof(path), "out/%s.png", scenes[i].name);
        if (!sim_write_png(path, &canvas, scale)) {
            fprintf(stderr, "cannot write %s\n", path);
            return 1;
        }
        printf("%s\n", path);
    }

    for (int first = 0, page = 1; first < gfx_font_count; page++) {
        gfx_init(&canvas);
        int next = font_page(&canvas, first);
        if (next == first) {
            break;   // a font taller than the panel would loop forever
        }
        first = next;

        snprintf(path, sizeof(path), "out/fonts%d.png", page);
        if (!sim_write_png(path, &canvas, scale)) {
            fprintf(stderr, "cannot write %s\n", path);
            return 1;
        }
        printf("%s\n", path);
    }

    // Icon fonts: what is in them is the whole question, so sweep and lay out
    // every glyph they carry.
    static unsigned cps[4096];
    for (int i = 0; i < gfx_font_count; i++) {
        const gfx_font_entry_t *e = font_at(i);
        if (!is_icon_font(e->name)) {
            continue;
        }
        int count = collect_glyphs(e->font, cps, (int)(sizeof(cps) / sizeof(cps[0])));
        printf("%-26s %d glyphs\n", e->name, count);

        for (int first = 0, page = 1; first < count; page++) {
            gfx_init(&canvas);
            int next = glyph_page(&canvas, e->font, cps, count, first);
            if (next == first) {
                break;
            }
            first = next;

            snprintf(path, sizeof(path), "out/icons_%s_%d.png", e->name, page);
            if (!sim_write_png(path, &canvas, scale)) {
                fprintf(stderr, "cannot write %s\n", path);
                return 1;
            }
            printf("%s\n", path);
        }
    }

    if (failures) {
        fprintf(stderr, "%d self-test failure(s)\n", failures);
        return 1;
    }
    return 0;
}
