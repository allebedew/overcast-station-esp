#include <stdio.h>

#include "gfx_canvas.h"
#include "gfx_target.h"
#include "ui.h"

/* Everything the panel's brightness depends on, over a ramp of every gray level
 * -- what a drive setting does to that ramp is what cannot be judged any other
 * way. The values it opens on are the ones the transport was brought up with, so
 * a session starts from the panel as it runs and not from a guess.
 *
 * Kept past the tuning it was written for: the settings that came out of it suit
 * one panel in one room, and the next panel, or a move into direct sun, is a
 * reason to find them again. Nothing here is persisted -- what a session settles
 * on goes into gfx_target.h by hand.
 *
 * State and input are the screen's own, the shape every page will have once
 * there is a shell to hold them. Brightness appears both here and in
 * ui_state_t: this screen owns the panel outright while it is the one on it,
 * and only one of the two is reachable at a time. Reconciling them is the job
 * of the settings module, whenever it arrives. */

#define RAMP_H 8

typedef enum {
    PF_BRIGHT,     /* SSD1322 0xC7, master current */
    PF_CONTRAST,   /* 0xC1, the ceiling the above scales down from */
    PF_PHASE2,     /* 0xB1 high nibble: pre-charge length, DCLK */
    PF_PRECHG2,    /* 0xB6: second pre-charge period, DCLK */
    PF_VPRECHG,    /* 0xBB: pre-charge voltage */
    PF_COUNT,
} panel_field_t;

/* Ranges are the registers' own; the contrast step of 8 puts its 256 values at
 * 32 detents, near enough to the others to compare them by feel. */
static const struct {
    const char *name;
    uint8_t     min, max, step;
} FIELDS[PF_COUNT] = {
    [PF_BRIGHT]   = { "Bright",   0, GFX_BRIGHTNESS_MAX, 1 },
    [PF_CONTRAST] = { "Contrast", 0, 0xFF, 8 },
    [PF_PHASE2]   = { "Phase2",   3, 0x0F, 1 },
    [PF_PRECHG2]  = { "Prechg2",  1, 0x0F, 1 },
    [PF_VPRECHG]  = { "Vprechg",  0, 0x1F, 1 },
};

/* Initialised to what the init sequence ran, so no apply() is needed before the
 * first turn: the panel is already in this state. */
static uint8_t s_v[PF_COUNT] = {
    [PF_BRIGHT]   = GFX_BRIGHTNESS_DEFAULT,
    [PF_CONTRAST] = GFX_CONTRAST_DEFAULT,
    [PF_PHASE2]   = GFX_PHASE2_DEFAULT,
    [PF_PRECHG2]  = GFX_PRECHG2_DEFAULT,
    [PF_VPRECHG]  = GFX_VPRECHG_DEFAULT,
};

static uint8_t s_field;

static void apply(void)
{
    gfx_set_brightness(s_v[PF_BRIGHT]);
    gfx_set_contrast(s_v[PF_CONTRAST]);
    gfx_set_precharge(s_v[PF_PHASE2], s_v[PF_PRECHG2], s_v[PF_VPRECHG]);
}

ui_event_t screen_panel_input(const encoder_input_t *in)
{
    ui_event_t ev = UI_EV_NONE;

    if (in->click) {
        s_field = (uint8_t)((s_field + in->click) % PF_COUNT);
        ev = UI_EV_FIELD;
    }

    /* Turns with the button down count the same: the gesture has no job here. */
    int delta = in->cw + in->cw_held - in->ccw - in->ccw_held;
    if (delta != 0) {
        int next = s_v[s_field] + delta * FIELDS[s_field].step;
        if (next < FIELDS[s_field].min) { next = FIELDS[s_field].min; }
        if (next > FIELDS[s_field].max) { next = FIELDS[s_field].max; }

        ev = next == s_v[s_field] ? UI_EV_LIMIT : UI_EV_STEP;
        s_v[s_field] = (uint8_t)next;
        apply();
    }

    return ev;
}

void screen_panel_format(char *buf, int n)
{
    int at = 0;

    for (int f = 0; f < PF_COUNT && at < n; f++) {
        at += snprintf(buf + at, (size_t)(n - at), "%s%s %u", f ? ", " : "",
                       FIELDS[f].name, s_v[f]);
    }
}

static void field(gfx_canvas_t *c, ui_cursor_t *cur, int f)
{
    gfx_font_metrics_t m;
    gfx_font_metrics(UI_TEXT.font, &m);

    int baseline = ui_row(cur, &UI_TEXT);
    if (f == s_field) {
        gfx_rect(c, (gfx_rect_t){ 0, (int16_t)(baseline - m.ascent), GFX_W, m.line_height },
                 GFX_NONE, GFX_HL, GFX_SOLID);
    }
    gfx_text(c, 0, baseline, &UI_TEXT, FIELDS[f].name);
    gfx_textf(c, UI_RX, baseline, &UI_TEXT_R, "%d", s_v[f]);
}

void screen_panel(gfx_canvas_t *c)
{
    ui_cursor_t cur = { 2 };

    gfx_clear(c, GFX_OFF);

    gfx_text(c, GFX_W / 2, ui_row(&cur, &UI_TEXT_C), &UI_TEXT_C, "PANEL");
    ui_rule(c, &cur);

    for (int f = 0; f < PF_COUNT; f++) {
        field(c, &cur, f);
    }
    ui_gap(&cur, UI_GAP);
    ui_rule(c, &cur);

    for (int i = 0; i < 16; i++) {
        gfx_rect(c, (gfx_rect_t){ 0, cur.y, GFX_W - 12, RAMP_H },
                 GFX_NONE, (gfx_level_t)i, GFX_SOLID);
        gfx_textf(c, UI_RX, cur.y + RAMP_H - 2, &UI_TINY_R, "%d", i);
        ui_gap(&cur, RAMP_H + 1);
    }

    ui_gap(&cur, UI_GAP);
    ui_rule(c, &cur);

    gfx_text(c, 0, ui_row(&cur, &UI_TINY), &UI_TINY, "click: field");
    gfx_text(c, 0, ui_row(&cur, &UI_TINY), &UI_TINY, "turn: value");
}
