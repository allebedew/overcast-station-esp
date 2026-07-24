#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "wifi_store.h"

#define WIFI_RETRIES_PER_NETWORK 5
#define WIFI_RETRY_DELAY_MS      5000

#define WIFI_AP_SSID     "WeatherStation"
#define WIFI_AP_PASSWORD "weather123"
#define WIFI_AP_MAX_CONN 4

static const char *TAG = "wifi";

static volatile wifi_status_t s_status = WIFI_STATUS_DISCONNECTED;
static wifi_status_cb_t s_status_cb;
static int s_attempt;
static char s_current_ssid[33];
static esp_timer_handle_t s_retry_timer;

void wifi_set_status_callback(wifi_status_cb_t cb)
{
    s_status_cb = cb;
}

wifi_status_t wifi_get_status(void)
{
    return s_status;
}

int wifi_get_ap_client_count(void)
{
    if (s_status != WIFI_STATUS_AP_MODE) {
        return 0;
    }
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) {
        return 0;
    }
    return sta_list.num;
}

const char *wifi_get_ssid(void)
{
    return s_status == WIFI_STATUS_AP_MODE ? WIFI_AP_SSID : s_current_ssid;
}

const char *wifi_get_ap_ssid(void)
{
    return WIFI_AP_SSID;
}

int wifi_get_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (s_status != WIFI_STATUS_CONNECTED ||
        esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return 0;
    }
    return ap_info.rssi;
}

int wifi_get_channel(void)
{
    uint8_t primary = 0;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&primary, &second) != ESP_OK) {
        return 0;
    }
    return primary;
}

bool wifi_get_bssid(char *out, size_t len)
{
    wifi_ap_record_t ap_info;
    if (s_status != WIFI_STATUS_CONNECTED ||
        esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return false;
    }
    snprintf(out, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
             ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5]);
    return true;
}

int wifi_scan(wifi_scan_ap_t *out, int max_count)
{
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        return -1;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) {
        return 0;
    }
    wifi_ap_record_t *recs = malloc(num * sizeof(wifi_ap_record_t));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return -1;
    }
    esp_wifi_scan_get_ap_records(&num, recs);

    int n = 0;
    for (int i = 0; i < num && n < max_count; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) {
            continue; /* skip hidden networks */
        }
        /* One entry per BSSID (no SSID de-dup): a managed network broadcasts
         * the same SSID from many APs, and we want each AP visible. */
        strlcpy(out[n].ssid, ssid, sizeof(out[n].ssid));
        memcpy(out[n].bssid, recs[i].bssid, sizeof(out[n].bssid));
        out[n].rssi = recs[i].rssi;
        out[n].channel = recs[i].primary;
        out[n].authmode = recs[i].authmode;
        n++;
    }
    free(recs);
    return n;
}

/* --- подключение --- */

static void set_status(wifi_status_t status)
{
    if (s_status == status) {
        return;
    }
    s_status = status;
    if (s_status_cb) {
        s_status_cb(status);
    }
}

static void schedule_retry(void)
{
    set_status(WIFI_STATUS_WAITING_RETRY);
    esp_timer_stop(s_retry_timer); /* мог быть уже взведён */
    if (esp_timer_start_once(s_retry_timer, WIFI_RETRY_DELAY_MS * 1000ULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to arm retry timer");
    }
}

/* Пытается подключиться; если драйвер занят (например, идёт скан) —
 * планирует новую попытку вместо зависания автомата. */
static void try_connect(void)
{
    set_status(WIFI_STATUS_CONNECTING);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s, will retry", esp_err_to_name(err));
        schedule_retry();
    }
}

/* Записывает в драйвер конфигурацию сети для текущей попытки. */
static void apply_current_network(void)
{
    int count = wifi_store_count();
    wifi_cred_t net;
    if (count == 0 || !wifi_store_get(s_attempt % count, &net)) {
        return; /* подключаться не к чему */
    }

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, net.ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, net.password, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = net.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    /* Scan every channel and pick the strongest BSSID for this SSID instead of
     * the first match: on a managed network the same SSID comes from many APs,
     * and the default fast scan would latch onto a weaker/farther one. */
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    /* If the network is pinned to a BSSID, connect only to that AP (the
     * all-channel scan above still finds it on whatever channel it uses);
     * an all-zero BSSID means "unpinned" and keeps the by-signal choice. */
    for (int i = 0; i < 6; i++) {
        if (net.bssid[i]) {
            cfg.sta.bssid_set = true;
            memcpy(cfg.sta.bssid, net.bssid, sizeof(cfg.sta.bssid));
            break;
        }
    }
    strlcpy(s_current_ssid, net.ssid, sizeof(s_current_ssid));

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &cfg));
}

