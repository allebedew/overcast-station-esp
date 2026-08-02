#include "ld2450.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "settings.h"

/* Wiring: module TX -> GPIO10, module RX -> GPIO11, 5 V, GND. The tracking
 * stream itself needs no commands; the TX line carries the one configuration
 * sequence below, which turns the module's Bluetooth off. */
#define UART_PORT    UART_NUM_1
#define UART_RX_GPIO 10
#define UART_TX_GPIO 11
#define UART_BAUD    256000
#define UART_RX_BUF  512 /* must exceed the 128-byte hardware FIFO */

/* 30 bytes: header, three 8-byte targets, tail. One every ~100 ms. */
#define FRAME_LEN 30
static const uint8_t FRAME_HEAD[4] = { 0xAA, 0xFF, 0x03, 0x00 };
static const uint8_t FRAME_TAIL[2] = { 0x55, 0xCC };

/* Configuration is a request/response protocol on the same UART, framed
 * unlike the data stream: FD FC FB FA, the payload length, the command word,
 * its value, then 04 03 02 01. The reply repeats the command word with 0x0100
 * set and carries a status word, 0 for success. The module stops streaming
 * between "enable" and "end", so nothing here may run once the reader loop
 * has started. */
static const uint8_t CMD_HEAD[4] = { 0xFD, 0xFC, 0xFB, 0xFA };
static const uint8_t CMD_TAIL[4] = { 0x04, 0x03, 0x02, 0x01 };
#define CMD_ENABLE    0x00FF
#define CMD_END       0x00FE
#define CMD_RESTART   0x00A3
#define CMD_BLUETOOTH 0x00A4
#define CMD_ACK_MS    500

/* Set once the module has confirmed Bluetooth off. The module keeps the
 * setting in its own flash, so the work is done for good; the flag is what
 * spares every later boot the write and the restart that follows it. Replacing
 * the module means clearing it by hand — there is no way to ask an LD2450
 * whether its Bluetooth is on. */
#define NVS_BT_KEY "radar_bt_off"

#define OFFLINE_MS       2000
#define PRESENCE_HOLD_MS 5000 /* see ld2450_data_t.presence */

static const char *TAG = "radar";

/* Written by the reader task, read by httpd. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static ld2450_data_t s_data;
static int64_t s_frame_us;  /* last frame of any kind, 0 = none yet */
static int64_t s_target_us; /* last frame with a target in it */

/* Not two's complement: bit 15 is the sign and 1 means positive. */
static int16_t decode_signed(const uint8_t *p)
{
    int16_t v = (int16_t)(((p[1] & 0x7F) << 8) | p[0]);
    return (p[1] & 0x80) ? v : (int16_t)-v;
}

