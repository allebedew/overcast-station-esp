#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Station (client) side only; the access point is an independent axis tracked
 * in wifi_info_t. */
typedef enum {
    WIFI_STA_IDLE,          /* not trying to connect (e.g. AP is up) */
    WIFI_STA_CONNECTING,    /* association attempt in progress */
    WIFI_STA_WAITING_RETRY, /* pause before the next attempt */
    WIFI_STA_CONNECTED,     /* associated, IP obtained */
} wifi_sta_state_t;

/* Maximum transmit power, in the steps esp_wifi_set_max_tx_power() actually
 * has — it rounds anything between them down. What lowering it buys is a
 * smaller current peak on transmit, which is what a tired cell browns out on;
 * below 11 dBm the retransmits cost more bursts than the smaller peak saves.
 * Stored as an index in NVS, so the order is a persisted contract: append
 * only. */
typedef enum {
    WIFI_TX_20DBM,
    WIFI_TX_18DBM,
    WIFI_TX_16DBM,
    WIFI_TX_15DBM,
    WIFI_TX_14DBM,
    WIFI_TX_13DBM,
    WIFI_TX_11DBM,
    WIFI_TX_POWER_COUNT,
} wifi_tx_power_t;

/* Indexed by the enum: the vocabulary wifi_tx_power is spelled in on the API. */
extern const char *const WIFI_TX_POWER_NAMES[WIFI_TX_POWER_COUNT];

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

/* Brings up the radio and returns immediately. Saved networks are tried
 * round-robin; once exhausted, the device brings up its own access point. */
esp_err_t wifi_connect(void);

/* Turns the access point on or off. Turning it on stops the station attempts;
 * turning it off returns to station mode and restarts the round-robin. */
void wifi_ap_enable(bool on);

/* Re-reads the saved networks, restarts the round-robin and drops the AP. */
void wifi_reconnect(void);

/* Stops the radio outright, or brings it back and restarts the round-robin.
 * For power_save.c; everything that talks over the link already gates on
 * wifi_is_connected(), which stays false while it is off. */
void wifi_radio_enable(bool on);

/* Fills out with a consistent snapshot of both interfaces. */
void wifi_get_info(wifi_info_t *out);

/* Shorthand for the common case: station associated and holding an IP. */
bool wifi_is_connected(void);

/* Station side alone; ssid gets the current attempt or link ("" before the
 * first). Asks the radio nothing, so the display can call it every frame. */
wifi_sta_state_t wifi_sta_state(char *ssid, size_t len);

/* Blocking air scan (~2-3 s). Returns the number of APs found — one entry per
 * BSSID, so an SSID may repeat — or -1 on error. */
int wifi_scan(wifi_scan_ap_t *out, int max_count);

/* Name of a wifi_scan_ap_t::authmode value. Never NULL. */
const char *wifi_authmode_str(int authmode);

/* UI label for the link's 802.11 generation ("Wi-Fi 6 (802.11ax)"); the lowest
 * generation while there is no link. Never NULL. */
const char *wifi_sta_phy_str(void);
