#include "webserver.h"

#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "mdns.h"
#include "nvs.h"
#include "led.h"
#include "sensors.h"
#include "wifi.h"
#include "wifi_store.h"

#define MDNS_HOSTNAME "weather"

static const char *TAG = "webserver";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

/* Экранирует " и \ для подстановки строки в JSON; управляющие символы
 * отбрасывает. Возвращает dst. */
static const char *json_escape(const char *src, char *dst, size_t dstlen)
{
    size_t o = 0;
    for (; *src && o + 2 < dstlen; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = c;
        } else if (c >= 0x20) {
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
    return dst;
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static const char *authmode_str(wifi_auth_mode_t mode)
{
    switch (mode) {
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

static const char *phy_str(const wifi_ap_record_t *ap)
{
    if (ap->phy_11ax) return "Wi-Fi 6 (802.11ax)";
    if (ap->phy_11n)  return "Wi-Fi 4 (802.11n)";
    if (ap->phy_11g)  return "802.11g";
    return "802.11b";
}

static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default:                return "other";
    }
}

/* Загрузка CPU между вызовами: доля времени вне задачи IDLE.
 * Счётчики 32-битные (мкс), переполнение (~71 мин) корректно
 * съедается вычитанием в uint32_t. */
static int cpu_load_percent(void)
{
    static uint32_t prev_idle, prev_total;
    uint32_t idle = ulTaskGetIdleRunTimeCounter();
    uint32_t total = (uint32_t)esp_timer_get_time();
    uint32_t d_idle = idle - prev_idle;
    uint32_t d_total = total - prev_total;
    prev_idle = idle;
    prev_total = total;

    if (d_total == 0 || d_idle > d_total) {
        return 0;
    }
    return 100 - (int)((uint64_t)100 * d_idle / d_total);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    bool ap_mode = wifi_get_status() == WIFI_STATUS_AP_MODE;

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_dns_info_t dns = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(
        ap_mode ? "WIFI_AP_DEF" : "WIFI_STA_DEF");
    if (netif) {
        esp_netif_get_ip_info(netif, &ip_info);
        esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    }

    uint8_t mac[6] = {0};
    esp_wifi_get_mac(ap_mode ? WIFI_IF_AP : WIFI_IF_STA, mac);

    /* буферы статические: обработчики выполняются в единственной задаче httpd */
    static char wifi_json[300];
    if (ap_mode) {
        wifi_sta_list_t sta_list = {0};
        esp_wifi_ap_get_sta_list(&sta_list);
        int off = snprintf(wifi_json, sizeof(wifi_json), "\"clients\":[");
        for (int i = 0; i < sta_list.num && off < (int)sizeof(wifi_json); i++) {
            off += snprintf(wifi_json + off, sizeof(wifi_json) - off,
                            "%s{\"mac\":\"" MACSTR "\",\"rssi\":%d}",
                            i ? "," : "",
                            MAC2STR(sta_list.sta[i].mac), sta_list.sta[i].rssi);
        }
        if (off < (int)sizeof(wifi_json)) {
            snprintf(wifi_json + off, sizeof(wifi_json) - off, "]");
        }
    } else {
        wifi_ap_record_t ap = {0};
        esp_wifi_sta_get_ap_info(&ap);
        snprintf(wifi_json, sizeof(wifi_json),
                 "\"rssi\":%d,\"bssid\":\"" MACSTR "\",\"phy\":\"%s\",\"auth\":\"%s\"",
                 ap.rssi, MAC2STR(ap.bssid), phy_str(&ap), authmode_str(ap.authmode));
    }

    const esp_app_desc_t *app = esp_app_get_description();

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    nvs_stats_t nvs = {0};
    nvs_get_stats(NULL, &nvs);

    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%d.%m.%Y %H:%M:%S", &tm);

    char essid[67];
    json_escape(wifi_get_ssid(), essid, sizeof(essid));

    static char json[1024];
    int len = snprintf(json, sizeof(json),
        "{\"ap_mode\":%s,\"ssid\":\"%s\",\"ip\":\"" IPSTR "\",\"gw\":\"" IPSTR "\","
        "\"dns\":\"" IPSTR "\",\"mac\":\"" MACSTR "\",\"channel\":%d,%s,"
        "\"uptime\":%lld,\"temp\":%.1f,\"time\":\"%s\","
        "\"app_version\":\"%s\",\"build\":\"%s %s\",\"idf_ver\":\"%s\","
        "\"chip_rev\":\"v%d.%d\",\"cpu_mhz\":%d,\"flash_mb\":%lu,"
        "\"reset_reason\":\"%s\",\"cpu_load\":%d,\"tasks\":%u,"
        "\"heap_free\":%u,\"heap_min\":%u,\"heap_total\":%u,\"heap_largest\":%u,"
        "\"nvs_used\":%u,\"nvs_total\":%u,\"led_brightness\":%u}",
        ap_mode ? "true" : "false",
        essid, IP2STR(&ip_info.ip), IP2STR(&ip_info.gw),
        IP2STR(&dns.ip.u_addr.ip4), MAC2STR(mac), wifi_get_channel(),
        wifi_json,
        esp_timer_get_time() / 1000000,
        sensors_chip_temp(), time_str,
        app->version, app->date, app->time, app->idf_ver,
        chip.revision / 100, chip.revision % 100,
        CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        (unsigned long)(flash_size / (1024 * 1024)),
        reset_reason_str(), cpu_load_percent(),
        (unsigned)uxTaskGetNumberOfTasks(),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)heap_caps_get_total_size(MALLOC_CAP_DEFAULT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
        (unsigned)nvs.used_entries, (unsigned)nvs.total_entries,
        (unsigned)led_get_brightness());

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, len);
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    static wifi_scan_ap_t aps[15];
    int n = wifi_scan(aps, 15);
    if (n < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "scan failed");
    }

    static char json[1024];
    char essid[67];
    int off = snprintf(json, sizeof(json), "[");
    for (int i = 0; i < n && off < (int)sizeof(json) - 2; i++) {
        off += snprintf(json + off, sizeof(json) - off,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\"}",
                        i ? "," : "",
                        json_escape(aps[i].ssid, essid, sizeof(essid)),
                        aps[i].rssi,
                        authmode_str((wifi_auth_mode_t)aps[i].authmode));
    }
    off += snprintf(json + off, sizeof(json) - off, "]");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, off);
}

