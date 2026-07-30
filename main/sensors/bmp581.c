#include "bmp581.h"

#include "esp_log.h"
#include "esp_rom_sys.h"

#include "i2c_dev.h"

#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* SDO picks the address; breakouts usually tie it high (0x47). */
static const uint8_t BMP581_ADDRS[] = { 0x47, 0x46 };

#define BMP581_REG_CHIP_ID    0x01
#define BMP581_REG_REV_ID     0x02
/* Six data registers in one block: temperature XLSB/LSB/MSB at 0x1D-0x1F,
 * then pressure XLSB/LSB/MSB at 0x20-0x22. */
#define BMP581_REG_TEMP_XLSB  0x1D
#define BMP581_REG_STATUS     0x28
#define BMP581_REG_DSP_CONFIG 0x30
#define BMP581_REG_DSP_IIR    0x31
#define BMP581_REG_OSR_CONFIG 0x36
#define BMP581_REG_ODR_CONFIG 0x37
#define BMP581_REG_OSR_EFF    0x38
#define BMP581_REG_CMD        0x7E

#define BMP581_CHIP_ID_581 0x50
#define BMP581_CHIP_ID_580 0x51

#define BMP581_CMD_SOFT_RESET 0xB6

/* The first access after a soft reset gets NACKed: the part is still booting
 * (datasheet: 2 ms), and a reset leaves it in deep standby, where the I2C
 * block is powered down and the access that wakes it is lost. Retrying covers
 * both without having to guess one delay long enough for either.
 *
 * The wait is a busy-wait: the FreeRTOS tick is 10 ms, so vTaskDelay() cannot
 * express anything shorter — pdMS_TO_TICKS() of a few ms rounds down to zero
 * and does not delay at all. */
#define BMP581_WAKE_TRIES   10
#define BMP581_WAKE_WAIT_US 2000

/* STATUS: bit 1 = NVM ready, bit 2 = NVM error. */
#define BMP581_STATUS_NVM_RDY 0x02
#define BMP581_STATUS_NVM_ERR 0x04

/* OSR_CONFIG: osr_t bits 2:0, osr_p bits 5:3, press_en bit 6.
 * x2 on temperature and x16 on pressure — the "standard" indoor setting from
 * the datasheet, ~1 Pa RMS noise, well within the sampling budget below. */
#define BMP581_OSR_T_X2  0x01
#define BMP581_OSR_P_X16 (0x04 << 3)
#define BMP581_PRESS_EN  (1 << 6)
#define BMP581_OSR_CONFIG (BMP581_OSR_T_X2 | BMP581_OSR_P_X16 | BMP581_PRESS_EN)

/* DSP_CONFIG: comp_pt_en bits 1:0, iir_flush_forced_en bit 2, shdw_sel_iir_t
 * bit 3, fifo_sel_iir_t bit 4, shdw_sel_iir_p bit 5, fifo_sel_iir_p bit 6.
 * Compensation stays on (its reset value); shdw_sel_iir_p routes the filtered
 * pressure into the data registers, which is where we read it from — without
 * that bit the filter runs but nothing sees its output.
 *
 * DSP_IIR: set_iir_t bits 2:0, set_iir_p bits 5:3, coefficient codes
 * 0 = bypass, 1 = 1, 2 = 3, 3 = 7, 4 = 15, ... The filter counts samples, not
 * seconds, so the coefficient goes with the ODR below: 15 at 15 Hz gives a
 * time constant near a second — enough to settle the last displayed digit,
 * short enough that a door opening still shows up immediately. Temperature is
 * left unfiltered — it is read from the TMP117 anyway.
 *
 * Both registers are written while the sensor is still in standby; the DSP
 * configuration is not meant to change during normal mode. */
