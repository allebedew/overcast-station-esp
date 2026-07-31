#include "wifi.h"

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

static volatile wifi_sta_state_t s_sta_state = WIFI_STA_IDLE;
static volatile bool s_ap_active;
static int s_attempt;
static char s_current_ssid[33];
static esp_timer_handle_t s_retry_timer;

bool wifi_is_connected(void)
{
    return s_sta_state == WIFI_STA_CONNECTED;
}

wifi_sta_state_t wifi_sta_state(char *ssid, size_t len)
{
    /* Read without a lock: a torn read costs one frame of a wrong name on the
     * display, which is cheaper than locking every frame. */
    strlcpy(ssid, s_current_ssid, len);
    return s_sta_state;
}

static int ap_client_count(void)
{
    wifi_sta_list_t sta_list;
    if (!s_ap_active || esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) {
        return 0;
    }
    return sta_list.num;
}

void wifi_get_info(wifi_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->sta_state = s_sta_state;
    out->ap_active = s_ap_active;
    strlcpy(out->sta_ssid, s_current_ssid, sizeof(out->sta_ssid));
    strlcpy(out->ap_ssid, WIFI_AP_SSID, sizeof(out->ap_ssid));

    uint8_t primary = 0;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
        out->channel = primary;
    }

    wifi_ap_record_t ap_info;
    if (s_sta_state == WIFI_STA_CONNECTED &&
        esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        out->rssi = ap_info.rssi;
        memcpy(out->sta_bssid, ap_info.bssid, sizeof(out->sta_bssid));
    }

    out->ap_clients = ap_client_count();
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
        /* One entry per BSSID: a managed network broadcasts one SSID from many
         * APs and each should stay visible. */
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

/* --- station side --- */

static void schedule_retry(void)
{
    s_sta_state = WIFI_STA_WAITING_RETRY;
    esp_timer_stop(s_retry_timer); /* may already be armed */
    if (esp_timer_start_once(s_retry_timer, WIFI_RETRY_DELAY_MS * 1000ULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to arm retry timer");
    }
}

/* A busy driver (a scan, say) gets another attempt scheduled rather than
 * leaving the state machine stuck. */
static void try_connect(void)
{
    s_sta_state = WIFI_STA_CONNECTING;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s, will retry", esp_err_to_name(err));
        schedule_retry();
    }
}

/* Writes the network config for the current attempt into the driver. */
static void apply_current_network(void)
{
    int count = wifi_store_count();
    wifi_cred_t net;
    if (count == 0 || !wifi_store_get(s_attempt % count, &net)) {
        return; /* nothing to connect to */
    }

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, net.ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, net.password, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = net.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    /* Strongest BSSID for the SSID, not the first match — the default fast
     * scan would latch onto a weaker AP of a managed network. */
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    /* An all-zero BSSID means unpinned and keeps the by-signal choice. */
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

static void start_ap(void)
{
    esp_timer_stop(s_retry_timer); /* may be armed; harmless if it is not */

    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .password = WIFI_AP_PASSWORD,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    /* Before the mode switch: dropping the link fires STA_DISCONNECTED, and the
     * handler must see the AP coming up so it schedules no reconnect. */
    s_ap_active = true;
    s_sta_state = WIFI_STA_IDLE;
    /* APSTA rather than AP: the station interface is needed for air scans */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    ESP_LOGI(TAG, "AP mode: SSID \"%s\", http://192.168.4.1/", WIFI_AP_SSID);
}

/* Drops the AP (if up) and restarts the round-robin from the first network. */
static void start_sta(void)
{
    if (wifi_store_count() == 0) {
        ESP_LOGW(TAG, "No saved networks, staying in AP mode");
        start_ap();
        return;
    }
    bool was_connected = s_sta_state == WIFI_STA_CONNECTED;
    s_attempt = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    apply_current_network();
    ESP_LOGI(TAG, "Reconnecting to \"%s\"...", s_current_ssid);
    /* Only now: STA_DISCONNECTED from leaving APSTA must still be swallowed. */
    s_ap_active = false;
    if (was_connected) {
        /* Already associated (a new BSSID saved for the same SSID, say):
         * esp_wifi_connect() would be a no-op and CONNECTING would hang. The
         * drop's STA_DISCONNECTED drives the reconnect with the new config. */
        s_sta_state = WIFI_STA_CONNECTING;
        esp_wifi_disconnect();
    } else {
        /* leaving APSTA does not raise STA_START, so connect by hand */
        try_connect();
    }
}

void wifi_ap_enable(bool on)
{
    if (on) {
        start_ap();
    } else {
        start_sta();
    }
}

void wifi_reconnect(void)
{
    start_sta();
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
        if (!s_ap_active) {
            try_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = event_data;
        ESP_LOGW(TAG, "Disconnected from \"%s\", reason %d (%s)",
                 s_current_ssid, event->reason, disconnect_reason_str(event->reason));
        if (s_ap_active) {
            return; /* dropped because we switched to AP mode — do not reconnect */
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
            start_ap();
        }
    } else if (event_base == WIFI_EVENT && (event_id == WIFI_EVENT_AP_STACONNECTED ||
                                            event_id == WIFI_EVENT_AP_STADISCONNECTED)) {
        ESP_LOGI(TAG, "AP: client %s (%d total)",
                 event_id == WIFI_EVENT_AP_STACONNECTED ? "connected" : "disconnected",
                 ap_client_count());
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_attempt = 0;
        s_sta_state = WIFI_STA_CONNECTED;
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
        start_ap();
        ESP_ERROR_CHECK(esp_wifi_start());
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    apply_current_network();

    s_sta_state = WIFI_STA_CONNECTING;
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to \"%s\"...", s_current_ssid);
    return ESP_OK;
}

const char *wifi_authmode_str(int authmode)
{
    switch ((wifi_auth_mode_t)authmode) {
    case WIFI_AUTH_OPEN:          return "open";
    case WIFI_AUTH_WEP:           return "WEP";
    case WIFI_AUTH_WPA_PSK:       return "WPA";
    case WIFI_AUTH_WPA2_PSK:      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default:                      return "?";
    }
}

const char *wifi_sta_phy_str(void)
{
    wifi_ap_record_t ap = {0};
    esp_wifi_sta_get_ap_info(&ap); /* leaves ap zeroed when not associated */

    if (ap.phy_11ax) return "Wi-Fi 6 (802.11ax)";
    if (ap.phy_11n)  return "Wi-Fi 4 (802.11n)";
    if (ap.phy_11g)  return "802.11g";
    return "802.11b";
}