static esp_err_t networks_get_handler(httpd_req_t *req)
{
    char json[512];
    char ssid[33], essid[67];
    int off = snprintf(json, sizeof(json), "[");
    for (int i = 0; wifi_store_get_ssid(i, ssid); i++) {
        off += snprintf(json + off, sizeof(json) - off,
                        "%s{\"ssid\":\"%s\"}", i ? "," : "",
                        json_escape(ssid, essid, sizeof(essid)));
    }
    off += snprintf(json + off, sizeof(json) - off, "]");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, off);
}

/* Читает тело запроса и парсит JSON; NULL при ошибке. */
static cJSON *read_json_body(httpd_req_t *req)
{
    char body[256];
    if (req->content_len >= sizeof(body)) {
        return NULL;
    }
    int received = 0;
    while (received < (int)req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            return NULL;
        }
        received += r;
    }
    body[received] = '\0';
    return cJSON_Parse(body);
}

static esp_err_t network_add_post_handler(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ssid"));
    const char *password = cJSON_GetStringValue(cJSON_GetObjectItem(root, "password"));
    esp_err_t err = ssid ? wifi_store_add(ssid, password) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   err == ESP_ERR_NO_MEM ? "list is full" : "bad ssid");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t network_delete_post_handler(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ssid"));
    esp_err_t err = ssid ? wifi_store_remove(ssid) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not found");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const cJSON *bright = cJSON_GetObjectItem(root, "led_brightness");
    if (cJSON_IsNumber(bright) && bright->valueint >= 1 && bright->valueint <= 255) {
        led_set_brightness((uint8_t)bright->valueint);
    }
    cJSON_Delete(root);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    /* ответ уходит до переключения режима, иначе клиент его не получит */
    esp_err_t ret = httpd_resp_sendstr(req, "{\"ok\":true}");
    wifi_reconnect();
    return ret;
}

static void mdns_start(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set("Weather Station"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
}

void webserver_start(void)
{
    mdns_start();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192; /* дефолтных 4 КиБ не хватает обработчику /api/status */
    config.max_uri_handlers = 16; /* дефолтных 8 уже впритык */
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    const httpd_uri_t scan_uri = {
        .uri = "/api/scan",
        .method = HTTP_GET,
        .handler = scan_get_handler,
    };
    const httpd_uri_t networks_uri = {
        .uri = "/api/networks",
        .method = HTTP_GET,
        .handler = networks_get_handler,
    };
    const httpd_uri_t network_add_uri = {
        .uri = "/api/networks/add",
        .method = HTTP_POST,
        .handler = network_add_post_handler,
    };
    const httpd_uri_t network_delete_uri = {
        .uri = "/api/networks/delete",
        .method = HTTP_POST,
        .handler = network_delete_post_handler,
    };
    const httpd_uri_t connect_uri = {
        .uri = "/api/connect",
        .method = HTTP_POST,
        .handler = connect_post_handler,
    };
    const httpd_uri_t settings_uri = {
        .uri = "/api/settings",
        .method = HTTP_POST,
        .handler = settings_post_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &scan_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &networks_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &network_add_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &network_delete_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &connect_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &settings_uri));

    ESP_LOGI(TAG, "Web server started: http://%s.local/", MDNS_HOSTNAME);
}
