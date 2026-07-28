#include "i2c_dev.h"

#include "i2c_bus.h"

/* Every device on this bus is happy at the standard speed, and none of them
 * takes anywhere near 100 ms to answer — a timeout that long only ever fires
 * when the part is gone. */
#define I2C_TIMEOUT_MS 100
#define I2C_SPEED_HZ   100000

bool i2c_dev_present(uint8_t addr)
{
    return i2c_master_probe(i2c_bus_handle(), addr, I2C_TIMEOUT_MS) == ESP_OK;
}

esp_err_t i2c_dev_attach(i2c_master_dev_handle_t *dev, uint8_t addr)
{
    if (*dev) {
        i2c_master_bus_rm_device(*dev);
        *dev = NULL;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_SPEED_HZ,
    };
    return i2c_master_bus_add_device(i2c_bus_handle(), &cfg, dev);
}

esp_err_t i2c_dev_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t len)
{
    return i2c_master_transmit(dev, buf, len, I2C_TIMEOUT_MS);
}

esp_err_t i2c_dev_receive(i2c_master_dev_handle_t dev, uint8_t *buf, size_t len)
{
    return i2c_master_receive(dev, buf, len, I2C_TIMEOUT_MS);
}

esp_err_t i2c_dev_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

esp_err_t i2c_dev_write_u8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_dev_transmit(dev, buf, sizeof(buf));
}

esp_err_t i2c_dev_read_u16be(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *value)
{
    uint8_t buf[2];
    esp_err_t err = i2c_dev_read(dev, reg, buf, sizeof(buf));
    if (err == ESP_OK) {
        *value = (buf[0] << 8) | buf[1];
    }
    return err;
}

esp_err_t i2c_dev_write_u16be(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = { reg, value >> 8, value & 0xFF };
    return i2c_dev_transmit(dev, buf, sizeof(buf));
}

esp_err_t i2c_dev_read_u16le(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *value)
{
    uint8_t buf[2];
    esp_err_t err = i2c_dev_read(dev, reg, buf, sizeof(buf));
    if (err == ESP_OK) {
        *value = (buf[1] << 8) | buf[0];
    }
    return err;
}

esp_err_t i2c_dev_write_u16le(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = { reg, value & 0xFF, value >> 8 };
    return i2c_dev_transmit(dev, buf, sizeof(buf));
}
