#include "timesync.h"

#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

static const char *TAG = "timesync";

/* Set once from the SNTP callback, read from other tasks; a bool
 * read/write is atomic here, no locking needed. */
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

void timesync_init(void)
{
    /* clock runs in UTC; the client keeps retrying until the network is up */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = on_time_sync;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&cfg));
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
