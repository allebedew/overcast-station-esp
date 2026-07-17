#pragma once

/* Notification rules: watches sensor readings and network state,
 * sends Telegram messages when thresholds are crossed. Thresholds
 * are defined in alerts.c. */
void alerts_init(void);