#define BMP581_COMP_PT_EN     0x03
#define BMP581_SHDW_SEL_IIR_P (1 << 5)
#define BMP581_DSP_CONFIG     (BMP581_COMP_PT_EN | BMP581_SHDW_SEL_IIR_P)
#define BMP581_IIR_T_BYPASS   0x00
#define BMP581_IIR_P_COEF_15  (0x04 << 3)
#define BMP581_DSP_IIR        (BMP581_IIR_T_BYPASS | BMP581_IIR_P_COEF_15)

/* ODR_CONFIG: pwr_mode bits 1:0, odr bits 6:2, deep_dis bit 7.
 * Normal mode at 15 Hz (odr code 0x16); deep standby must be disabled or the
 * sensor drops out of normal mode at low ODR.
 *
 * The station reads this sensor at 4 Hz. Running the sensor at exactly that
 * rate would leave the two clocks free-running against each other, so a poll
 * would sometimes fetch the sample it already had and sometimes skip one;
 * nearly four conversions per poll removes the question. The oversampling
 * above puts the conversion at a few tens of ms, which is what keeps this
 * below the ~25 Hz the part manages at x16 — going faster would have it
 * silently drop the oversampling instead (the OSR_EFF check below). */
#define BMP581_PWR_STANDBY 0x00
#define BMP581_PWR_NORMAL  0x01
#define BMP581_ODR_15HZ    (0x16 << 2)
#define BMP581_DEEP_DIS    (1 << 7)
#define BMP581_ODR_CONFIG (BMP581_PWR_NORMAL | BMP581_ODR_15HZ | BMP581_DEEP_DIS)

/* Leaving deep standby is a write to ODR_CONFIG with deep_dis set; do that
 * first, still in standby, before configuring anything. */
#define BMP581_ODR_AWAKE (BMP581_PWR_STANDBY | BMP581_DEEP_DIS)

/* OSR_EFF bit 7: the requested ODR/OSR combination fits the conversion time. */
#define BMP581_ODR_IS_VALID 0x80

/* Both readings are 24-bit little-endian; the DSP applies compensation before
 * the data registers, so the fixed-point scaling below is all that is left. */
#define BMP581_TEMP_LSB_C  (1.0f / 65536.0f)
#define BMP581_PRESS_LSB_PA (1.0f / 64.0f)

static const char *TAG = "bmp581";

static i2c_master_dev_handle_t s_dev;

/* Where start() gave up, so the warnings below can name the step. */
static const char *s_step = "";

/* Pokes ODR_CONFIG until the part answers, which both waits out the post-reset
 * boot and takes it out of deep standby. */
static esp_err_t wake_after_reset(void)
{
    esp_err_t err = ESP_ERR_INVALID_STATE;

    for (int i = 0; i < BMP581_WAKE_TRIES; i++) {
        esp_rom_delay_us(BMP581_WAKE_WAIT_US);
        err = i2c_dev_write_u8(s_dev, BMP581_REG_ODR_CONFIG, BMP581_ODR_AWAKE);
        if (err == ESP_OK) {
            if (i > 0) {
                ESP_LOGI(TAG, "awake after %d attempts (~%d ms)", i + 1,
                         (i + 1) * BMP581_WAKE_WAIT_US / 1000);
            }
            return ESP_OK;
        }
    }
    return err;
}

