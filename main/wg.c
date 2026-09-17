#include "wg.h"

#include <stdio.h>
#include <string.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

/* The WireGuard block in secrets.h is optional; its private key is the switch. */
#ifdef WG_PRIVATE_KEY
#define WG_ENABLED 1
#endif

#ifdef WG_ENABLED

#include "esp_log.h"
#include "esp_wireguard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "timesync.h"
#include "wifi.h"

/* The interface is added straight to lwip, so its `state` is WireGuard's own.
 * esp_netif reads that field as its own handle unless LWIP_ESP_NETIF_DATA is
 * on, which no Kconfig symbol sets directly -- see sdkconfig.defaults. */
#if !defined(CONFIG_ESP_NETIF_BRIDGE_EN) && !defined(CONFIG_LWIP_PPP_SUPPORT)
#error "WireGuard needs LWIP_ESP_NETIF_DATA: set CONFIG_ESP_NETIF_BRIDGE_EN=y"
#endif

#define WG_POLL_MS 5000
/* The driver retries the handshake on its own; this long without one means the
 * endpoint we resolved is stale (dynamic DNS, a moved server), so tear the
 * interface down and build it again, which resolves the name afresh. */
#define WG_REBUILD_AFTER_MS 180000

static const char *TAG = "wg";

static wireguard_config_t s_config = {
    .private_key = WG_PRIVATE_KEY,
    .listen_port = 0,
    .fw_mark = 0,
    .public_key = WG_PEER_PUBLIC_KEY,
    .preshared_key = NULL,
    .allowed_ip = WG_LOCAL_IP,
    .allowed_ip_mask = WG_LOCAL_NETMASK,
    .endpoint = WG_ENDPOINT,
    .port = WG_ENDPOINT_PORT,
    .persistent_keepalive = WG_KEEPALIVE_S,
};

static wireguard_ctx_t s_ctx;
static volatile bool s_active;
static volatile bool s_up;

static void wg_task(void *arg)
{
    bool started = false; /* the interface exists and a peer is registered */
    TickType_t last_up = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WG_POLL_MS));

        /* The handshake carries a tai64n timestamp the peer checks against its
         * own clock, so the tunnel cannot come up before SNTP has. */
        if (!wifi_is_connected() || !timesync_is_synced()) {
            s_active = false;
            if (started) {
                esp_wireguard_disconnect(&s_ctx);
                started = false;
                s_up = false;
            }
            continue;
        }

        if (!started) {
            if (esp_wireguard_connect(&s_ctx) != ESP_OK) {
                continue; /* endpoint unresolvable; try again next tick */
            }
            started = true;
            last_up = xTaskGetTickCount();
        }
        s_active = true;

        bool up = esp_wireguardif_peer_is_up(&s_ctx) == ESP_OK;
        if (up != s_up) {
            ESP_LOGI(TAG, "peer %s", up ? "up" : "down");
            s_up = up;
        }

        if (up) {
            last_up = xTaskGetTickCount();
        } else if (xTaskGetTickCount() - last_up >
                   pdMS_TO_TICKS(WG_REBUILD_AFTER_MS)) {
            ESP_LOGW(TAG, "no handshake for %d s, rebuilding the interface",
                     WG_REBUILD_AFTER_MS / 1000);
            esp_wireguard_disconnect(&s_ctx);
            started = false;
        }
    }
}

void wg_init(void)
{
    if (esp_wireguard_init(&s_config, &s_ctx) != ESP_OK) {
        ESP_LOGE(TAG, "init failed, no tunnel");
        return;
    }
    xTaskCreate(wg_task, "wg", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "%s:%d, local %s", WG_ENDPOINT, WG_ENDPOINT_PORT, WG_LOCAL_IP);
}

void wg_get_info(wg_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->configured = true;
    out->active = s_active;
    out->up = s_up;
    strlcpy(out->ip, WG_LOCAL_IP, sizeof(out->ip));
    snprintf(out->endpoint, sizeof(out->endpoint), "%s:%d", WG_ENDPOINT,
             WG_ENDPOINT_PORT);
}

#else /* no WireGuard block in secrets.h */

void wg_init(void) {}

void wg_get_info(wg_info_t *out)
{
    memset(out, 0, sizeof(*out));
}

#endif
