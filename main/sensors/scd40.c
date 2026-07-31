#include "scd40.h"

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "i2c_dev.h"

/* Sensirion I2C protocol: 16-bit commands, big-endian 16-bit data words
 * each followed by CRC-8 (poly 0x31, init 0xFF). */
#define SCD40_ADDR             0x62
#define SCD40_CMD_START        0x21B1 /* start periodic measurement, 5 s cycle */
#define SCD40_CMD_STOP         0x3F86
#define SCD40_CMD_READ         0xEC05
#define SCD40_CMD_DATA_READY   0xE4B8
#define SCD40_CMD_FRC          0x362F /* forced recalibration */
#define SCD40_CMD_SET_PRESSURE 0xE000 /* ambient pressure compensation, hPa */
#define SCD40_CMD_GET_ASC      0x2313 /* automatic self-calibration, on/off */
#define SCD40_CMD_SET_ASC      0x2416
#define SCD40_CMD_PERSIST      0x3615 /* writes the settings to EEPROM */

/* Datasheet: a read command needs ≥1 ms before the result can be clocked out.
 * Busy-waited — a vTaskDelay() would round to a whole 10 ms tick and hold the
 * bus lock for it. */
#define SCD40_CMD_DELAY_US  2000
#define SCD40_STOP_DELAY_MS 500 /* datasheet: stop takes up to 500 ms */
#define SCD40_FRC_DELAY_MS  400 /* datasheet: FRC takes 400 ms */

/* A fresh result appears every 5 s. Once one is caught the phase is known and
 * checks are skipped until shortly before the next is due; the cost is that an
 * unplugged sensor goes unnoticed for those seconds. */
#define SCD40_QUIET_MS 4500

static const char *TAG = "scd40";

static i2c_master_dev_handle_t s_dev;

/* Where in the start sequence the next call resumes. */
typedef enum {
    START_STOP, /* stop whatever the sensor was doing */
    START_ASC,  /* check (and once, clear) automatic self-calibration */
    START_RUN,  /* begin periodic measurement */
} start_phase_t;

static start_phase_t s_phase = START_STOP;

/* Earliest time the sensor is worth asking again, µs on the esp_timer clock. */
static int64_t s_next_check_us;

static uint8_t crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
        }
    }
    return crc;
}

static esp_err_t cmd(uint16_t c)
{
    uint8_t buf[2] = { c >> 8, c & 0xFF };
    return i2c_dev_transmit(s_dev, buf, sizeof(buf));
}

/* Sends a command with one 16-bit argument (CRC-protected). */
static esp_err_t write_word(uint16_t c, uint16_t value)
{
    uint8_t buf[5] = { c >> 8, c & 0xFF, value >> 8, value & 0xFF, 0 };
    buf[4] = crc8(&buf[2], 2);
    return i2c_dev_transmit(s_dev, buf, sizeof(buf));
}

/* Sends a read command and receives n 16-bit words, verifying CRCs. */
static esp_err_t read_words(uint16_t c, uint16_t *words, int n)
{
    esp_err_t err = cmd(c);
    if (err != ESP_OK) {
        return err;
    }
    esp_rom_delay_us(SCD40_CMD_DELAY_US);

    uint8_t buf[9]; /* up to 3 words */
    err = i2c_dev_receive(s_dev, buf, 3 * n);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < n; i++) {
        if (crc8(&buf[3 * i], 2) != buf[3 * i + 2]) {
            return ESP_ERR_INVALID_CRC;
        }
        words[i] = (buf[3 * i] << 8) | buf[3 * i + 1];
    }
    return ESP_OK;
}

void scd40_init(void)
{
    ESP_ERROR_CHECK(i2c_dev_attach(&s_dev, SCD40_ADDR));
}

/* ASC assumes the sensor sees ~400 ppm outdoor air as its weekly minimum; a
 * room that is never aired that far drags the calibration off, so this station
 * uses the FRC button instead. Cleared only when actually on — disabling costs
 * an EEPROM write, once in the sensor's life rather than once per boot. */
static esp_err_t disable_asc(bool *written)
{
    uint16_t enabled;
    esp_err_t err = read_words(SCD40_CMD_GET_ASC, &enabled, 1);
    if (err != ESP_OK || !enabled) {
        *written = false;
        return err;
    }

    err = write_word(SCD40_CMD_SET_ASC, 0);
    if (err == ESP_OK) {
        err = cmd(SCD40_CMD_PERSIST);
    }
    if (err != ESP_OK) {
        *written = false;
        return err;
    }
    ESP_LOGI(TAG, "automatic self-calibration disabled and persisted; "
                  "use the FRC calibration on the web page instead");
    *written = true;
    return ESP_OK;
}

