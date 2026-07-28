#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/* The register access every sensor driver on this bus was repeating: attach a
 * device handle, write a register, read one back. Endianness differs between
 * the parts (the VEML7700 puts the low byte first, everyone else the high
 * one), so both orders are spelled out rather than hidden behind a flag.
 *
 * None of these take the bus lock — the caller owns the sequence and holds it
 * around the whole operation. See i2c_bus.h. */

/* True if something ACKs at that address. */
bool i2c_dev_present(uint8_t addr);

/* Points *dev at addr on the shared bus. The address is fixed when the handle
 * is created, so an existing handle is dropped first — that is what makes a
 * re-probe on a different address work. */
esp_err_t i2c_dev_attach(i2c_master_dev_handle_t *dev, uint8_t addr);

/* Raw transfers, for parts without a register model (the SCD40's command
 * protocol). */
esp_err_t i2c_dev_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t len);
esp_err_t i2c_dev_receive(i2c_master_dev_handle_t dev, uint8_t *buf, size_t len);

/* Register reads/writes: address byte out, data in (or out) in one transfer. */
esp_err_t i2c_dev_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len);
esp_err_t i2c_dev_write_u8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value);
esp_err_t i2c_dev_read_u16be(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *value);
esp_err_t i2c_dev_write_u16be(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t value);
esp_err_t i2c_dev_read_u16le(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *value);
esp_err_t i2c_dev_write_u16le(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t value);
