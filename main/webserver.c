#include "webserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include "lwip/sockets.h"
#include "cJSON.h"
#include "mdns.h"
#include "nvs.h"
#include "chip_temp.h"
#include "climate.h"
#include "history.h"
#include "led.h"
#include "ota.h"
#include "screen_16x2.h"
#include "sensors.h"
#include "timesync.h"
#include "weather_api.h"
#include "weather_store.h"
#include "wifi.h"
#include "wifi_store.h"

#define MDNS_HOSTNAME "weather"

static const char *TAG = "webserver";

/* The page is embedded already gzipped (see main/CMakeLists.txt) and handed to
 * the browser as-is. Embedded as BINARY, so no NUL is appended and the length
 * is exactly end - start. */
extern const char index_html_start[] asm("_binary_index_html_gz_start");
extern const char index_html_end[] asm("_binary_index_html_gz_end");

static httpd_handle_t s_server;

/* Number of currently open client sockets (includes the one serving
 * the request being handled). */
static int http_conn_count(void)
{
    size_t n = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (!s_server || httpd_get_client_list(s_server, &n, fds) != ESP_OK) {
        return 0;
    }
    return (int)n;
}

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

/* A JSON number, or `null` when there is no sensor behind the quantity —
 * a reading that does not exist must not be served as a zero. Returns dst. */
static const char *json_num(char *dst, size_t dstlen, bool ok,
                            const char *fmt, double value)
{
    if (ok) {
        snprintf(dst, dstlen, fmt, value);
    } else {
        snprintf(dst, dstlen, "null");
    }
    return dst;
}

/* Обёртка над всеми обработчиками (реальный лежит в user_ctx):
 * мигает светодиодом и логирует каждый HTTP-запрос. */
static esp_err_t handle_request(httpd_req_t *req)
{
    /* /api/status and /api/history are polled every 1-2 s by the web
     * UI — too noisy to log */
    if (strcmp(req->uri, "/api/status") != 0 &&
        strncmp(req->uri, "/api/history", 12) != 0) {
        char ip[INET6_ADDRSTRLEN] = "?";
        struct sockaddr_in6 addr;
        socklen_t addr_len = sizeof(addr);
        if (getpeername(httpd_req_to_sockfd(req),
                        (struct sockaddr *)&addr, &addr_len) == 0) {
            /* lwip reports the IPv4 client address as IPv4-mapped IPv6 */
            inet_ntop(AF_INET, &addr.sin6_addr.un.u32_addr[3], ip, sizeof(ip));
        }
        ESP_LOGI(TAG, "%s %s from %s",
                 http_method_str(req->method), req->uri, ip);
    }

    led_notify_activity();
    return ((esp_err_t (*)(httpd_req_t *))req->user_ctx)(req);
}

