#include "webserver.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "mdns.h"
#include "climate.h"
#include "forecast.h"
#include "history.h"
#include "led.h"
#include "ota.h"
#include "screen_16x2.h"
#include "sensors.h"
#include "sysinfo.h"
#include "timesync.h"
#include "weather_api.h"
#include "weather_store.h"
#include "wifi.h"
#include "wifi_store.h"

#define MDNS_HOSTNAME "weather"

static const char *TAG = "webserver";

/* The page is embedded already gzipped (see main/CMakeLists.txt) and served
 * as-is. Embedded as BINARY: no trailing NUL, length is exactly end - start. */
extern const char index_html_start[] asm("_binary_index_html_gz_start");
extern const char index_html_end[] asm("_binary_index_html_gz_end");

static httpd_handle_t s_server;

/* Includes the socket serving the request being handled. */
static int http_conn_count(void)
{
    size_t n = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (!s_server || httpd_get_client_list(s_server, &n, fds) != ESP_OK) {
        return 0;
    }
    return (int)n;
}

/* Bounded assembly of a JSON reply. snprintf() returns the length it *wanted*
 * to write, so an `off += snprintf(buf + off, sizeof(buf) - off, ...)` chain
 * walks past the end of the buffer once a reply does not fit; appending
 * through jbuf clamps instead and records the truncation. */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;      /* always < cap; buf stays NUL-terminated */
    bool truncated;
} jbuf_t;

static void jbuf_init(jbuf_t *j, char *buf, size_t cap)
{
    j->buf = buf;
    j->cap = cap;
    j->len = 0;
    j->truncated = false;
    buf[0] = '\0';
}

