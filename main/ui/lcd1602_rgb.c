#include "lcd1602_rgb.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "i2c_bus.h"

#define LCD_ADDR       0x3E /* AIP31068L, fixed */
#define I2C_SPEED_HZ   100000
#define I2C_TIMEOUT_MS 100

/* Every transfer starts with a control byte: bit 7 clear means "the rest of
 * this transfer is payload of the same kind", bit 6 selects the HD44780 RS
 * line (0 = command, 1 = data). So a whole row goes out in one transfer. */
#define CTRL_CMD  0x80
#define CTRL_DATA 0x40

/* HD44780 command set, the subset we need. */
#define CMD_CLEAR      0x01
#define CMD_ENTRY_MODE 0x06 /* advance cursor, do not shift the display */
#define CMD_DISPLAY_ON 0x0C /* display on, cursor off, blink off */
#define CMD_FUNCTION   0x28 /* 4-bit interface, 2 lines, 5x8 font */
#define CMD_SET_DDRAM  0x80
#define DDRAM_ROW1     0x40 /* second row starts here, rows are not contiguous */

/* The character ROM holds katakana and symbols above ASCII; of those two are
 * worth passing through, written as "\xDF" and "\xFF" in a string — the degree
 * sign, and the solid block at the very end of the table, which is the only
 * fully-lit cell the ROM has and so the only way to draw a bar without
 * defining custom characters. */
#define DEGREE_SIGN 0xDF
#define FULL_BLOCK  0xFF

#define CLEAR_DELAY_MS 2 /* clear/home are the slow commands (~1.5 ms) */
#define RETRY_PERIOD_MS 5000 /* how often to look for an absent module */

/* The backlight driver differs between board revisions: four chips are in the
 * wild, each with its own address, PWM register per channel and wake-up
 * sequence (values taken from DFRobot_RGBLCD1602). We probe for all of them,
 * which also covers boards that ship with a chip we have not seen yet — those
 * simply end up without color. None of these collide with the SCD40 at 0x62. */
#define BL_INIT_MAX 5

typedef struct {
    uint8_t addr;
    uint8_t reg_r, reg_g, reg_b;
    uint8_t init[BL_INIT_MAX][2]; /* register/value pairs */
    int init_len;
} backlight_t;

static const backlight_t s_backlights[] = {
    /* PCA9633: leave sleep, all outputs under PWM control */
    { 0x60, 0x04, 0x03, 0x02, {{0x00, 0x00}, {0x08, 0xFF}, {0x01, 0x20}}, 3 },
    { 0x30, 0x06, 0x07, 0x08, {{0x01, 0x00}, {0x02, 0xFF}, {0x04, 0x15}}, 3 },
    { 0x6B, 0x06, 0x05, 0x04,
      {{0x2F, 0x00}, {0x00, 0x20}, {0x01, 0x00}, {0x02, 0x01}, {0x03, 0x04}}, 5 },
    { 0x2D, 0x01, 0x02, 0x03, {{0}}, 0 },
};

static const char *TAG = "lcd1602";

static i2c_master_dev_handle_t s_lcd;
static i2c_master_dev_handle_t s_bl;
static const backlight_t *s_bl_type;

static bool s_present;
static int64_t s_retry_at_us;

/* What the panel is currently showing, so unchanged content costs no bus
 * traffic. Invalidated on every (re)attach to force a full repaint. */
static char s_shadow[LCD1602_ROWS][LCD1602_COLS];
static uint8_t s_color[3] = { 255, 255, 255 };
static bool s_color_valid;

/* Every transfer takes the bus lock; the sequences that must not be broken up
 * take it again around the whole group, which the recursive lock allows. The
 * waits below are deliberately left outside it — they are the controller's
 * settling time, not the bus's. */
