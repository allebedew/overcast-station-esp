#pragma once

#include <stdint.h>

/* Status LED (single WS2812). A self-contained FreeRTOS task polls the other
 * modules (Wi-Fi, sensors, OTA) every tick and decides what to show — no one
 * pushes state into this module. See led.c for the priority of the states. */

/* Configures the LED and starts the indicator task. */
void led_init(void);

/* Marks network activity: while connected the LED dips off for one tick on
 * top of the CO2 color. Callable from any task. */
void led_notify_activity(void);

/* Brightness (1-255). Persisted in NVS and applied immediately. */
void led_set_brightness(uint8_t brightness);

uint8_t led_get_brightness(void);
