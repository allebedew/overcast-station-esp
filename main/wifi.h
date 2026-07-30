#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* State of the station (client) side only. The access point is tracked
 * separately in wifi_info_t: the two are independent axes. */
typedef enum {
    WIFI_STA_IDLE,          /* not trying to connect (e.g. AP is up) */
    WIFI_STA_CONNECTING,    /* association attempt in progress */
    WIFI_STA_WAITING_RETRY, /* pause before the next attempt */
    WIFI_STA_CONNECTED,     /* associated, IP obtained */
} wifi_sta_state_t;

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    int rssi;
    int channel;  /* primary channel */
    int authmode; /* wifi_auth_mode_t */
} wifi_scan_ap_t;

/* Snapshot of both interfaces, taken as a whole. */
typedef struct {
    wifi_sta_state_t sta_state;
    char    sta_ssid[33];  /* network of the current attempt or link */
    uint8_t sta_bssid[6];  /* all-zero unless connected */
    int     rssi;          /* dBm; 0 unless connected */
    int     channel;       /* radio channel in any mode; 0 if radio is down */
    bool    ap_active;
    char    ap_ssid[33];   /* own SoftAP SSID (constant) */
    int     ap_clients;    /* clients on our SoftAP; 0 when it is down */
} wifi_info_t;

/* Brings up the radio and starts connecting (STA), returning immediately.
 * Saved networks are tried round-robin; once the attempts are exhausted the
 * device brings up its own access point. */
esp_err_t wifi_connect(void);

/* Turns the access point on or off. Turning it on stops the station attempts;
 * turning it off returns to station mode and restarts the round-robin. */
void wifi_ap_enable(bool on);

/* Re-reads the saved networks and restarts the round-robin from the first one.
 * Also drops the access point if it was up. */
void wifi_reconnect(void);

/* Fills out with a consistent snapshot of both interfaces. */
void wifi_get_info(wifi_info_t *out);

/* Shorthand for the common case: station associated and holding an IP. */
bool wifi_is_connected(void);

/* The station side alone: the state, and into ssid the network of the current
 * attempt or link ("" before the first one). Unlike wifi_get_info() this asks
 * the radio nothing, so the display can call it every frame. */
wifi_sta_state_t wifi_sta_state(char *ssid, size_t len);

/* Blocking air scan (~2-3 s). Returns the number of APs found (one entry
 * per BSSID, so the same SSID may repeat across access points) or -1 on
 * error. */
int wifi_scan(wifi_scan_ap_t *out, int max_count);

/* Name of an authentication mode ("WPA2", "WPA2/WPA3", ...) for the value
 * carried in wifi_scan_ap_t::authmode. Never NULL; "?" for anything
 * unrecognised. */
const char *wifi_authmode_str(int authmode);

/* The 802.11 generation of the current station link, as a label for the UI
 * ("Wi-Fi 6 (802.11ax)"). Reports the lowest generation while there is no
 * link. Never NULL. */
const char *wifi_sta_phy_str(void);