void wifi_start_ap(void)
{
    esp_timer_stop(s_retry_timer); /* мог быть взведён — не страшно, если нет */

    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .password = WIFI_AP_PASSWORD,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    set_status(WIFI_STATUS_AP_MODE);
    /* APSTA, а не AP: STA-интерфейс нужен для сканирования эфира */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    ESP_LOGI(TAG, "AP mode: SSID \"%s\", http://192.168.4.1/", WIFI_AP_SSID);
}

void wifi_reconnect(void)
{
    if (wifi_store_count() == 0) {
        ESP_LOGW(TAG, "No saved networks, staying in AP mode");
        wifi_start_ap();
        return;
    }
    s_attempt = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    apply_current_network();
    ESP_LOGI(TAG, "Reconnecting to \"%s\"...", s_current_ssid);
    if (s_status == WIFI_STATUS_CONNECTED) {
        /* Already associated (e.g. the user saved a new BSSID for the same
         * SSID): esp_wifi_connect() would be a no-op and the state machine
         * would hang in CONNECTING forever. Drop the link instead — the
         * STA_DISCONNECTED event then drives the reconnect with the new
         * config. */
        set_status(WIFI_STATUS_CONNECTING);
        esp_wifi_disconnect();
    } else {
        /* при выходе из APSTA событие STA_START не приходит — подключаемся сами */
        try_connect();
    }
}

static void retry_timer_cb(void *arg)
{
    apply_current_network();
    try_connect();
}

static const char *disconnect_reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:            return "AP not found";
    case WIFI_REASON_AUTH_FAIL:              return "auth failed";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "handshake timeout (wrong password?)";
    case WIFI_REASON_BEACON_TIMEOUT:         return "beacon timeout";
    case WIFI_REASON_ASSOC_LEAVE:            return "leaving";
    case WIFI_REASON_AUTH_EXPIRE:            return "auth expired";
    default:                                 return "other";
    }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_status != WIFI_STATUS_AP_MODE) {
            try_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = event_data;
        ESP_LOGW(TAG, "Disconnected from \"%s\", reason %d (%s)",
                 s_current_ssid, event->reason, disconnect_reason_str(event->reason));
        if (s_status == WIFI_STATUS_AP_MODE) {
            return; /* отключение из-за смены режима на AP — не переподключаемся */
        }
        int nets = wifi_store_count();
        if (nets == 0) {
            nets = 1;
        }
        s_attempt++;
        if (s_attempt < WIFI_RETRIES_PER_NETWORK * nets) {
            ESP_LOGI(TAG, "Retrying in %d ms (attempt %d/%d)",
                     WIFI_RETRY_DELAY_MS, s_attempt + 1, WIFI_RETRIES_PER_NETWORK * nets);
            schedule_retry();
        } else {
            ESP_LOGE(TAG, "All networks failed, starting AP");
            wifi_start_ap();
        }
    } else if (event_base == WIFI_EVENT && (event_id == WIFI_EVENT_AP_STACONNECTED ||
                                            event_id == WIFI_EVENT_AP_STADISCONNECTED)) {
        ESP_LOGI(TAG, "AP: client %s (%d total)",
                 event_id == WIFI_EVENT_AP_STACONNECTED ? "connected" : "disconnected",
                 wifi_get_ap_client_count());
        if (s_status_cb) {
            s_status_cb(s_status); /* статус тот же, но подписчик перечитает счётчик */
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_attempt = 0;
        set_status(WIFI_STATUS_CONNECTED);
    }
}

esp_err_t wifi_connect(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_store_init();

    const esp_timer_create_args_t retry_timer_args = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_timer_args, &s_retry_timer));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, NULL));

    if (wifi_store_count() == 0) {
        ESP_LOGW(TAG, "No saved networks, starting AP for provisioning");
        wifi_start_ap();
        ESP_ERROR_CHECK(esp_wifi_start());
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    apply_current_network();

    set_status(WIFI_STATUS_CONNECTING);
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to \"%s\"...", s_current_ssid);
    return ESP_OK;
}