/* Not cached: the page changes with every firmware build. */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
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
    wifi_info_t wifi;
    wifi_get_info(&wifi);

    /* STA and AP are reported as independent objects: in APSTA both interfaces
     * can be up at once, so the station link is exposed even while the AP is on
     * (and vice versa). Static buffers are safe — one httpd task. */

    static char sta_json[400];
    {
        esp_netif_ip_info_t ip = {0};
        esp_netif_dns_info_t dns = {0};
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (nif) {
            esp_netif_get_ip_info(nif, &ip);
            esp_netif_get_dns_info(nif, ESP_NETIF_DNS_MAIN, &dns);
        }
        uint8_t mac[6] = {0};
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        wifi_ap_record_t ap = {0};
        bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        char ssid[67];
        json_escape((const char *)ap.ssid, ssid, sizeof(ssid));
        snprintf(sta_json, sizeof(sta_json),
                 "\"sta\":{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"" IPSTR "\","
                 "\"gw\":\"" IPSTR "\",\"dns\":\"" IPSTR "\",\"mac\":\"" MACSTR "\","
                 "\"channel\":%d,\"rssi\":%d,\"bssid\":\"" MACSTR "\",\"phy\":\"%s\","
                 "\"auth\":\"%s\"}",
                 connected ? "true" : "false", ssid, IP2STR(&ip.ip), IP2STR(&ip.gw),
                 IP2STR(&dns.ip.u_addr.ip4), MAC2STR(mac), ap.primary, ap.rssi,
                 MAC2STR(ap.bssid), phy_str(&ap), authmode_str(ap.authmode));
    }

    static char ap_json[440];
    {
        esp_netif_ip_info_t ip = {0};
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (nif) {
            esp_netif_get_ip_info(nif, &ip);
        }
        uint8_t mac[6] = {0};
        esp_wifi_get_mac(WIFI_IF_AP, mac);
        char ssid[67];
        json_escape(wifi.ap_ssid, ssid, sizeof(ssid));
        int off = snprintf(ap_json, sizeof(ap_json),
                           "\"ap\":{\"active\":%s,\"ssid\":\"%s\",\"ip\":\"" IPSTR "\","
                           "\"mac\":\"" MACSTR "\",\"channel\":%d,\"clients\":[",
                           wifi.ap_active ? "true" : "false", ssid, IP2STR(&ip.ip),
                           MAC2STR(mac), wifi.channel);
        if (wifi.ap_active) {
            wifi_sta_list_t sta_list = {0};
            esp_wifi_ap_get_sta_list(&sta_list);
            for (int i = 0; i < sta_list.num && off < (int)sizeof(ap_json); i++) {
                off += snprintf(ap_json + off, sizeof(ap_json) - off,
                                "%s{\"mac\":\"" MACSTR "\",\"rssi\":%d}", i ? "," : "",
                                MAC2STR(sta_list.sta[i].mac), sta_list.sta[i].rssi);
            }
        }
        if (off < (int)sizeof(ap_json)) {
            snprintf(ap_json + off, sizeof(ap_json) - off, "]}");
        }
    }

    const esp_app_desc_t *app = esp_app_get_description();

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    nvs_stats_t nvs = {0};
    nvs_get_stats(NULL, &nvs);

    scd40_data_t air = {0};
    bool air_ok = sensors_scd40_get(&air);

    /* One object per I2C device, so an absent or failing sensor still shows
     * up with its own `ok` instead of vanishing from the reply. */
    tmp117_data_t tmp117 = {0};
    bool tmp117_ok = sensors_tmp117_get(&tmp117);

    bmp581_data_t bmp581 = {0};
    bool bmp581_ok = sensors_bmp581_get(&bmp581);

    veml7700_data_t veml = { .gain = "" };
    bool veml_ok = sensors_veml7700_get(&veml);

    weather_api_data_t weather;
    bool weather_ok = weather_api_get(&weather);

    weather_location_t wloc;
    bool wloc_ok = weather_store_get_active_location(&wloc);
    char weather_name[67];
    json_escape(wloc_ok ? wloc.name : "", weather_name, sizeof(weather_name));

    char time_str[32] = "--.--.---- --:--:--";
    if (timesync_is_synced()) {
        time_t now;
        time(&now);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(time_str, sizeof(time_str), "%d.%m.%Y %H:%M:%S", &tm);
    }

    /* The room, as opposed to the chips below: one source per quantity, no
     * substitutions. This is what the main cards and the charts show, so the
     * value on a card and the line under it always come from the same
     * sensor. */
    climate_t cl;
    climate_get(&cl);
    char cl_temp[12], cl_rh[12], cl_co2[12], cl_press[12], cl_msl[12], cl_lux[16];
    json_num(cl_temp, sizeof(cl_temp), cl.temp_ok, "%.2f", cl.temp_c);
    json_num(cl_rh, sizeof(cl_rh), cl.rh_ok, "%.1f", cl.rh_pct);
    json_num(cl_co2, sizeof(cl_co2), cl.co2_ok, "%.0f", (double)cl.co2_ppm);
    json_num(cl_press, sizeof(cl_press), cl.press_ok, "%.3f", cl.press_hpa);
    json_num(cl_msl, sizeof(cl_msl), cl.press_ok, "%.3f", cl.press_msl_hpa);
    json_num(cl_lux, sizeof(cl_lux), cl.lux_ok, "%.1f", cl.lux);

    static char json[3072];
    int len = snprintf(json, sizeof(json),
        "{%s,%s,"
        "\"climate\":{"
        "\"temp\":%s,\"rh\":%s,\"co2\":%s,"
        "\"press\":%s,\"press_msl\":%s,\"lux\":%s},"
        "\"sensors\":{\"chip_temp\":%.1f,"
        "\"scd40\":{\"ok\":%s,\"co2\":%u,\"temp\":%.1f,\"rh\":%.1f},"
        "\"tmp117\":{\"ok\":%s,\"temp\":%.2f},"
        "\"bmp581\":{\"ok\":%s,\"press\":%.3f,\"press_pa\":%.1f,\"temp\":%.2f},"
        "\"veml7700\":{\"ok\":%s,"
        "\"lux\":%.1f,\"white\":%.1f,\"als_raw\":%u,\"white_raw\":%u,"
        "\"gain\":\"%s\",\"it\":%u}},"
        "\"weather\":{\"ok\":%s,\"temp\":%.1f,\"feels\":%.1f,"
        "\"tmin\":%.1f,\"tmax\":%.1f,"
        "\"hum\":%.0f,\"press\":%.0f,\"press_msl\":%.0f,"
        "\"uvi\":%.1f,\"wind\":%.1f,\"gust\":%.1f,"
        "\"wind_dir\":%d,\"precip\":%.1f,"
        "\"clouds\":%d,\"code\":%d,"
        "\"elev\":%.0f,\"utc_offset\":%d,\"age\":%d,"
        "\"name\":\"%s\",\"active\":%d,"
        "\"lat\":%.4f,\"lon\":%.4f},"
        "\"system\":{"
        "\"uptime\":%lld,\"time\":\"%s\",\"time_synced\":%s,"
        "\"app_version\":\"%s\",\"build\":\"%s %s\",\"idf_ver\":\"%s\","
        "\"chip_rev\":\"v%d.%d\",\"cpu_mhz\":%d,\"flash_mb\":%lu,"
        "\"reset_reason\":\"%s\",\"cpu_load\":%d,\"tasks\":%u,"
        "\"http_conns\":%d,"
        "\"heap_free\":%u,\"heap_min\":%u,\"heap_total\":%u,\"heap_largest\":%u,"
        "\"nvs_used\":%u,\"nvs_total\":%u},"
        "\"settings\":{\"led_brightness\":%u,"
        "\"backlight_rgb\":\"%06X\",\"altitude\":%d}}",
        sta_json, ap_json,
        cl_temp, cl_rh, cl_co2, cl_press, cl_msl, cl_lux,
        chip_temp_celsius(),
        air_ok ? "true" : "false", air.co2_ppm, air.temp_c, air.rh_pct,
        tmp117_ok ? "true" : "false", tmp117.temp_c,
        bmp581_ok ? "true" : "false",
        bmp581.press_hpa, bmp581.press_hpa * 100.0f, bmp581.temp_c,
        veml_ok ? "true" : "false",
        veml.lux, veml.white_lux, veml.als_raw, veml.white_raw,
        veml.gain, veml.it_ms,
        weather_ok ? "true" : "false", weather.temp_c, weather.feels_c,
        weather.temp_min_c, weather.temp_max_c,
        weather.humidity_pct, weather.pressure_hpa, weather.pressure_msl_hpa,
        weather.uvi, weather.wind_kmh, weather.gust_kmh,
        weather.wind_dir_deg, weather.precip_mm,
        weather.cloud_pct, weather.weather_code,
        weather.elevation_m, weather.utc_offset_s, (int)weather.age_s,
        weather_name, weather_store_get_active(),
        wloc_ok ? wloc.lat : 0, wloc_ok ? wloc.lon : 0,
        esp_timer_get_time() / 1000000,
        time_str,
        timesync_is_synced() ? "true" : "false",
        app->version, app->date, app->time, app->idf_ver,
        chip.revision / 100, chip.revision % 100,
        CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        (unsigned long)(flash_size / (1024 * 1024)),
        reset_reason_str(), cpu_load_percent(),
        (unsigned)uxTaskGetNumberOfTasks(),
        http_conn_count(),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)heap_caps_get_total_size(MALLOC_CAP_DEFAULT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
        (unsigned)nvs.used_entries, (unsigned)nvs.total_entries,
        (unsigned)led_get_brightness(),
        (unsigned)screen_16x2_backlight_rgb(),
        climate_altitude_m());

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, len);
}

