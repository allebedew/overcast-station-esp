#pragma once

/* Status LED (single WS2812). Its task polls the other modules every tick and
 * decides what to show — nothing pushes state in. Priorities are in led.c. */

/* Configures the LED and starts the indicator task. */
void led_init(void);

/* Marks network activity: the LED dips off for one tick. Any task. */
void led_notify_activity(void);

/* Brightness is SETTING_LED_BRIGHTNESS, read and written there; this module
 * only follows it. */
