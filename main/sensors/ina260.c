#include "ina260.h"

#include "esp_log.h"

#include "i2c_dev.h"

#define INA260_ADDR 0x40 /* A0 = A1 = GND; the pins pick 0x40-0x4F */

#define INA260_REG_CONFIG  0x00
#define INA260_REG_CURRENT 0x01
#define INA260_REG_VOLTAGE 0x02
#define INA260_REG_POWER   0x03
#define INA260_REG_MASK    0x06
#define INA260_REG_MFR_ID  0xFE
#define INA260_REG_DIE_ID  0xFF

#define INA260_MFR_ID      0x5449 /* "TI" */
#define INA260_DIE_ID_MASK 0xFFF0 /* bits 3:0 are the revision */
#define INA260_DIE_ID      0x2270

/* Config fields: reserved bits 14:12 (reset value 110), AVG 11:9, VBUSCT 8:6,
 * ISHCT 5:3, MODE 2:0.
 *
 * 128 averages of a 588 us current + 140 us voltage pair: a result every
 * ~93 ms (datasheet max ~102), with the current sampled 81 % of the time —
 * the battery voltage barely moves, the current follows the radio's bursts. */
#define INA260_RESERVED       (0x6 << 12)
#define INA260_AVG_128        (0x4 << 9)
#define INA260_VBUSCT_140US   (0x0 << 6)
#define INA260_ISHCT_588US    (0x3 << 3)
#define INA260_MODE_CONT_BOTH 0x7
#define INA260_CONFIG                                                  \
    (INA260_RESERVED | INA260_AVG_128 | INA260_VBUSCT_140US |          \
     INA260_ISHCT_588US | INA260_MODE_CONT_BOTH)

/* Conversion ready; reading the mask register clears it. */
#define INA260_MASK_CVRF 0x0008

#define INA260_CURRENT_LSB_MA 1.25f
#define INA260_VOLTAGE_LSB_V  0.00125f
#define INA260_POWER_LSB_MW   10.0f

static const char *TAG = "ina260";

static i2c_master_dev_handle_t s_dev;

esp_err_t ina260_start(void)
{
    if (!i2c_dev_present(INA260_ADDR)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!s_dev) {
        esp_err_t err = i2c_dev_attach(&s_dev, INA260_ADDR);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "0x%02X: setup failed at step \"attach\": %s",
                     INA260_ADDR, esp_err_to_name(err));
            return err;
        }
    }

    uint16_t mfr, die;
    esp_err_t err = i2c_dev_read_u16be(s_dev, INA260_REG_MFR_ID, &mfr);
    if (err == ESP_OK) {
        err = i2c_dev_read_u16be(s_dev, INA260_REG_DIE_ID, &die);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "0x%02X ACKed the probe but the id read failed: %s",
                 INA260_ADDR, esp_err_to_name(err));
        return err;
    }
    if (mfr != INA260_MFR_ID || (die & INA260_DIE_ID_MASK) != INA260_DIE_ID) {
        ESP_LOGW(TAG, "0x%02X answers with ids 0x%04X/0x%04X, not an INA260",
                 INA260_ADDR, mfr, die);
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = i2c_dev_write_u16be(s_dev, INA260_REG_CONFIG, INA260_CONFIG);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "0x%02X: setup failed at step \"config\": %s",
                 INA260_ADDR, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "INA260 at 0x%02X, die rev %u", INA260_ADDR, die & 0xF);
    return ESP_OK;
}

esp_err_t ina260_read(ina260_data_t *out)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    /* The result registers read zero until the first average is in. */
    uint16_t mask;
    esp_err_t err = i2c_dev_read_u16be(s_dev, INA260_REG_MASK, &mask);
    if (err != ESP_OK) {
        return err;
    }
    if (!(mask & INA260_MASK_CVRF)) {
        return ESP_ERR_NOT_FINISHED;
    }

    uint16_t current, voltage, power;
    err = i2c_dev_read_u16be(s_dev, INA260_REG_CURRENT, &current);
    if (err == ESP_OK) {
        err = i2c_dev_read_u16be(s_dev, INA260_REG_VOLTAGE, &voltage);
    }
    if (err == ESP_OK) {
        err = i2c_dev_read_u16be(s_dev, INA260_REG_POWER, &power);
    }
    if (err == ESP_OK) {
        out->current_ma = (int16_t)current * INA260_CURRENT_LSB_MA;
        out->voltage_v = voltage * INA260_VOLTAGE_LSB_V;
        out->power_mw = power * INA260_POWER_LSB_MW;
    }
    return err;
}
