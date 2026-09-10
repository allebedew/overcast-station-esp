#include "tmp117.h"

#include "esp_log.h"
#include "esp_rom_sys.h"

#include "i2c_dev.h"

#define TMP117_ADDR 0x48 /* ADD0 to GND; 0x49-0x4B with it on V+ / SDA / SCL */

#define TMP117_REG_TEMP      0x00
#define TMP117_REG_CONFIG    0x01
#define TMP117_REG_OFFSET    0x07
#define TMP117_REG_DEVICE_ID 0x0F

/* Device ID register: bits 11:0 are the part number, bits 15:12 the revision. */
#define TMP117_ID_MASK 0x0FFF
#define TMP117_ID      0x0117

/* Config fields: MOD bits 11:10, CONV bits 9:7, AVG bits 6:5.
 *
 * Continuous, 8 averaged samples, 125 ms cycle — the fastest this averaging
 * allows, and two results per 4 Hz poll whatever jitter the scheduler adds.
 * The averaging cuts noise about threefold, which is what stops the display's
 * second decimal dancing; a faster cycle would have to give it up.
 *
 * Written explicitly so a warm restart cannot leave the sensor shut down. */
#define TMP117_MOD_CONTINUOUS (0x0 << 10)
#define TMP117_CONV_125MS     (0x1 << 7)
#define TMP117_AVG_8          (0x1 << 5)
#define TMP117_CONFIG \
    (TMP117_MOD_CONTINUOUS | TMP117_CONV_125MS | TMP117_AVG_8)

#define TMP117_CONFIG_RESET 0x0002 /* soft reset bit */
#define TMP117_CONFIG_DRDY  0x2000 /* set once a conversion has completed */
/* Datasheet: 2 ms. Busy-waited — a vTaskDelay() of a few ms rounds to zero. */
#define TMP117_RESET_US     2500

/* 7.8125 m°C per LSB. */
#define TMP117_LSB_C 0.0078125f

static const char *TAG = "tmp117";

static i2c_master_dev_handle_t s_dev;

/* Where start() gave up, so the warnings below can name the step. */
static const char *s_step = "";

/* The offset register is added to every reading after linearization and is
 * EEPROM-backed, so a value written into it once outlives resets and power
 * cycles — and would look exactly like a miscalibrated part. Factory value is
 * 0; diagnostics only, nothing here writes it. */
static void log_offset(void)
{
    uint16_t raw;
    esp_err_t err = i2c_dev_read_u16be(s_dev, TMP117_REG_OFFSET, &raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "offset register unreadable: %s", esp_err_to_name(err));
        return;
    }
    float off_c = (int16_t)raw * TMP117_LSB_C;
    if (raw == 0) {
        ESP_LOGI(TAG, "temperature offset register: 0 (factory)");
    } else {
        ESP_LOGW(TAG, "temperature offset register: %+.4f C (raw 0x%04X) - "
                      "every reading is shifted by it", off_c, raw);
    }
}

esp_err_t tmp117_start(void)
{
    s_step = "probe";
    if (!i2c_dev_present(TMP117_ADDR)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!s_dev) {
        s_step = "attach";
        esp_err_t err = i2c_dev_attach(&s_dev, TMP117_ADDR);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "0x%02X: setup failed at step \"%s\": %s", TMP117_ADDR,
                     s_step, esp_err_to_name(err));
            return err;
        }
    }

    s_step = "device_id";
    uint16_t id;
    esp_err_t err = i2c_dev_read_u16be(s_dev, TMP117_REG_DEVICE_ID, &id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "0x%02X ACKed the probe but the id read failed: %s",
                 TMP117_ADDR, esp_err_to_name(err));
        return err;
    }
    if ((id & TMP117_ID_MASK) != TMP117_ID) {
        ESP_LOGW(TAG, "0x%02X answers with id 0x%04X, not a TMP117", TMP117_ADDR, id);
        s_step = "id_mismatch";
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_step = "config";
    err = i2c_dev_write_u16be(s_dev, TMP117_REG_CONFIG, TMP117_CONFIG_RESET);
    if (err == ESP_OK) {
        esp_rom_delay_us(TMP117_RESET_US);
        err = i2c_dev_write_u16be(s_dev, TMP117_REG_CONFIG, TMP117_CONFIG);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "0x%02X: setup failed at step \"%s\": %s", TMP117_ADDR,
                 s_step, esp_err_to_name(err));
        return err;
    }

    s_step = "";
    ESP_LOGI(TAG, "TMP117 at 0x%02X, id 0x%03X rev %u", TMP117_ADDR,
             id & TMP117_ID_MASK, id >> 12);
    log_offset();
    return ESP_OK;
}

esp_err_t tmp117_read(tmp117_data_t *out)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Before the first conversion the temperature register reads 0x8000,
     * which would publish as -256 C. */
    uint16_t cfg;
    esp_err_t err = i2c_dev_read_u16be(s_dev, TMP117_REG_CONFIG, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    if (!(cfg & TMP117_CONFIG_DRDY)) {
        return ESP_ERR_NOT_FINISHED;
    }

    uint16_t raw;
    err = i2c_dev_read_u16be(s_dev, TMP117_REG_TEMP, &raw);
    if (err == ESP_OK) {
        out->temp_c = (int16_t)raw * TMP117_LSB_C;
    }
    return err;
}
