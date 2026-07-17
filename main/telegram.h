#pragma once

#include <stdbool.h>

/* Starts the sender task. If the bot token/chat id are not configured
 * (see telegram.c), the module stays disabled and notify() is a no-op. */
void telegram_init(void);

/* Queues a printf-formatted message for delivery (non-blocking).
 * Messages wait in the queue until Wi-Fi is up; delivery is retried.
 * Returns false if the module is disabled or the queue is full. */
bool telegram_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