static void jbuf_printf(jbuf_t *j, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void jbuf_printf(jbuf_t *j, const char *fmt, ...)
{
    size_t left = j->cap - j->len;
    if (left <= 1) {
        j->truncated = true;
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->len, left, fmt, ap);
    va_end(ap);

    if (n < 0) {
        j->truncated = true;
    } else if ((size_t)n >= left) {
        j->len = j->cap - 1; /* vsnprintf wrote up to the NUL and stopped */
        j->truncated = true;
    } else {
        j->len += n;
    }
}

/* A truncated reply is broken JSON; only the log says which buffer ran out. */
static esp_err_t jbuf_send(httpd_req_t *req, const jbuf_t *j)
{
    if (j->truncated) {
        ESP_LOGW(TAG, "%s: reply truncated at %u bytes", req->uri,
                 (unsigned)j->cap);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, j->buf, j->len);
}

/* Escapes " and \ for a JSON string; drops control characters. Returns dst. */
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

/* A JSON number, or `null` — a reading that does not exist must not be served
 * as a zero. Returns dst. */
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

/* Wraps every handler (the real one sits in user_ctx): blinks the LED and
 * logs the request. */
static esp_err_t handle_request(httpd_req_t *req)
{
    /* Polled every 1-2 s by the web UI — too noisy to log. */
    if (strcmp(req->uri, "/api/status") != 0 &&
        !(req->method == HTTP_GET &&
          strncmp(req->uri, "/api/history", 12) == 0)) {
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

static esp_err_t status_get_handler(httpd_req_t *req)
{
    wifi_info_t wifi;
    wifi_get_info(&wifi);

    /* STA and AP are independent objects: in APSTA both can be up at once.
     * Static buffers are safe — one httpd task. */

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
                 MAC2STR(ap.bssid), wifi_sta_phy_str(),
                 wifi_authmode_str(ap.authmode));
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
        jbuf_t j;
        jbuf_init(&j, ap_json, sizeof(ap_json));
        jbuf_printf(&j, "\"ap\":{\"active\":%s,\"ssid\":\"%s\",\"ip\":\"" IPSTR "\","
                        "\"mac\":\"" MACSTR "\",\"channel\":%d,\"clients\":[",
                    wifi.ap_active ? "true" : "false", ssid, IP2STR(&ip.ip),
                    MAC2STR(mac), wifi.channel);
        if (wifi.ap_active) {
            wifi_sta_list_t sta_list = {0};
            esp_wifi_ap_get_sta_list(&sta_list);
            for (int i = 0; i < sta_list.num; i++) {
                jbuf_printf(&j, "%s{\"mac\":\"" MACSTR "\",\"rssi\":%d}",
                            i ? "," : "",
                            MAC2STR(sta_list.sta[i].mac), sta_list.sta[i].rssi);
            }
        }
        jbuf_printf(&j, "]}");
    }

    const sysinfo_static_t *sys = sysinfo_static();
    sysinfo_runtime_t run;
    sysinfo_get_runtime(&run);

    scd40_data_t air = {0};
    bool air_ok = sensors_scd40_get(&air);

    /* One object per I2C device, so a failing sensor still shows up with its
     * own `ok` instead of vanishing from the reply. */
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

    char time_str[TIMESYNC_STR_LEN];
    timesync_format(time_str, sizeof(time_str));

    /* The room, as opposed to the chips below: one source per quantity, so a
     * card and its chart always come from the same sensor. */
    climate_t cl;
    climate_get(&cl);
    char cl_temp[12], cl_rh[12], cl_co2[12], cl_press[12], cl_msl[12], cl_lux[16];
    json_num(cl_temp, sizeof(cl_temp), cl.temp_ok, "%.2f", cl.temp_c);
    json_num(cl_rh, sizeof(cl_rh), cl.rh_ok, "%.1f", cl.rh_pct);
    json_num(cl_co2, sizeof(cl_co2), cl.co2_ok, "%.0f", (double)cl.co2_ppm);
    json_num(cl_press, sizeof(cl_press), cl.press_ok, "%.3f", cl.press_hpa);
    json_num(cl_msl, sizeof(cl_msl), cl.press_ok, "%.3f", cl.press_msl_hpa);
    json_num(cl_lux, sizeof(cl_lux), cl.lux_ok, "%.1f", cl.lux);

    /* Absent as a whole until three hours of pressure are on record. */
    forecast_t fc;
    forecast_get(&fc);
    char fc_json[64] = "null";
    if (fc.ok) {
        snprintf(fc_json, sizeof(fc_json),
                 "{\"trend\":%d,\"delta_3h\":%.2f,\"code\":%u}", fc.trend,
                 fc.delta_3h_hpa, fc.code);
    }

    static char json[3072];
    jbuf_t j;
    jbuf_init(&j, json, sizeof(json));
    jbuf_printf(&j,
        "{%s,%s,"
        "\"climate\":{"
        "\"temp\":%s,\"rh\":%s,\"co2\":%s,"
        "\"press\":%s,\"press_msl\":%s,\"lux\":%s},"
        "\"forecast\":%s,"
        "\"sensors\":{"
        "\"scd40\":{\"ok\":%s,\"co2\":%u,\"temp\":%.1f,\"rh\":%.1f,"
        "\"dew\":%.1f},"
        "\"tmp117\":{\"ok\":%s,\"temp\":%.2f},"
        "\"bmp581\":{\"ok\":%s,\"press\":%.3f,\"press_pa\":%.1f,\"temp\":%.2f},"
        "\"veml7700\":{\"ok\":%s,"
        "\"lux\":%.1f,\"white_ratio\":%.2f,"
        "\"gain\":\"%s\",\"it\":%u}},"
        "\"weather\":{\"ok\":%s,\"temp\":%.1f,\"feels\":%.1f,"
        "\"tmin\":%.1f,\"tmax\":%.1f,"
        "\"hum\":%.0f,\"press\":%.1f,\"press_msl\":%.1f,"
        "\"uvi\":%.2f,\"wind\":%.1f,\"gust\":%.1f,"
        "\"wind_dir\":%d,\"precip\":%.2f,"
        "\"clouds\":%d,\"code\":%d,"
        "\"elev\":%.0f,\"utc_offset\":%d,\"age\":%d,"
        "\"name\":\"%s\",\"active\":%d,"
        "\"lat\":%.4f,\"lon\":%.4f},"
        "\"system\":{"
        "\"uptime\":%lld,\"time\":\"%s\",\"time_synced\":%s,"
        "\"app_version\":\"%s\",\"build\":\"%s %s\",\"idf_ver\":\"%s\","
        "\"chip_rev\":\"v%d.%d\",\"chip_temp\":%.1f,"
        "\"cpu_mhz\":%d,\"flash_mb\":%lu,"
        "\"reset_reason\":\"%s\",\"cpu_load\":%d,\"tasks\":%u,"
        "\"http_conns\":%d,"
        "\"heap_free\":%u,\"heap_min\":%u,\"heap_total\":%u,\"heap_largest\":%u,"
        "\"nvs_used\":%u,\"nvs_total\":%u},"
        "\"settings\":{\"led_brightness\":%u,"
        "\"backlight_rgb\":\"%06X\",\"backlight_scale\":%u,"
        "\"altitude\":%d}}",
        sta_json, ap_json,
        cl_temp, cl_rh, cl_co2, cl_press, cl_msl, cl_lux,
        fc_json,
        air_ok ? "true" : "false", air.co2_ppm, air.temp_c, air.rh_pct,
        air.dew_c,
        tmp117_ok ? "true" : "false", tmp117.temp_c,
        bmp581_ok ? "true" : "false",
        bmp581.press_hpa, bmp581.press_hpa * 100.0f, bmp581.temp_c,
        veml_ok ? "true" : "false",
        veml.lux, veml.white_ratio,
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
        run.uptime_s,
        time_str,
        timesync_is_synced() ? "true" : "false",
        sys->app_version, sys->build_date, sys->build_time, sys->idf_ver,
        sys->chip_rev_major, sys->chip_rev_minor,
        sysinfo_chip_temp_c(),
        sys->cpu_mhz,
        (unsigned long)sys->flash_mb,
        sysinfo_reset_reason_str(), run.cpu_load_pct,
        run.tasks,
        http_conn_count(),
        run.heap_free, run.heap_min, run.heap_total, run.heap_largest,
        (unsigned)sys->nvs_used_entries, (unsigned)sys->nvs_total_entries,
        (unsigned)led_get_brightness(),
        (unsigned)screen_16x2_backlight_rgb(),
        (unsigned)screen_16x2_backlight_scale(),
        climate_altitude_m());

    return jbuf_send(req, &j);
}

/* History as arrays per metric (null = gap); ?p=5m|1h|1d selects the ring
 * (default 1d). Up to ~20 KB, so it is streamed in chunks. */
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

    /* Each array is gated on its own bit: a sensor absent for part of the
     * window leaves nulls only in its own series. */
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

    jbuf_t j;
    jbuf_init(&j, buf, sizeof(buf));
    jbuf_printf(&j, "{\"period\":%d", history_interval(tier));
    for (int m = 0; m < (int)(sizeof(series) / sizeof(series[0])); m++) {
        jbuf_printf(&j, ",\"%s\":[", series[m].name);
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
                    /* Stored as measured, served reduced to sea level: a later
                     * correction to the site altitude re-reduces the whole
                     * history correctly. */
                    snprintf(val, sizeof(val), "%.3f",
                             climate_to_sea_level(p.press_mhpa / 1000.0f));
                    break;
                default:
                    snprintf(val, sizeof(val), "%.1f", p.lux);
                    break;
                }
            }
            jbuf_printf(&j, "%s%s", i ? "," : "", val);

            /* Room to spare for the next value, so no append is cut in half. */
            if (j.len > sizeof(buf) - 32) {
                if (httpd_resp_send_chunk(req, j.buf, j.len) != ESP_OK) {
                    return ESP_FAIL;
                }
                jbuf_init(&j, buf, sizeof(buf));
            }
        }
        jbuf_printf(&j, "]");
    }
    jbuf_printf(&j, "}");
    httpd_resp_send_chunk(req, j.buf, j.len);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t history_reset_post_handler(httpd_req_t *req)
{
    history_reset();
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    static wifi_scan_ap_t aps[15];
    int n = wifi_scan(aps, 15);
    if (n < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "scan failed");
    }

    /* Fifteen APs with fully escaped SSIDs come to ~2 KB. */
    static char json[2560];
    char essid[67];
    jbuf_t j;
    jbuf_init(&j, json, sizeof(json));
    jbuf_printf(&j, "[");
    for (int i = 0; i < n; i++) {
        jbuf_printf(&j, "%s{\"ssid\":\"%s\",\"bssid\":\"" MACSTR "\","
                        "\"ch\":%d,\"rssi\":%d,\"auth\":\"%s\"}",
                    i ? "," : "",
                    json_escape(aps[i].ssid, essid, sizeof(essid)),
                    MAC2STR(aps[i].bssid),
                    aps[i].channel,
                    aps[i].rssi,
                    wifi_authmode_str(aps[i].authmode));
    }
    jbuf_printf(&j, "]");

    return jbuf_send(req, &j);
}

