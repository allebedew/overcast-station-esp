#include "timesync.h"

#include <stdio.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

static const char *TAG = "timesync";

/* The RTC timer keeps running across a software reset (OTA, panic, watchdog,
 * reset button) and only a power cycle takes it back to 1970. SNTP is the sole
 * writer of the clock, so a reading past this date can only have come from a
 * sync in an earlier session and is trusted as one. */
#define CLOCK_SANE_EPOCH 1767225600 /* 2026-01-01 UTC */

/* Set from the SNTP callback, read elsewhere; a bool access is atomic here. */
static volatile bool s_synced;

static void on_time_sync(struct timeval *tv)
{
    s_synced = true;

    struct tm utc;
    char time_str[32];
    gmtime_r(&tv->tv_sec, &utc);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &utc);
    ESP_LOGI(TAG, "Time synchronized (SNTP): %s UTC", time_str);
}

/* Requesting before the link is up costs the whole lwIP backoff (15 s doubling
 * to 150 s), so the client only ever asks with an address in hand. A restart
 * also drops the backoff a failed run accumulated. */
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    static bool started;
    if (!started) {
        ESP_ERROR_CHECK(esp_netif_sntp_start());
        started = true;
    } else {
        esp_sntp_restart();
    }
}

void timesync_init(void)
{
    time_t now = time(NULL);
    if (now > CLOCK_SANE_EPOCH) {
        s_synced = true;
        struct tm utc;
        char time_str[32];
        gmtime_r(&now, &utc);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &utc);
        ESP_LOGI(TAG, "Clock survived the reset: %s UTC", time_str);
    }

    /* clock runs in UTC; started from the IP event, not here */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start = false;
    cfg.sync_cb = on_time_sync;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &on_got_ip, NULL, NULL));
}

bool timesync_is_synced(void)
{
    return s_synced;
}

void timesync_format(char *buf, size_t len)
{
    if (!s_synced) {
        snprintf(buf, len, "--.--.---- --:--:--");
        return;
    }
    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, len, "%d.%m.%Y %H:%M:%S", &tm);
}
