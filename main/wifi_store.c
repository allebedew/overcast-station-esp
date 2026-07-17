#include "wifi_store.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"

#define NVS_NAMESPACE "wifi_creds"
#define NVS_KEY_LIST  "list"

static const char *TAG = "wifi_store";

static wifi_cred_t s_networks[WIFI_MAX_NETWORKS];
static int s_count;
static SemaphoreHandle_t s_lock;

static void save(void)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
    if (s_count > 0) {
        ESP_ERROR_CHECK(nvs_set_blob(h, NVS_KEY_LIST, s_networks,
                                     s_count * sizeof(wifi_cred_t)));
    } else {
        nvs_erase_key(h, NVS_KEY_LIST);
    }
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}

void wifi_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t size = sizeof(s_networks);
        if (nvs_get_blob(h, NVS_KEY_LIST, s_networks, &size) == ESP_OK &&
            size % sizeof(wifi_cred_t) == 0) {
            s_count = size / sizeof(wifi_cred_t);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "%d saved network(s)", s_count);
}

esp_err_t wifi_store_add(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 ||
        (password && strlen(password) > 64)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i;
    for (i = 0; i < s_count && strcmp(s_networks[i].ssid, ssid) != 0; i++) {}
    if (i == s_count) {
        if (s_count == WIFI_MAX_NETWORKS) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_NO_MEM;
        }
        s_count++;
    }
    strlcpy(s_networks[i].ssid, ssid, sizeof(s_networks[i].ssid));
    strlcpy(s_networks[i].password, password ? password : "",
            sizeof(s_networks[i].password));
    save();
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Network \"%s\" saved (%d total)", ssid, s_count);
    return ESP_OK;
}

esp_err_t wifi_store_remove(const char *ssid)
{
    if (!ssid) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_networks[i].ssid, ssid) == 0) {
            memmove(&s_networks[i], &s_networks[i + 1],
                    (s_count - i - 1) * sizeof(wifi_cred_t));
            s_count--;
            save();
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "Network \"%s\" removed (%d left)", ssid, s_count);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

int wifi_store_count(void)
{
    return s_count;
}

bool wifi_store_get(int idx, wifi_cred_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (idx < 0 || idx >= s_count) {
        xSemaphoreGive(s_lock);
        return false;
    }
    *out = s_networks[idx];
    xSemaphoreGive(s_lock);
    return true;
}

bool wifi_store_get_ssid(int idx, char ssid[33])
{
    wifi_cred_t net;
    if (!wifi_store_get(idx, &net)) {
        return false;
    }
    strlcpy(ssid, net.ssid, 33);
    return true;
}
