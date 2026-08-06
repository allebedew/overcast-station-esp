#pragma once

#include "gfx_canvas.h"
#include "ui_model.h"
#include "ui_state.h"

/* The main screen: the room's readings, the chart the knob selects and the
 * forecast under it. Drawn by ui_render(); nothing else calls it. */
void screen_now(gfx_canvas_t *c, const ui_model_t *m, const ui_state_t *s);
