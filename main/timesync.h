#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Starts the SNTP client (clock in UTC). */
void timesync_init(void);

/* True once the clock has been set by SNTP at least once. */
bool timesync_is_synced(void);

/* Local time as "dd.mm.yyyy hh:mm:ss", or the same shape in dashes while the
 * clock has never been set. Needs 20 bytes. */
#define TIMESYNC_STR_LEN 20

void timesync_format(char *buf, size_t len);