static esp_err_t lcd_write(uint8_t ctrl, const uint8_t *payload, int len)
{
    uint8_t buf[1 + LCD1602_COLS];
    buf[0] = ctrl;
    memcpy(&buf[1], payload, len);

    i2c_bus_lock();
    esp_err_t err = i2c_master_transmit(s_lcd, buf, 1 + len, I2C_TIMEOUT_MS);
    i2c_bus_unlock();
    return err;
}

static esp_err_t lcd_cmd(uint8_t cmd)
{
    return lcd_write(CTRL_CMD, &cmd, 1);
}

static esp_err_t bl_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };

    i2c_bus_lock();
    esp_err_t err = i2c_master_transmit(s_bl, buf, sizeof(buf), I2C_TIMEOUT_MS);
    i2c_bus_unlock();
    return err;
}

static bool probe(uint8_t addr)
{
    i2c_bus_lock();
    bool present = i2c_master_probe(i2c_bus_handle(), addr, I2C_TIMEOUT_MS) == ESP_OK;
    i2c_bus_unlock();
    return present;
}

/* Marks the module offline and schedules the next detection attempt. */
static void fail(esp_err_t err)
{
    if (s_present) {
        ESP_LOGW(TAG, "display not responding (%s), will re-probe",
                 esp_err_to_name(err));
    }
    s_present = false;
    s_retry_at_us = esp_timer_get_time() + RETRY_PERIOD_MS * 1000LL;
}

/* Power-on sequence from the AIP31068L datasheet: the function set has to be
 * repeated three times before the controller is guaranteed to be listening. */
static esp_err_t lcd_reset(void)
{
    vTaskDelay(pdMS_TO_TICKS(50)); /* ≥40 ms after power-up */

    esp_err_t err = lcd_cmd(CMD_FUNCTION);
    vTaskDelay(pdMS_TO_TICKS(5));
    if (err == ESP_OK) {
        err = lcd_cmd(CMD_FUNCTION);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (err == ESP_OK) {
        err = lcd_cmd(CMD_FUNCTION);
    }
    if (err == ESP_OK) {
        err = lcd_cmd(CMD_DISPLAY_ON);
    }
    if (err == ESP_OK) {
        err = lcd_cmd(CMD_CLEAR);
        vTaskDelay(pdMS_TO_TICKS(CLEAR_DELAY_MS));
    }
    if (err == ESP_OK) {
        err = lcd_cmd(CMD_ENTRY_MODE);
    }
    return err;
}

/* Finds the backlight chip and wakes it up. Failure is not fatal: a display
 * with an unknown backlight driver still shows text. */
static void backlight_attach(void)
{
    if (s_bl == NULL) {
        for (int i = 0; i < sizeof(s_backlights) / sizeof(s_backlights[0]); i++) {
            const backlight_t *bl = &s_backlights[i];
            if (!probe(bl->addr)) {
                continue;
            }
            i2c_device_config_t cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = bl->addr,
                .scl_speed_hz = I2C_SPEED_HZ,
            };
            if (i2c_master_bus_add_device(i2c_bus_handle(), &cfg, &s_bl) != ESP_OK) {
                continue;
            }
            s_bl_type = bl;
            ESP_LOGI(TAG, "RGB backlight at 0x%02X", bl->addr);
            break;
        }
    }
    if (s_bl_type == NULL) {
        ESP_LOGW(TAG, "no known RGB backlight driver found, text only");
        return;
    }
    for (int i = 0; i < s_bl_type->init_len; i++) {
        bl_reg(s_bl_type->init[i][0], s_bl_type->init[i][1]);
    }
    s_color_valid = false; /* the chip lost its PWM values across the reset */
}

/* Probes the module and brings it into a known state. */
static bool attach(void)
{
    if (!probe(LCD_ADDR)) {
        return false;
    }
    if (lcd_reset() != ESP_OK) {
        return false;
    }
    backlight_attach();

    memset(s_shadow, 0, sizeof(s_shadow)); /* NUL never matches padded text */
    s_present = true;
    ESP_LOGI(TAG, "display ready at 0x%02X, %dx%d",
             LCD_ADDR, LCD1602_COLS, LCD1602_ROWS);
    return true;
}

