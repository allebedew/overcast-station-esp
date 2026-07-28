#pragma once

#include <stdint.h>

/* What the 16x2 display shows: page layouts and backlight. Owns the
 * lcd1602_rgb transport and is the only user of it. Four pages, refreshed at
 * 60 fps: indoor readings with the outdoor headline, the same air at the
 * dedicated sensors' full resolution, the full outdoor reading, and the clock
 * over the station's own vitals.
 *
 * Everything here is shaped by the two-rows-of-sixteen-characters budget —
 * hence the name. A larger display will want its own layout module rather
 * than a rewrite of this one. */

/* Starts the screen task. Requires the shared I2C bus, so call after
 * sensors_init(); the display may be absent, it is picked up whenever it
 * appears. */
void screen_16x2_init(void);

/* Shows the next page. Wired to a short press of the BOOT button; safe to
 * call from any task. */
void screen_16x2_next_page(void);

/* Backlight color as 0xRRGGBB. The device stores and applies it as-is —
 * hue, saturation and any other model belong to whoever sends the value (the
 * web UI). Persisted; takes effect on the next frame. */
void screen_16x2_set_backlight(uint32_t rgb);

uint32_t screen_16x2_backlight_rgb(void);
