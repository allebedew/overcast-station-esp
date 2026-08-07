#pragma once

#include "gfx_canvas.h"

#include <stddef.h>

/* The firmware update screen: the gui task draws this instead of the main one
 * while an image is being received, and nothing else is on the panel. */
void screen_ota(gfx_canvas_t *c, size_t received, size_t total);
