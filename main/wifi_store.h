#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Хранилище сохранённых Wi-Fi сетей (NVS, неймспейс "wifi_creds").
 * Все функции потокобезопасны. */

#define WIFI_MAX_NETWORKS 5

typedef struct {
    char ssid[33];
    char password[65];
} wifi_cred_t;

void wifi_store_init(void);

/* Добавляет сеть или обновляет пароль существующей. */
esp_err_t wifi_store_add(const char *ssid, const char *password);

esp_err_t wifi_store_remove(const char *ssid);

int wifi_store_count(void);

/* Копирует сеть по индексу; false, если индекс вне списка. */
bool wifi_store_get(int idx, wifi_cred_t *out);

/* Копирует только SSID сети по индексу. */
bool wifi_store_get_ssid(int idx, char ssid[33]);
