#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Transport for the DFRobot Gravity I2C 16x2 LCD with RGB backlight (DFR0464):
 * the AIP31068L character controller and the PWM backlight driver, and nothing
 * about what is worth showing — that is screen_16x2.c.
 *
 * Text is ASCII; bytes outside 0x20..0x7E become '?', except the degree sign
 * ("\xDF") and the solid block ("\xFF") from the character ROM.
 *
 * Not thread-safe — call from a single task. Sharing the bus with the sensors
 * is: uninterruptible sequences are bracketed by the lock from i2c_bus.h. */

#define LCD1602_COLS 16
#define LCD1602_ROWS 2

/* Attaches to the shared I2C bus (after i2c_bus_init()) and detects the module.
 * ESP_ERR_NOT_FOUND when it does not answer; the other calls stay safe and keep
 * retrying, so a display plugged in later comes up on its own. */
esp_err_t lcd1602_rgb_init(void);

/* False while the module is absent or failing. */
bool lcd1602_rgb_present(void);

/* One row (0-based), padded to the full width and truncated past it.
 * Repeating the current contents costs no bus traffic. */
esp_err_t lcd1602_rgb_set_line(int row, const char *text);

/* Backlight colour; ESP_ERR_NOT_SUPPORTED on an unrecognised backlight chip. */
esp_err_t lcd1602_rgb_set_color(uint8_t r, uint8_t g, uint8_t b);

/* Blanks both rows. */
esp_err_t lcd1602_rgb_clear(void);
