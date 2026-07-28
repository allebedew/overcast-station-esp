#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Transport for the DFRobot Gravity I2C 16x2 LCD with RGB backlight (DFR0464).
 * Handles the two chips on the module — an AIP31068L character controller and
 * a PWM backlight driver — and nothing about what is worth showing; that is
 * screen_16x2.c. Named after the part on purpose: this display is a stopgap
 * until a larger one arrives, and only this file should have to go.
 *
 * Text is ASCII: the character ROM has no Cyrillic, so bytes outside
 * 0x20..0x7E are replaced with '?'.
 *
 * Not thread-safe — call from a single task. Sharing the bus with the sensors
 * is: every sequence that must not be interrupted is bracketed by the bus lock
 * from i2c_bus.h. */

#define LCD1602_COLS 16
#define LCD1602_ROWS 2

/* Attaches to the shared I2C bus (i2c_bus_init() must have run) and detects
 * the module. ESP_ERR_NOT_FOUND when it does not answer — the other calls stay
 * safe in that case and keep retrying in the background, so a display plugged
 * in later starts working on its own. */
esp_err_t lcd1602_rgb_init(void);

/* False while the module is absent or failing. */
bool lcd1602_rgb_present(void);

/* Writes one row (0-based), padded with spaces to the full width and
 * truncated past it. Repeating the current contents costs no bus traffic. */
esp_err_t lcd1602_rgb_set_line(int row, const char *text);

/* Backlight color, full range per channel. Missing on revisions whose
 * backlight chip was not recognised — returns ESP_ERR_NOT_SUPPORTED then. */
esp_err_t lcd1602_rgb_set_color(uint8_t r, uint8_t g, uint8_t b);

/* Blanks both rows. */
esp_err_t lcd1602_rgb_clear(void);
