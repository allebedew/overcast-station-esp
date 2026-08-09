#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Starts the SNTP client (clock in UTC). Call right after the network is up:
 * the client is armed from the got-IP event, which a later call would miss. */
void timesync_init(void);

/* True once the clock is trusted: SNTP answered, or the RTC carried a plausible
 * time across a software reset. */
bool timesync_is_synced(void);

/* Local time as "dd.mm.yyyy hh:mm:ss", or the same shape in dashes while the
 * clock has never been set. Needs 20 bytes. */
#define TIMESYNC_STR_LEN 20

void timesync_format(char *buf, size_t len);
