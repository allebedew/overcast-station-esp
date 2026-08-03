#pragma once

#include <math.h>
#include <stdbool.h>

#include "gfx_canvas.h"

bool sim_write_png(const char *path, const gfx_canvas_t *c, int scale);
