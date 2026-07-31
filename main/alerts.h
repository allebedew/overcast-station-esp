#pragma once

/* Watches sensor readings and network state and sends Telegram messages when
 * thresholds are crossed. Thresholds are in alerts.c. */
void alerts_init(void);