/* History as arrays per metric (null = gap); ?p=5m|1h|1d selects the
 * ring (default 1d). The largest response is ~15-20 KB, so it is
 * streamed in chunks. */
static esp_err_t history_get_handler(httpd_req_t *req)
{
    history_tier_t tier = HISTORY_1D;
    char query[16], val_p[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "p", val_p, sizeof(val_p)) == ESP_OK) {
        if (strcmp(val_p, "5m") == 0) {
            tier = HISTORY_5M;
        } else if (strcmp(val_p, "1h") == 0) {
            tier = HISTORY_1H;
        } else if (strcmp(val_p, "1d") != 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "bad period");
        }
    }

    static char buf[1024]; /* single httpd task */
    httpd_resp_set_type(req, "application/json");

    /* One array per quantity, each gated on its own bit: a sensor that was
     * absent for part of the window leaves nulls in its own series without
     * punching holes in the others. */
    static const struct {
        const char *name;
        uint8_t bit;
    } series[] = {
        { "co2",   HISTORY_HAS_CO2 },
        { "temp",  HISTORY_HAS_TEMP },
        { "rh",    HISTORY_HAS_RH },
        { "press", HISTORY_HAS_PRESS },
        { "lux",   HISTORY_HAS_LUX },
    };

    int len = snprintf(buf, sizeof(buf), "{\"period\":%d",
                       history_interval(tier));
    for (int m = 0; m < (int)(sizeof(series) / sizeof(series[0])); m++) {
        len += snprintf(buf + len, sizeof(buf) - len, ",\"%s\":[",
                        series[m].name);
        int count = history_count(tier);
        for (int i = 0; i < count; i++) {
            history_point_t p;
            char val[16] = "null";
            if (history_get(tier, i, &p) && (p.have & series[m].bit)) {
                switch (series[m].bit) {
                case HISTORY_HAS_CO2:
                    snprintf(val, sizeof(val), "%u", p.co2_ppm);
                    break;
                case HISTORY_HAS_TEMP:
                    snprintf(val, sizeof(val), "%.2f", p.temp_cx100 / 100.0);
                    break;
                case HISTORY_HAS_RH:
                    snprintf(val, sizeof(val), "%.1f", p.rh_dpct / 10.0);
                    break;
                case HISTORY_HAS_PRESS:
                    snprintf(val, sizeof(val), "%.3f", p.press_mhpa / 1000.0);
                    break;
                default:
                    snprintf(val, sizeof(val), "%.1f", p.lux);
                    break;
                }
            }
            len += snprintf(buf + len, sizeof(buf) - len, "%s%s",
                            i ? "," : "", val);
            if (len > (int)sizeof(buf) - 32) {
                if (httpd_resp_send_chunk(req, buf, len) != ESP_OK) {
                    return ESP_FAIL;
                }
                len = 0;
            }
        }
        len += snprintf(buf + len, sizeof(buf) - len, "]");
    }
    len += snprintf(buf + len, sizeof(buf) - len, "}");
    httpd_resp_send_chunk(req, buf, len);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    static wifi_scan_ap_t aps[15];
    int n = wifi_scan(aps, 15);
    if (n < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "scan failed");
    }

    static char json[2048];
    char essid[67];
    int off = snprintf(json, sizeof(json), "[");
    for (int i = 0; i < n && off < (int)sizeof(json) - 2; i++) {
        off += snprintf(json + off, sizeof(json) - off,
                        "%s{\"ssid\":\"%s\",\"bssid\":\"" MACSTR "\","
                        "\"ch\":%d,\"rssi\":%d,\"auth\":\"%s\"}",
                        i ? "," : "",
                        json_escape(aps[i].ssid, essid, sizeof(essid)),
                        MAC2STR(aps[i].bssid),
                        aps[i].channel,
                        aps[i].rssi,
                        authmode_str((wifi_auth_mode_t)aps[i].authmode));
    }
    off += snprintf(json + off, sizeof(json) - off, "]");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, off);
}