esp_err_t scd40_start(void)
{
    if (s_phase == START_STOP) {
        if (!i2c_dev_present(SCD40_ADDR)) {
            return ESP_ERR_NOT_FOUND;
        }
        /* after an ESP reboot the sensor may still be measuring, and every
         * command below needs it idle */
        cmd(SCD40_CMD_STOP);
        s_phase = START_ASC;
        return ESP_ERR_NOT_FINISHED;
    }

    if (s_phase == START_ASC) {
        bool persisting = false;
        esp_err_t err = disable_asc(&persisting);
        if (err != ESP_OK) {
            s_phase = START_STOP;
            return err;
        }
        s_phase = START_RUN;
        if (persisting) {
            return ESP_ERR_NOT_FINISHED; /* the EEPROM write takes 800 ms */
        }
        /* nothing written, so no wait is owed */
    }

    s_phase = START_STOP; /* a later start begins anew */
    esp_err_t err = cmd(SCD40_CMD_START);
    if (err != ESP_OK) {
        return err;
    }
    s_next_check_us = 0; /* read at the first opportunity */
    ESP_LOGI(TAG, "detected at 0x%02X, periodic measurement started", SCD40_ADDR);
    return ESP_OK;
}

/* Magnus-Tetens, WMO coefficients. RH is clamped off zero: logf(0) is -inf. */
static float dew_point_c(float t_c, float rh_pct)
{
    const float b = 17.62f, c = 243.12f;
    if (rh_pct < 1.0f) {
        rh_pct = 1.0f;
    }
    float gamma = logf(rh_pct / 100.0f) + b * t_c / (c + t_c);
    return c * gamma / (b - gamma);
}

esp_err_t scd40_read(scd40_data_t *out)
{
    int64_t now = esp_timer_get_time();
    if (now < s_next_check_us) {
        return ESP_ERR_NOT_FINISHED; /* still inside the quiet window */
    }

    uint16_t ready;
    esp_err_t err = read_words(SCD40_CMD_DATA_READY, &ready, 1);
    if (err != ESP_OK) {
        return err;
    }
    if ((ready & 0x07FF) == 0) {
        return ESP_ERR_NOT_FINISHED; /* measurement in progress */
    }

    uint16_t raw[3];
    err = read_words(SCD40_CMD_READ, raw, 3);
    if (err != ESP_OK) {
        return err;
    }

    out->co2_ppm = raw[0];
    out->temp_c = -45.0f + 175.0f * raw[1] / 65535.0f;
    out->rh_pct = 100.0f * raw[2] / 65535.0f;
    out->dew_c = dew_point_c(out->temp_c, out->rh_pct);

    s_next_check_us = now + (int64_t)SCD40_QUIET_MS * 1000;
    return ESP_OK;
}

esp_err_t scd40_set_pressure(uint16_t hpa)
{
    return write_word(SCD40_CMD_SET_PRESSURE, hpa);
}

esp_err_t scd40_calibrate(uint16_t target_ppm, int *correction_ppm)
{
    /* Atomic on the bus, waits included: blocks everything else for about a
     * second. Runs from a web request, once, on purpose. */
    esp_err_t err = cmd(SCD40_CMD_STOP);
    vTaskDelay(pdMS_TO_TICKS(SCD40_STOP_DELAY_MS));

    if (err == ESP_OK) {
        err = write_word(SCD40_CMD_FRC, target_ppm);
    }
    vTaskDelay(pdMS_TO_TICKS(SCD40_FRC_DELAY_MS));

    uint8_t rsp[3] = {0};
    if (err == ESP_OK) {
        err = i2c_dev_receive(s_dev, rsp, sizeof(rsp));
    }
    if (err == ESP_OK && crc8(rsp, 2) != rsp[2]) {
        err = ESP_ERR_INVALID_CRC;
    }
    uint16_t word = (rsp[0] << 8) | rsp[1];
    if (err == ESP_OK && word == 0xFFFF) {
        err = ESP_FAIL; /* sensor rejected the calibration */
    }

    cmd(SCD40_CMD_START); /* resume measurements regardless */
    s_next_check_us = 0;

    if (err == ESP_OK) {
        *correction_ppm = (int)word - 0x8000;
        ESP_LOGI(TAG, "FRC done: target %u ppm, correction %d ppm",
                 target_ppm, *correction_ppm);
    } else {
        ESP_LOGW(TAG, "FRC failed: %s", esp_err_to_name(err));
    }
    return err;
}
