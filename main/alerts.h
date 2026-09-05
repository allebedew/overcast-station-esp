#pragma once

/* Watches the CO2 level and the PIR's presence flag and sends a Telegram
 * message on a change. Thresholds are in alerts.c. */
void alerts_init(void);
