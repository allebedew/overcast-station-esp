#pragma once

#include <stdint.h>

/* Page layouts and backlight for the 16x2 display; the only user of the
 * lcd1602_rgb transport. Everything here is shaped by the two-rows-of-sixteen
 * budget — a larger display wants its own layout module. */

/* Starts the screen task. Call after sensors_init(): the display shares the
 * I2C bus. An absent display is picked up whenever it appears. */
void screen_16x2_init(void);

/* Wired to a short press of the BOOT button; safe to call from any task. Does
 * nothing while the OTA page is up. */
void screen_16x2_next_page(void);

/* Backlight colour as 0xRRGGBB, applied as-is. Persisted; effective next
 * frame. */
void screen_16x2_set_backlight(uint32_t rgb);

uint32_t screen_16x2_backlight_rgb(void);

/* Ambient-light scale of that colour, 0-255; 255 also means no VEML7700. */
uint8_t screen_16x2_backlight_scale(void);
