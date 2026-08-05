#include "gui.h"

#include "gfx_canvas.h"
#include "gfx_target.h"
#include "ssd1322.h"
#include "ui.h"

static gfx_canvas_t s_canvas;

void gui_init(void)
{
    ssd1322_init();

    /* Experiment stage: one static frame, no task and no model behind it. */
    gfx_init(&s_canvas);
    screen_test(&s_canvas);
    gfx_present(&s_canvas);
}
