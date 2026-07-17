#pragma once

#include <stdbool.h>

/* Starts the SNTP client (clock in UTC). */
void timesync_init(void);

/* True once the clock has been set by SNTP at least once. */
bool timesync_is_synced(void);
