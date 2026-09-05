#pragma once

#include <stdbool.h>

/* WireGuard tunnel to our own server -- the way in from outside the LAN. The
 * web server already listens on every interface, so it and OTA answer over the
 * tunnel as they do over Wi-Fi. Only the tunnel subnet is routed into it.
 *
 * Keys and endpoint come from wg_secrets.h, which is not in git; without that
 * file every function here is a no-op. */

typedef struct {
    bool configured;   /* wg_secrets.h was present at build time */
    bool up;           /* handshake completed, peer answering */
    char ip[16];       /* own address inside the tunnel */
    char endpoint[80]; /* "host:port" of the peer */
} wg_info_t;

/* Starts the supervisor task, which waits for Wi-Fi and the clock. */
void wg_init(void);

void wg_get_info(wg_info_t *out);