static esp_err_t networks_get_handler(httpd_req_t *req)
{
    static char json[1024];
    char essid[67];
    wifi_cred_t net;
    int off = snprintf(json, sizeof(json), "[");
    for (int i = 0; wifi_store_get(i, &net); i++) {
        bool pinned = false;
        for (int k = 0; k < 6; k++) {
            if (net.bssid[k]) {
                pinned = true;
                break;
            }
        }
        off += snprintf(json + off, sizeof(json) - off, "%s{\"ssid\":\"%s\"",
                        i ? "," : "", json_escape(net.ssid, essid, sizeof(essid)));
        if (pinned) {
            off += snprintf(json + off, sizeof(json) - off,
                            ",\"bssid\":\"" MACSTR "\"", MAC2STR(net.bssid));
        }
        off += snprintf(json + off, sizeof(json) - off, "}");
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

/* Parses "xx:xx:xx:xx:xx:xx" into 6 bytes; false on any other format. */
static bool parse_bssid(const char *s, uint8_t out[6])
{
    return s && sscanf(s, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
                       &out[0], &out[1], &out[2],
                       &out[3], &out[4], &out[5]) == 6;
}

static esp_err_t network_add_post_handler(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ssid"));
    const char *password = cJSON_GetStringValue(cJSON_GetObjectItem(root, "password"));
    const char *bssid_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "bssid"));
    uint8_t bssid[6];
    bool have_bssid = parse_bssid(bssid_str, bssid);
    esp_err_t err = ssid ? wifi_store_add(ssid, password, have_bssid ? bssid : NULL)
                         : ESP_ERR_INVALID_ARG;
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

static esp_err_t locations_get_handler(httpd_req_t *req)
{
    static char json[1024];
    char name[100];
    int off = snprintf(json, sizeof(json), "{\"active\":%d,\"locations\":[",
                       weather_store_get_active());
    weather_location_t loc;
    for (int i = 0; weather_store_get(i, &loc); i++) {
        off += snprintf(json + off, sizeof(json) - off,
                        "%s{\"name\":\"%s\",\"lat\":%.4f,\"lon\":%.4f}",
                        i ? "," : "", json_escape(loc.name, name, sizeof(name)),
                        loc.lat, loc.lon);
    }
    off += snprintf(json + off, sizeof(json) - off, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, off);
}

/* Coordinates are resolved from a place name by the browser (Open-Meteo
 * geocoding) and posted here already numeric. */
static esp_err_t location_add_post_handler(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    const cJSON *lat = cJSON_GetObjectItem(root, "lat");
    const cJSON *lon = cJSON_GetObjectItem(root, "lon");
    esp_err_t err = (name && cJSON_IsNumber(lat) && cJSON_IsNumber(lon))
        ? weather_store_add(name, (float)lat->valuedouble, (float)lon->valuedouble)
        : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   err == ESP_ERR_NO_MEM ? "list is full"
                                                         : "bad location");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t location_delete_handler(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const cJSON *index = cJSON_GetObjectItem(root, "index");
    esp_err_t err = cJSON_IsNumber(index) ? weather_store_remove(index->valueint)
                                          : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not found");
    }
    weather_api_refresh(); /* the active location may have shifted */
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t location_active_put_handler(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const cJSON *index = cJSON_GetObjectItem(root, "index");
    esp_err_t err = cJSON_IsNumber(index) ? weather_store_set_active(index->valueint)
                                          : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index");
    }
    weather_api_refresh();
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

    /* The backlight arrives as a ready RGB hex string ("00AAFF", a leading
     * "#" is tolerated): picking the color is the UI's job, the device only
     * stores what it is given. */
    const cJSON *rgb = cJSON_GetObjectItem(root, "backlight_rgb");
    if (cJSON_IsString(rgb) && rgb->valuestring != NULL) {
        const char *hex = rgb->valuestring;
        hex += (*hex == '#');
        char *end;
        unsigned long value = strtoul(hex, &end, 16);
        if (end != hex && *end == '\0' && value <= 0xFFFFFF) {
            screen_16x2_set_backlight((uint32_t)value);
        }
    }
    /* Height of the station above sea level, metres — the only input to the
     * reduction of the measured pressure to sea level. */
    const cJSON *alt = cJSON_GetObjectItem(root, "altitude");
    if (cJSON_IsNumber(alt) && alt->valueint >= CLIMATE_ALTITUDE_MIN &&
        alt->valueint <= CLIMATE_ALTITUDE_MAX) {
        climate_set_altitude_m(alt->valueint);
    }

    cJSON_Delete(root);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t scd40_calibrate_post_handler(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const cJSON *ppm = cJSON_GetObjectItem(root, "ppm");
    int target = cJSON_IsNumber(ppm) ? ppm->valueint : 0;
    cJSON_Delete(root);
    if (target < 400 || target > 2000) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ppm");
    }

    int correction = 0;
    esp_err_t err = sensors_scd40_calibrate((uint16_t)target, &correction);
    if (err == ESP_ERR_INVALID_STATE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "sensor offline");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "calibration rejected");
    }
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"correction\":%d}", correction);
    return httpd_resp_sendstr(req, resp);
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
    config.max_uri_handlers = 20; /* дефолтных 8 уже впритык */
    /* Recycle the least-recently-used connection instead of holding all
     * max_open_sockets slots, so the browser's keep-alive polling can't
     * starve the shared LWIP socket pool of outbound TLS clients. */
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    s_server = server;

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handle_request,
        .user_ctx = index_get_handler,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = handle_request,
        .user_ctx = status_get_handler,
    };
    const httpd_uri_t history_uri = {
        .uri = "/api/history",
        .method = HTTP_GET,
        .handler = handle_request,
        .user_ctx = history_get_handler,
    };
    const httpd_uri_t scan_uri = {
        .uri = "/api/scan",
        .method = HTTP_GET,
        .handler = handle_request,
        .user_ctx = scan_get_handler,
    };
    const httpd_uri_t networks_uri = {
        .uri = "/api/networks",
        .method = HTTP_GET,
        .handler = handle_request,
        .user_ctx = networks_get_handler,
    };
    const httpd_uri_t network_add_uri = {
        .uri = "/api/networks/add",
        .method = HTTP_POST,
        .handler = handle_request,
        .user_ctx = network_add_post_handler,
    };
    const httpd_uri_t network_delete_uri = {
        .uri = "/api/networks/delete",
        .method = HTTP_POST,
        .handler = handle_request,
        .user_ctx = network_delete_post_handler,
    };
    const httpd_uri_t locations_get_uri = {
        .uri = "/api/locations",
        .method = HTTP_GET,
        .handler = handle_request,
        .user_ctx = locations_get_handler,
    };
    const httpd_uri_t location_add_uri = {
        .uri = "/api/locations",
        .method = HTTP_POST,
        .handler = handle_request,
        .user_ctx = location_add_post_handler,
    };
    const httpd_uri_t location_delete_uri = {
        .uri = "/api/locations",
        .method = HTTP_DELETE,
        .handler = handle_request,
        .user_ctx = location_delete_handler,
    };
    const httpd_uri_t location_active_uri = {
        .uri = "/api/locations/active",
        .method = HTTP_PUT,
        .handler = handle_request,
        .user_ctx = location_active_put_handler,
    };
    const httpd_uri_t connect_uri = {
        .uri = "/api/connect",
        .method = HTTP_POST,
        .handler = handle_request,
        .user_ctx = connect_post_handler,
    };
    const httpd_uri_t settings_uri = {
        .uri = "/api/settings",
        .method = HTTP_POST,
        .handler = handle_request,
        .user_ctx = settings_post_handler,
    };
    const httpd_uri_t scd40_cal_uri = {
        .uri = "/api/scd40/calibrate",
        .method = HTTP_POST,
        .handler = handle_request,
        .user_ctx = scd40_calibrate_post_handler,
    };
    const httpd_uri_t ota_uri = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = handle_request,
        .user_ctx = ota_post_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &history_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &scan_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &networks_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &network_add_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &network_delete_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &locations_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &location_add_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &location_delete_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &location_active_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &connect_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &settings_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &scd40_cal_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota_uri));

    ESP_LOGI(TAG, "Web server started: http://%s.local/", MDNS_HOSTNAME);
}