static esp_err_t networks_get_handler(httpd_req_t *req)
{
    static char json[1024];
    char essid[67];
    wifi_cred_t net;
    jbuf_t j;
    jbuf_init(&j, json, sizeof(json));
    jbuf_printf(&j, "[");
    for (int i = 0; wifi_store_get(i, &net); i++) {
        bool pinned = false;
        for (int k = 0; k < 6; k++) {
            if (net.bssid[k]) {
                pinned = true;
                break;
            }
        }
        jbuf_printf(&j, "%s{\"ssid\":\"%s\"",
                    i ? "," : "", json_escape(net.ssid, essid, sizeof(essid)));
        if (pinned) {
            jbuf_printf(&j, ",\"bssid\":\"" MACSTR "\"", MAC2STR(net.bssid));
        }
        jbuf_printf(&j, "}");
    }
    jbuf_printf(&j, "]");

    return jbuf_send(req, &j);
}

/* Reads the request body and parses JSON; NULL on error. */
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

static esp_err_t network_delete_handler(httpd_req_t *req)
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
    /* Ten locations whose names escape to twice their stored length reach
     * ~1.1 KB, so the buffer is sized for the full list rather than for the
     * handful a station usually has. */
    static char json[1536];
    char name[100];
    jbuf_t j;
    jbuf_init(&j, json, sizeof(json));
    jbuf_printf(&j, "{\"active\":%d,\"locations\":[", weather_store_get_active());
    weather_location_t loc;
    for (int i = 0; weather_store_get(i, &loc); i++) {
        jbuf_printf(&j, "%s{\"name\":\"%s\",\"lat\":%.4f,\"lon\":%.4f}",
                    i ? "," : "", json_escape(loc.name, name, sizeof(name)),
                    loc.lat, loc.lon);
    }
    jbuf_printf(&j, "]}");

    return jbuf_send(req, &j);
}

