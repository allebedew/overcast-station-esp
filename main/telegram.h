#pragma once

#include <stdbool.h>

/* Starts the sender task. Without a bot token/chat id (see telegram.c) the
 * module stays disabled and notify() is a no-op. */
void telegram_init(void);

/* Queues a printf-formatted message; non-blocking. Messages wait for Wi-Fi and
 * delivery is retried. False if the module is disabled or the queue is full. */
bool telegram_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
