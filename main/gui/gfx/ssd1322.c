#include "ssd1322.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"

#include "gfx_target.h"
#include "gfx_text.h"

static const char *TAG = "ssd1322";

#define PIN_SCLK 6 /* IOMUX FSPICLK */
#define PIN_SDIN 7 /* IOMUX FSPID */
#define PIN_DC   4
#define PIN_RES  5

/* /CS is tied to ground on the panel — the display is alone on the bus, so the
 * peripheral drives no chip select at all. The cost is that nothing re-syncs
 * the controller's bit counter, which is why a reset pulse is part of init. */
#define SPI_HOST_ID SPI2_HOST
#define SPI_HZ      8000000

/* The 256-pixel panel sits in the middle of the 480-pixel driver, and one
 * column address covers 4 pixels = 2 bytes of RAM. */
#define COL_FIRST 0x1C
#define COL_LAST  0x5B
#define ROW_LAST  0x3F

#define ROW_BYTES (GFX_H / 2) /* 128: one panel row, two pixels per byte */

#define CMD_COL_ADDR   0x15
#define CMD_WRITE_RAM  0x5C
#define CMD_ROW_ADDR   0x75
#define CMD_MODE_ALLON 0xA5
#define CMD_MODE_NORM  0xA6

static spi_device_handle_t s_dev;

/* D/C is not a bus signal, so it is set from the transaction's own user field
 * right before the peripheral starts clocking. */
static void dc_pre_cb(spi_transaction_t *t)
{
    gpio_set_level(PIN_DC, (int)(intptr_t)t->user);
}

static void tx(const uint8_t *p, size_t n, int dc)
{
    spi_transaction_t t = {
        .length = n * 8,
        .user   = (void *)(intptr_t)dc,
    };
    if (n <= 4) {
        t.flags = SPI_TRANS_USE_TXDATA;
        memcpy(t.tx_data, p, n);
    } else {
        t.tx_buffer = p;
    }
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_dev, &t));
}

static void cmd(uint8_t c)
{
    tx(&c, 1, 0);
}

static void cmd_args(uint8_t c, const uint8_t *args, size_t n)
{
    cmd(c);
    if (n) {
        tx(args, n, 1);
    }
}

/* Newhaven's sequence from the module datasheet, flattened to <command, count,
 * args...>. Two values are ours to revisit: the re-map byte decides which
 * physical edge is the top of the portrait screen, and contrast sets how hard
 * the panel is driven. */
static const uint8_t INIT_SCRIPT[] = {
    0xFD, 1, 0x12,             /* unlock the basic command set */
    0xAE, 0,                   /* display off while it is configured */
    0x15, 2, COL_FIRST, COL_LAST,
    0x75, 2, 0x00, ROW_LAST,
    0xB3, 1, 0x91,             /* clock: 80 frames/s */
    0xCA, 1, 0x3F,             /* 1/64 duty */
    0xA2, 1, 0x00,             /* display offset */
    0xA1, 1, 0x00,             /* start line */
    /* Re-map: horizontal increment, dual COM. COM scan runs from COM0, the one
     * bit that differs from the datasheet's sequence — it is the canvas' x axis
     * once the panel is mounted rotated, and the other way round it mirrors. */
    0xA0, 2, 0x04, 0x11,
    0xB5, 1, 0x00,             /* GPIO inputs disabled */
    0xAB, 1, 0x01,             /* internal VDD regulator */
    0xB4, 2, 0xA0, 0xFD,       /* external VSL, present on this module */
    0xC1, 1, 0x9F,             /* contrast */
    0xC7, 1, 0x0F,             /* master current */
    0xB9, 0,                   /* default linear grayscale table */
    0xB1, 1, 0xE2,             /* phase 1 = 5 clocks, phase 2 = 14 */
    0xD1, 2, 0xA2, 0x20,       /* enhanced driving scheme */
    0xBB, 1, 0x1F,             /* pre-charge voltage 0.60 * VCC */
    0xB6, 1, 0x08,             /* second pre-charge period */
    0xBE, 1, 0x07,             /* VCOMH 0.86 * VCC */
    CMD_MODE_NORM, 0,
    0xA9, 0,                   /* partial display off */
    0xAF, 0,                   /* display on */
};