/* The browser resolves a place name to coordinates (Open-Meteo geocoding) and
 * posts them already numeric. */
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

    /* RGB hex string ("00AAFF", a leading "#" is tolerated). */
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
    /* Metres above sea level — the only input to the pressure reduction. */
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
    /* Reply goes out before the mode switch, or the client never gets it. */
    esp_err_t ret = httpd_resp_sendstr(req, "{\"ok\":true}");
    wifi_reconnect();
    return ret;
}

/* Every route in one place; the config below takes its handler count from
 * this table. */
static const struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
} s_routes[] = {
    { "/",                     HTTP_GET,    index_get_handler },
    { "/api/status",           HTTP_GET,    status_get_handler },
    { "/api/history",          HTTP_GET,    history_get_handler },
    { "/api/history/reset",    HTTP_POST,   history_reset_post_handler },
    { "/api/scan",             HTTP_GET,    scan_get_handler },
    { "/api/networks",         HTTP_GET,    networks_get_handler },
    { "/api/networks",         HTTP_POST,   network_add_post_handler },
    { "/api/networks",         HTTP_DELETE, network_delete_handler },
    { "/api/locations",        HTTP_GET,    locations_get_handler },
    { "/api/locations",        HTTP_POST,   location_add_post_handler },
    { "/api/locations",        HTTP_DELETE, location_delete_handler },
    { "/api/locations/active", HTTP_PUT,    location_active_put_handler },
    { "/api/connect",          HTTP_POST,   connect_post_handler },
    { "/api/settings",         HTTP_POST,   settings_post_handler },
    { "/api/scd40/calibrate",  HTTP_POST,   scd40_calibrate_post_handler },
    { "/api/ota",              HTTP_POST,   ota_post_handler },
};

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
    config.stack_size = 8192; /* the default 4 KiB is short for /api/status */
    config.max_uri_handlers = sizeof(s_routes) / sizeof(s_routes[0]);
    /* Recycle the LRU connection: the browser's keep-alive polling would
     * otherwise starve the shared LWIP socket pool of outbound TLS clients. */
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    s_server = server;

    for (int i = 0; i < (int)(sizeof(s_routes) / sizeof(s_routes[0])); i++) {
        const httpd_uri_t uri = {
            .uri = s_routes[i].uri,
            .method = s_routes[i].method,
            .handler = handle_request,
            .user_ctx = (void *)s_routes[i].handler,
        };
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
    }

    ESP_LOGI(TAG, "Web server started: http://%s.local/", MDNS_HOSTNAME);
}
