#pragma once

#include <stdint.h>

/* Status LED (single WS2812). Its task polls the other modules every tick and
 * decides what to show — nothing pushes state in. Priorities are in led.c. */

/* Configures the LED and starts the indicator task. */
void led_init(void);

/* Marks network activity: the LED dips off for one tick. Any task. */
void led_notify_activity(void);

/* Brightness (1-255). Persisted in NVS and applied immediately. */
void led_set_brightness(uint8_t brightness);

uint8_t led_get_brightness(void);