/* Panel rows are the canvas' x axis; the column range is always the full width,
 * so the 4-pixel column granularity never reaches the caller. */
static void set_window(int row_first, int row_last)
{
    const uint8_t cols[] = { COL_FIRST, COL_LAST };
    const uint8_t rows[] = { (uint8_t)row_first, (uint8_t)row_last };

    cmd_args(CMD_COL_ADDR, cols, sizeof(cols));
    cmd_args(CMD_ROW_ADDR, rows, sizeof(rows));
    cmd(CMD_WRITE_RAM);
}

void ssd1322_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RES),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(PIN_RES, 1);

    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_SDIN,
        .miso_io_num     = -1, /* the controller cannot be read over SPI */
        .sclk_io_num     = PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = GFX_W * ROW_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_ID, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_HZ,
        .mode           = 0,
        .spics_io_num   = -1,
        .queue_size     = 1,
        .pre_cb         = dc_pre_cb,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI_HOST_ID, &dev, &s_dev));

    /* Reset only once the bus is up: a half-configured SCLK can shift the
     * controller's bit counter, and this pulse is the only way back. */
    gpio_set_level(PIN_RES, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    for (size_t i = 0; i < sizeof(INIT_SCRIPT);) {
        uint8_t c = INIT_SCRIPT[i];
        uint8_t n = INIT_SCRIPT[i + 1];
        cmd_args(c, &INIT_SCRIPT[i + 2], n);
        i += 2 + n;
    }

    ESP_LOGI(TAG, "panel up on SPI%d, %d MHz", SPI_HOST_ID + 1, SPI_HZ / 1000000);
}

/* Asymmetric on both axes, so it settles orientation as well as mirroring: the
 * block sits at the canvas origin and the ramp runs from black at the top to
 * full at the bottom. */
static void orientation_scene(gfx_canvas_t *c)
{
    gfx_init(c);

    gfx_rect(c, (gfx_rect_t){ 0, 0, GFX_W, GFX_H }, GFX_DIM, GFX_NONE, GFX_SOLID);
    gfx_rect(c, (gfx_rect_t){ 2, 2, 8, 8 }, GFX_NONE, GFX_FULL, GFX_SOLID);

    const gfx_text_style_t st = { u8g2_font_04b_03_tr, GFX_FULL, GFX_LEFT };
    gfx_text(c, 14, 10, &st, "TOP");

    for (int i = 0; i < 16; i++) {
        gfx_rect(c, (gfx_rect_t){ 40, (int16_t)(20 + i * 8), 20, 8 },
                 GFX_NONE, (gfx_level_t)i, GFX_SOLID);
    }
}

void ssd1322_selftest(int pattern)
{
    /* One row at a time: a full-screen constant needs no 8 KB buffer of its own,
     * and RAM is written in the same order gfx_present() uses. */
    static const uint8_t row[ROW_BYTES] = { [0 ... ROW_BYTES - 1] = 0xFF };

    /* Static: 8 KB is far past what a task stack here can hold. */
    static gfx_canvas_t canvas;

    switch (pattern) {
    case 0:
        cmd(CMD_MODE_ALLON);
        break;
    case 1:
        cmd(CMD_MODE_NORM);
        set_window(0, GFX_W - 1);
        for (int i = 0; i < GFX_W; i++) {
            tx(row, sizeof(row), 1);
        }
        break;
    default:
        cmd(CMD_MODE_NORM);
        orientation_scene(&canvas);
        gfx_present(&canvas);
        break;
    }
}

void gfx_present(const gfx_canvas_t *c)
{
    gfx_present_cols(c, 0, GFX_W - 1);
}

void gfx_present_cols(const gfx_canvas_t *c, int x0, int x1)
{
    if (x0 < 0)          { x0 = 0; }
    if (x1 > GFX_W - 1)  { x1 = GFX_W - 1; }
    if (x0 > x1)         { return; }

    set_window(x0, x1);
    tx(c->buf[x0], (size_t)(x1 - x0 + 1) * ROW_BYTES, 1);
}