/* True when the module can be talked to; retries detection at most once every
 * RETRY_PERIOD_MS so a display connected later comes up by itself. */
static bool online(void)
{
    if (s_present) {
        return true;
    }
    if (s_lcd == NULL) {
        return false; /* lcd1602_rgb_init() never got a bus */
    }
    if (esp_timer_get_time() < s_retry_at_us) {
        return false;
    }
    if (!attach()) {
        s_retry_at_us = esp_timer_get_time() + RETRY_PERIOD_MS * 1000LL;
        return false;
    }
    return true;
}

esp_err_t lcd1602_rgb_init(void)
{
    if (i2c_bus_handle() == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LCD_ADDR,
        .scl_speed_hz = I2C_SPEED_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(i2c_bus_handle(), &cfg, &s_lcd);
    if (err != ESP_OK) {
        return err;
    }

    if (!attach()) {
        ESP_LOGW(TAG, "no display at 0x%02X, will re-probe every %d s",
                 LCD_ADDR, RETRY_PERIOD_MS / 1000);
        s_retry_at_us = esp_timer_get_time() + RETRY_PERIOD_MS * 1000LL;
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

bool lcd1602_rgb_present(void)
{
    return s_present;
}

esp_err_t lcd1602_rgb_set_line(int row, const char *text)
{
    if (row < 0 || row >= LCD1602_ROWS || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!online()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Pad to the full width so leftovers from a longer previous line are
     * overwritten, and keep to the printable ASCII the character ROM has. */
    char line[LCD1602_COLS];
    int len = 0;
    while (len < LCD1602_COLS && text[len] != '\0') {
        unsigned char c = (unsigned char)text[len];
        bool ok = (c >= 0x20 && c <= 0x7E) || c == DEGREE_SIGN ||
                  c == FULL_BLOCK;
        line[len] = ok ? c : '?';
        len++;
    }
    memset(&line[len], ' ', LCD1602_COLS - len);

    if (memcmp(s_shadow[row], line, LCD1602_COLS) == 0) {
        return ESP_OK;
    }

    /* The cursor address and the characters that follow it are one operation:
     * anything else on the bus in between would land the row somewhere else. */
    i2c_bus_lock();
    esp_err_t err = lcd_cmd(CMD_SET_DDRAM | (row == 0 ? 0 : DDRAM_ROW1));
    if (err == ESP_OK) {
        err = lcd_write(CTRL_DATA, (const uint8_t *)line, LCD1602_COLS);
    }
    i2c_bus_unlock();

    if (err != ESP_OK) {
        fail(err);
        return err;
    }
    memcpy(s_shadow[row], line, LCD1602_COLS);
    return ESP_OK;
}

esp_err_t lcd1602_rgb_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!online()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_bl_type == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_color_valid && s_color[0] == r && s_color[1] == g && s_color[2] == b) {
        return ESP_OK;
    }

    esp_err_t err = bl_reg(s_bl_type->reg_r, r);
    if (err == ESP_OK) {
        err = bl_reg(s_bl_type->reg_g, g);
    }
    if (err == ESP_OK) {
        err = bl_reg(s_bl_type->reg_b, b);
    }
    if (err != ESP_OK) {
        fail(err);
        return err;
    }
    s_color[0] = r;
    s_color[1] = g;
    s_color[2] = b;
    s_color_valid = true;
    return ESP_OK;
}

esp_err_t lcd1602_rgb_clear(void)
{
    if (!online()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = lcd_cmd(CMD_CLEAR);
    if (err != ESP_OK) {
        fail(err);
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(CLEAR_DELAY_MS));
    memset(s_shadow, ' ', sizeof(s_shadow));
    return ESP_OK;
}
