#include "i2c_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

/* Wiring: J1 bottom corner — SDA GPIO2, SCL GPIO3, 5V, GND. */
#define I2C_SDA_GPIO 2
#define I2C_SCL_GPIO 3

/* 7-bit address space minus the reserved ranges. A missing device NACKs at
 * once, so a short timeout keeps a full sweep under a second. */
#define I2C_SCAN_FIRST_ADDR 0x08
#define I2C_SCAN_LAST_ADDR  0x77
#define I2C_SCAN_TIMEOUT_MS 20

static const char *TAG = "i2c_bus";

static i2c_master_bus_handle_t s_bus;
static SemaphoreHandle_t s_lock;

i2c_master_bus_handle_t i2c_bus_handle(void)
{
    return s_bus;
}

void i2c_bus_lock(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void i2c_bus_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

/* Names for the expected addresses, so the scan log reads by itself. */
static const char *known_device(uint8_t addr)
{
    switch (addr) {
    case 0x62:       return " (SCD40)";
    case 0x10:       return " (VEML7700)";
    case 0x46: case 0x47:
                     return " (BMP581)";
    case 0x48: case 0x49: case 0x4A: case 0x4B:
                     return " (TMP117)";
    case 0x3E:       return " (LCD1602 controller)";
    case 0x60: case 0x6B: case 0x30: case 0x2D:
                     return " (LCD1602 RGB backlight)";
    default:         return "";
    }
}

int i2c_bus_scan(void)
{
    int found = 0;

    /* The driver logs an error per NACK, which is the normal case here. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    i2c_bus_lock();
    ESP_LOGI(TAG, "I2C scan on GPIO%d/%d (SDA/SCL):", I2C_SDA_GPIO, I2C_SCL_GPIO);
    for (uint8_t addr = I2C_SCAN_FIRST_ADDR; addr <= I2C_SCAN_LAST_ADDR; addr++) {
        if (i2c_master_probe(s_bus, addr, I2C_SCAN_TIMEOUT_MS) != ESP_OK) {
            continue;
        }
        found++;
        ESP_LOGI(TAG, "  0x%02X%s", addr, known_device(addr));
    }
    i2c_bus_unlock();

    esp_log_level_set("i2c.master", ESP_LOG_INFO);

    if (found == 0) {
        ESP_LOGW(TAG, "I2C scan: no devices found");
    } else {
        ESP_LOGI(TAG, "I2C scan: %d device(s) found", found);
    }
    return found;
}

void i2c_bus_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, /* the breakout has its own too */
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_bus));

    s_lock = xSemaphoreCreateRecursiveMutex();
    ESP_ERROR_CHECK(s_lock ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "I2C bus ready on GPIO%d/%d", I2C_SDA_GPIO, I2C_SCL_GPIO);
    i2c_bus_scan(); /* before the polling tasks, so the log stays readable */
}
