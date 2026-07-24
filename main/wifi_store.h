#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Хранилище сохранённых Wi-Fi сетей (NVS, неймспейс "wifi_creds").
 * Все функции потокобезопасны. */

#define WIFI_MAX_NETWORKS 5

typedef struct {
    char ssid[33];
    char password[65];
    uint8_t bssid[6]; /* all-zero = no pin, connect to the strongest AP */
} wifi_cred_t;

void wifi_store_init(void);

/* Adds a network or updates the password/BSSID of an existing one.
 * bssid may be NULL or all-zero to leave the network un-pinned. */
esp_err_t wifi_store_add(const char *ssid, const char *password,
                         const uint8_t bssid[6]);

esp_err_t wifi_store_remove(const char *ssid);

int wifi_store_count(void);

/* Копирует сеть по индексу; false, если индекс вне списка. */
bool wifi_store_get(int idx, wifi_cred_t *out);

/* Копирует только SSID сети по индексу. */
bool wifi_store_get_ssid(int idx, char ssid[33]);