static void parse_frame(const uint8_t *f)
{
    ld2450_data_t d = { 0 };

    for (int i = 0; i < LD2450_MAX_TARGETS; i++) {
        const uint8_t *t = f + 4 + i * 8;
        int16_t x = decode_signed(t);
        int16_t y = decode_signed(t + 2);
        int16_t speed = decode_signed(t + 4);
        if (x == 0 && y == 0 && speed == 0) {
            continue; /* an empty slot is all zeroes, and slots are not packed */
        }

        ld2450_target_t *o = &d.targets[d.count++];
        o->x_mm = x;
        o->y_mm = y;
        o->speed_cms = speed;
        o->res_mm = (uint16_t)((t[7] << 8) | t[6]);
        o->dist_m = sqrtf((float)x * x + (float)y * y) / 1000.0f;

        if (d.count == 1 || o->dist_m < d.nearest_m) {
            d.nearest_m = o->dist_m;
        }
    }

    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    s_data = d;
    s_frame_us = now;
    if (d.count > 0) {
        s_target_us = now;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static esp_err_t send_cmd(uint16_t word, const uint8_t *val, size_t vlen)
{
    uint8_t f[16];
    size_t n = 0;
    memcpy(f, CMD_HEAD, sizeof(CMD_HEAD));
    n += sizeof(CMD_HEAD);
    uint16_t plen = (uint16_t)(2 + vlen);
    f[n++] = plen & 0xFF;
    f[n++] = plen >> 8;
    f[n++] = word & 0xFF;
    f[n++] = word >> 8;
    if (vlen) {
        memcpy(f + n, val, vlen);
        n += vlen;
    }
    memcpy(f + n, CMD_TAIL, sizeof(CMD_TAIL));
    n += sizeof(CMD_TAIL);

    uart_flush_input(UART_PORT);
    if (uart_write_bytes(UART_PORT, f, n) != (int)n) {
        return ESP_FAIL;
    }

    /* Data frames already in flight arrive before the reply, so the reply is
     * searched for rather than expected at the head of the buffer. */
    uint8_t rx[128];
    size_t len = 0;
    int64_t deadline = esp_timer_get_time() + CMD_ACK_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        int r = uart_read_bytes(UART_PORT, rx + len, sizeof(rx) - len,
                                pdMS_TO_TICKS(50));
        if (r > 0) {
            len += r;
        }
        for (size_t i = 0; i + 10 <= len; i++) {
            if (memcmp(rx + i, CMD_HEAD, sizeof(CMD_HEAD)) != 0) {
                continue;
            }
            uint16_t ack = rx[i + 6] | (rx[i + 7] << 8);
            uint16_t status = rx[i + 8] | (rx[i + 9] << 8);
            if (ack == (word | 0x0100)) {
                return status == 0 ? ESP_OK : ESP_FAIL;
            }
        }
        if (len == sizeof(rx)) {
            len = 0; /* no reply in a full buffer: start over */
        }
    }
    return ESP_ERR_TIMEOUT;
}

/* Bluetooth is on out of the factory, and anyone in range can pull the same
 * target stream with the vendor's app. Switched off once, on the first boot
 * that reaches the module; the setting takes effect on the restart that
 * follows. */
static void disable_bluetooth(void)
{
    static const uint8_t on[2] = { 0x01, 0x00 };
    static const uint8_t off[2] = { 0x00, 0x00 };

    if (settings_get_u8(NVS_BT_KEY, 0)) {
        return;
    }

    if (send_cmd(CMD_ENABLE, on, sizeof(on)) != ESP_OK) {
        ESP_LOGW(TAG, "no reply to the config command — Bluetooth left as it is");
        return;
    }
    esp_err_t err = send_cmd(CMD_BLUETOOTH, off, sizeof(off));
    if (err == ESP_OK) {
        err = send_cmd(CMD_RESTART, NULL, 0);
    }
    if (err != ESP_OK) {
        /* Leaving it in configuration mode would leave it mute, and only a
         * power cycle would bring the stream back. */
        send_cmd(CMD_END, NULL, 0);
        ESP_LOGW(TAG, "could not switch Bluetooth off: %s", esp_err_to_name(err));
        return;
    }
    settings_set_u8(NVS_BT_KEY, 1);
    ESP_LOGI(TAG, "Bluetooth off, module restarting");
}

static void ld2450_task(void *arg)
{
    static uint8_t buf[FRAME_LEN * 4];
    size_t len = 0;
    bool online = false, silent_logged = false;

    /* Before the reader loop: the module is mute while it is configured, and
     * the restart it ends with must not be read as a dead stream. */
    disable_bluetooth();
    const int64_t start_us = esp_timer_get_time();

    for (;;) {
        int n = uart_read_bytes(UART_PORT, buf + len, sizeof(buf) - len,
                                pdMS_TO_TICKS(200));
        if (n > 0) {
            len += n;

            /* Framing is by header and tail alone — there is no length field
             * and no checksum, so a boundary is only ever a guess and resync is
             * a byte at a time. */
            size_t i = 0;
            while (len - i >= FRAME_LEN) {
                if (memcmp(buf + i, FRAME_HEAD, sizeof(FRAME_HEAD)) != 0 ||
                    memcmp(buf + i + FRAME_LEN - sizeof(FRAME_TAIL), FRAME_TAIL,
                           sizeof(FRAME_TAIL)) != 0) {
                    i++;
                    continue;
                }
                parse_frame(buf + i);
                i += FRAME_LEN;
            }

            len -= i; /* what is left may be the head of the next frame */
            memmove(buf, buf + i, len);
        }

        /* s_frame_us is written by this task alone, so it is read here without
         * the lock. Before the first frame the clock runs from startup, which
         * is what turns a miswired module into a warning rather than silence. */
        int64_t now = esp_timer_get_time();
        if (now - (s_frame_us ? s_frame_us : start_us) > OFFLINE_MS * 1000) {
            if (!silent_logged) {
                ESP_LOGW(TAG, "no frames on GPIO%d for %d ms — check the wiring "
                              "and that the module runs at %d baud",
                         UART_RX_GPIO, OFFLINE_MS, UART_BAUD);
                silent_logged = true;
                online = false;
            }
        } else if (!online) {
            online = true;
            silent_logged = false;
            ESP_LOGI(TAG, "frames arriving");
        }
    }
}

bool ld2450_get(ld2450_data_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    ld2450_data_t d = s_data;
    int64_t frame_us = s_frame_us;
    int64_t target_us = s_target_us;
    taskEXIT_CRITICAL(&s_lock);

    if (frame_us == 0) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    int32_t age_ms = (int32_t)((now - frame_us) / 1000);
    if (age_ms > OFFLINE_MS) {
        return false;
    }

    d.age_ms = age_ms;
    d.presence = target_us != 0 &&
                 (now - target_us) / 1000 <= PRESENCE_HOLD_MS;
    *out = d;
    return true;
}

void ld2450_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        /* XTAL rather than the default APB: the baud rate then survives any
         * later CPU frequency scaling. */
        .source_clk = UART_SCLK_XTAL,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_GPIO, UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(ld2450_task, "radar", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "LD2450 on UART%d, RX GPIO%d / TX GPIO%d, %d baud", UART_PORT,
             UART_RX_GPIO, UART_TX_GPIO, UART_BAUD);
}
