#include "sensors.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "history.h"

/* SCD40 wiring: J1 bottom corner — SDA GPIO2, SCL GPIO3, 5V, GND. */
#define I2C_SDA_GPIO 2
#define I2C_SCL_GPIO 3
#define I2C_TIMEOUT_MS 100

/* Bus scan: 7-bit address space minus the reserved ranges (0x00-0x07,
 * 0x78-0x7F). A missing device NACKs immediately, so a short timeout
 * keeps a full sweep well under a second. */
#define I2C_SCAN_FIRST_ADDR 0x08
#define I2C_SCAN_LAST_ADDR  0x77
#define I2C_SCAN_TIMEOUT_MS 20

/* Sensirion I2C protocol: 16-bit commands, big-endian 16-bit data words
 * each followed by CRC-8 (poly 0x31, init 0xFF). */
#define SCD40_ADDR           0x62
#define SCD40_CMD_START      0x21B1 /* start periodic measurement, 5 s cycle */
#define SCD40_CMD_STOP       0x3F86
#define SCD40_CMD_READ       0xEC05
#define SCD40_CMD_DATA_READY 0xE4B8
#define SCD40_CMD_FRC        0x362F /* forced recalibration */
#define SCD40_CMD_SET_PRESSURE 0xE000 /* ambient pressure compensation, hPa */
#define SCD40_STOP_DELAY_MS  500    /* datasheet: stop takes up to 500 ms */
#define SCD40_FRC_DELAY_MS   400    /* datasheet: FRC takes 400 ms */
#define SCD40_MAX_ERRORS     3      /* consecutive failures before "offline" */
#define SCD40_PROBE_PERIOD_MS 5000  /* how often to look for an absent sensor */

/* Measured at the sensor site (245 m above sea level). Hardcoded until
 * a pressure sensor (BMP390) provides live values. */
#define SCD40_AMBIENT_HPA 985

static const char *TAG = "sensors";

static temperature_sensor_handle_t s_temp_sensor;

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_scd40;

/* Serializes bus access between the polling task and calibration. */
static SemaphoreHandle_t s_i2c_mutex;
static bool s_running; /* sensor detected, periodic measurement active */

/* Written by the scd40 task, read by httpd/alerts. */
static portMUX_TYPE s_scd40_lock = portMUX_INITIALIZER_UNLOCKED;
static scd40_data_t s_scd40_data;
static bool s_scd40_valid;

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

