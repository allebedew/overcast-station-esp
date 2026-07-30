#pragma once

#include <stdint.h>

/* Page layouts and backlight for the 16x2 display; owns the lcd1602_rgb
 * transport and is the only user of it. Everything here is shaped by the
 * two-rows-of-sixteen-characters budget — a larger display wants its own
 * layout module. */

/* Starts the screen task. Call after sensors_init(): the display shares the
 * I2C bus. An absent display is picked up whenever it appears. */
void screen_16x2_init(void);

/* Wired to a short press of the BOOT button; safe to call from any task. Does
 * nothing while the Wi-Fi or OTA page is up. */
void screen_16x2_next_page(void);

/* Backlight color as 0xRRGGBB, applied as-is — any hue or saturation model
 * belongs to the caller. Persisted; takes effect on the next frame. */
void screen_16x2_set_backlight(uint32_t rgb);

uint32_t screen_16x2_backlight_rgb(void);

/* What that color is scaled by for the ambient light, 0-255; 255 is the stored
 * color untouched and is also what an absent VEML7700 gives. */
uint8_t screen_16x2_backlight_scale(void);