static esp_err_t configure(void)
{
    s_step = "reset";
    esp_err_t err = i2c_dev_write_u8(s_dev, BMP581_REG_CMD, BMP581_CMD_SOFT_RESET);
    if (err != ESP_OK) {
        return err;
    }

    s_step = "wake";
    err = wake_after_reset();
    if (err != ESP_OK) {
        return err;
    }

    s_step = "nvm";
    uint8_t status;
    err = i2c_dev_read(s_dev, BMP581_REG_STATUS, &status, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (!(status & BMP581_STATUS_NVM_RDY) || (status & BMP581_STATUS_NVM_ERR)) {
        ESP_LOGW(TAG, "NVM not ready after reset (status 0x%02X)", status);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* filter and oversampling first — writing ODR_CONFIG is what starts the
     * conversions, and neither is meant to change once they are running */
    s_step = "iir";
    err = i2c_dev_write_u8(s_dev, BMP581_REG_DSP_IIR, BMP581_DSP_IIR);
    if (err == ESP_OK) {
        s_step = "dsp";
        err = i2c_dev_write_u8(s_dev, BMP581_REG_DSP_CONFIG, BMP581_DSP_CONFIG);
    }
    if (err == ESP_OK) {
        s_step = "osr";
        err = i2c_dev_write_u8(s_dev, BMP581_REG_OSR_CONFIG, BMP581_OSR_CONFIG);
    }
    if (err == ESP_OK) {
        s_step = "odr";
        err = i2c_dev_write_u8(s_dev, BMP581_REG_ODR_CONFIG, BMP581_ODR_CONFIG);
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t osr_eff;
    if (i2c_dev_read(s_dev, BMP581_REG_OSR_EFF, &osr_eff, 1) == ESP_OK &&
        !(osr_eff & BMP581_ODR_IS_VALID)) {
        /* the sensor silently lowers the oversampling instead of failing */
        ESP_LOGW(TAG, "ODR/OSR combination rejected (osr_eff 0x%02X)", osr_eff);
    }
    return ESP_OK;
}

esp_err_t bmp581_start(void)
{
    esp_err_t last = ESP_ERR_NOT_FOUND;

    s_step = "probe";

    for (int i = 0; i < ARRAY_SIZE(BMP581_ADDRS); i++) {
        uint8_t addr = BMP581_ADDRS[i];
        if (!i2c_dev_present(addr)) {
            continue;
        }
        esp_err_t err = i2c_dev_attach(&s_dev, addr);
        if (err != ESP_OK) {
            s_step = "attach";
            ESP_LOGW(TAG, "0x%02X: setup failed at step \"%s\": %s", addr,
                     s_step, esp_err_to_name(err));
            return err;
        }

        s_step = "chip_id";
        uint8_t id;
        err = i2c_dev_read(s_dev, BMP581_REG_CHIP_ID, &id, 1);
        if (err != ESP_OK) {
            /* the address ACKed the probe but will not talk: a foreign device,
             * or one that cannot drive the bus properly (levels, pull-ups) */
            ESP_LOGW(TAG, "0x%02X ACKed the probe but the chip-id read failed: %s",
                     addr, esp_err_to_name(err));
            last = err;
            continue;
        }
        if (id != BMP581_CHIP_ID_581 && id != BMP581_CHIP_ID_580) {
            ESP_LOGW(TAG, "0x%02X answers with chip id 0x%02X, not a BMP581/580",
                     addr, id);
            s_step = "id_mismatch";
            last = ESP_ERR_NOT_SUPPORTED;
            continue;
        }

        err = configure();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "0x%02X: setup failed at step \"%s\": %s", addr,
                     s_step, esp_err_to_name(err));
            last = err;
            continue;
        }

        uint8_t rev = 0;
        i2c_dev_read(s_dev, BMP581_REG_REV_ID, &rev, 1);
        s_step = "";
        ESP_LOGI(TAG, "%s at 0x%02X, rev 0x%02X",
                 id == BMP581_CHIP_ID_581 ? "BMP581" : "BMP580", addr, rev);
        return ESP_OK;
    }
    return last;
}

esp_err_t bmp581_read(bmp581_data_t *out)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[6];
    esp_err_t err = i2c_dev_read(s_dev, BMP581_REG_TEMP_XLSB, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    int32_t raw_t = (int32_t)((buf[2] << 24) | (buf[1] << 16) | (buf[0] << 8)) >> 8;
    uint32_t raw_p = ((uint32_t)buf[5] << 16) | (buf[4] << 8) | buf[3];

    out->temp_c = raw_t * BMP581_TEMP_LSB_C;
    out->press_hpa = raw_p * BMP581_PRESS_LSB_PA / 100.0f;
    return ESP_OK;
}