static esp_err_t scd40_cmd(uint16_t cmd)
{
    uint8_t buf[2] = { cmd >> 8, cmd & 0xFF };
    return i2c_master_transmit(s_scd40, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

/* Sends a command with one 16-bit argument (CRC-protected). */
static esp_err_t scd40_write_word(uint16_t cmd, uint16_t value)
{
    uint8_t buf[5] = { cmd >> 8, cmd & 0xFF, value >> 8, value & 0xFF, 0 };
    buf[4] = crc8(&buf[2], 2);
    return i2c_master_transmit(s_scd40, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

/* Sends a read command and receives n 16-bit words, verifying CRCs. */
static esp_err_t scd40_read_words(uint16_t cmd, uint16_t *words, int n)
{
    esp_err_t err = scd40_cmd(cmd);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2)); /* command execution time, ≥1 ms */

    uint8_t buf[9]; /* up to 3 words */
    err = i2c_master_receive(s_scd40, buf, 3 * n, I2C_TIMEOUT_MS);
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

static void scd40_set_invalid(void)
{
    taskENTER_CRITICAL(&s_scd40_lock);
    s_scd40_valid = false;
    taskEXIT_CRITICAL(&s_scd40_lock);
}

/* Probes the sensor and (re)starts periodic measurement. */
static bool scd40_start(void)
{
    if (i2c_master_probe(s_i2c_bus, SCD40_ADDR, I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    /* the sensor may still be measuring after an ESP reboot — stop first */
    scd40_cmd(SCD40_CMD_STOP);
    vTaskDelay(pdMS_TO_TICKS(SCD40_STOP_DELAY_MS));
    if (scd40_cmd(SCD40_CMD_START) != ESP_OK) {
        return false;
    }
    /* compensation resets on sensor power-down, so set it on every start */
    if (scd40_write_word(SCD40_CMD_SET_PRESSURE, SCD40_AMBIENT_HPA) != ESP_OK) {
        ESP_LOGW(TAG, "SCD40: failed to set ambient pressure");
    }
    ESP_LOGI(TAG, "SCD40 detected, periodic measurement started (%d hPa)",
             SCD40_AMBIENT_HPA);
    return true;
}

/* One polling step: grab the measurement if one is ready.
 * Tracks consecutive errors; on too many marks the sensor offline. */
static void scd40_poll(int *errors)
{
    uint16_t ready;
    esp_err_t err = scd40_read_words(SCD40_CMD_DATA_READY, &ready, 1);
    if (err == ESP_OK && (ready & 0x07FF) == 0) {
        return; /* measurement in progress */
    }

    uint16_t raw[3];
    if (err == ESP_OK) {
        err = scd40_read_words(SCD40_CMD_READ, raw, 3);
    }
    if (err != ESP_OK) {
        if (++*errors >= SCD40_MAX_ERRORS) {
            ESP_LOGW(TAG, "SCD40 not responding (%s), will re-probe",
                     esp_err_to_name(err));
            scd40_set_invalid();
            s_running = false;
        }
        return;
    }
    *errors = 0;

    scd40_data_t d = {
        .co2_ppm = raw[0],
        .temp_c = -45.0f + 175.0f * raw[1] / 65535.0f,
        .rh_pct = 100.0f * raw[2] / 65535.0f,
    };
    taskENTER_CRITICAL(&s_scd40_lock);
    s_scd40_data = d;
    s_scd40_valid = true;
    taskEXIT_CRITICAL(&s_scd40_lock);

    history_add(d.co2_ppm, d.temp_c, d.rh_pct);
}

/* Polls the sensor once a second; a fresh reading appears every 5 s
 * (the fastest the SCD40 supports). On repeated errors the sensor is
 * declared offline and re-probed every SCD40_PROBE_PERIOD_MS. */
static void scd40_task(void *arg)
{
    int errors = 0;

    for (;;) {
        if (!s_running) {
            xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
            s_running = scd40_start();
            xSemaphoreGive(s_i2c_mutex);
            errors = 0;
            if (!s_running) {
                vTaskDelay(pdMS_TO_TICKS(SCD40_PROBE_PERIOD_MS));
            }
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
        xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
        if (s_running) { /* calibration may have failed the sensor meanwhile */
            scd40_poll(&errors);
        }
        xSemaphoreGive(s_i2c_mutex);
    }
}

esp_err_t sensors_scd40_calibrate(uint16_t target_ppm, int *correction_ppm)
{
    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    if (!s_running) {
        xSemaphoreGive(s_i2c_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* datasheet sequence: stop periodic → FRC command with target → result */
    esp_err_t err = scd40_cmd(SCD40_CMD_STOP);
    vTaskDelay(pdMS_TO_TICKS(SCD40_STOP_DELAY_MS));

    if (err == ESP_OK) {
        err = scd40_write_word(SCD40_CMD_FRC, target_ppm);
    }
    vTaskDelay(pdMS_TO_TICKS(SCD40_FRC_DELAY_MS));

    uint8_t rsp[3] = {0};
    if (err == ESP_OK) {
        err = i2c_master_receive(s_scd40, rsp, sizeof(rsp), I2C_TIMEOUT_MS);
    }
    if (err == ESP_OK && crc8(rsp, 2) != rsp[2]) {
        err = ESP_ERR_INVALID_CRC;
    }
    uint16_t word = (rsp[0] << 8) | rsp[1];
    if (err == ESP_OK && word == 0xFFFF) {
        err = ESP_FAIL; /* sensor rejected the calibration */
    }

    scd40_cmd(SCD40_CMD_START); /* resume measurements regardless */
    xSemaphoreGive(s_i2c_mutex);

    if (err == ESP_OK) {
        *correction_ppm = (int)word - 0x8000;
        ESP_LOGI(TAG, "SCD40 FRC done: target %u ppm, correction %d ppm",
                 target_ppm, *correction_ppm);
    } else {
        ESP_LOGW(TAG, "SCD40 FRC failed: %s", esp_err_to_name(err));
    }
    return err;
}

i2c_master_bus_handle_t sensors_i2c_bus(void)
{
    return s_i2c_bus;
}

/* Names for the addresses we expect to see, to make the log self-explanatory. */
static const char *i2c_known_device(uint8_t addr)
{
    switch (addr) {
    case SCD40_ADDR: return " (SCD40)";
    case 0x3E:       return " (LCD1602 controller)";
    case 0x60: case 0x6B: case 0x30: case 0x2D:
                     return " (LCD1602 RGB backlight)";
    default:         return "";
    }
}

int sensors_i2c_scan(void)
{
    int found = 0;

    /* The driver logs an error for every address that NACKs, which is the
     * normal case here — mute it for the duration of the sweep. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "I2C scan on GPIO%d/%d (SDA/SCL):", I2C_SDA_GPIO, I2C_SCL_GPIO);
    for (uint8_t addr = I2C_SCAN_FIRST_ADDR; addr <= I2C_SCAN_LAST_ADDR; addr++) {
        if (i2c_master_probe(s_i2c_bus, addr, I2C_SCAN_TIMEOUT_MS) != ESP_OK) {
            continue;
        }
        found++;
        ESP_LOGI(TAG, "  0x%02X%s", addr, i2c_known_device(addr));
    }
    xSemaphoreGive(s_i2c_mutex);

    esp_log_level_set("i2c.master", ESP_LOG_INFO);

    if (found == 0) {
        ESP_LOGW(TAG, "I2C scan: no devices found");
    } else {
        ESP_LOGI(TAG, "I2C scan: %d device(s) found", found);
    }
    return found;
}

void sensors_init(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    ESP_ERROR_CHECK(temperature_sensor_install(&cfg, &s_temp_sensor));
    ESP_ERROR_CHECK(temperature_sensor_enable(s_temp_sensor));

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, /* breakout has its own, belt & braces */
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SCD40_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_scd40));

    s_i2c_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "Chip temperature sensor ready, I2C bus on GPIO%d/%d",
             I2C_SDA_GPIO, I2C_SCL_GPIO);

    sensors_i2c_scan(); /* before the task starts, so the log stays readable */
    xTaskCreate(scd40_task, "scd40", 3072, NULL, 2, NULL);
}

float sensors_chip_temp(void)
{
    float temp = -273;
    temperature_sensor_get_celsius(s_temp_sensor, &temp);
    return temp;
}

bool sensors_scd40_get(scd40_data_t *out)
{
    taskENTER_CRITICAL(&s_scd40_lock);
    bool valid = s_scd40_valid;
    *out = s_scd40_data;
    taskEXIT_CRITICAL(&s_scd40_lock);
    return valid;
}
