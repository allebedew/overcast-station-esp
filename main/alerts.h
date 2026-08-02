#pragma once

/* Watches the CO2 level and the radar's presence flag and sends a Telegram
 * message on a confirmed change. Thresholds and confirmation windows are in
 * alerts.c. */
void alerts_init(void);
