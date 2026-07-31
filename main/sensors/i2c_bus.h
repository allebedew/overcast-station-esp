#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

/* The board's one I2C master bus (SDA GPIO2 / SCL GPIO3) and the lock that
 * arbitrates it. The bus is thread-safe per transaction, but nearly every
 * device needs whole *sequences* atomic — a command then a read, an address
 * then the data — and a foreign transfer in between corrupts them.
 *
 * Rule: take the lock around a complete device operation, at whatever level
 * owns the sequence. It is recursive, so a self-locking helper and a caller
 * bracketing a longer sequence compose without deadlocking. Do not hold it
 * across a vTaskDelay() waiting out a device's settling time — the bus is free
 * during that wait. */

/* Creates the bus and the lock, then logs a scan of the address space.
 * Call once, before any module that touches the bus. */
void i2c_bus_init(void);

/* NULL before i2c_bus_init(). */
i2c_master_bus_handle_t i2c_bus_handle(void);

void i2c_bus_lock(void);
void i2c_bus_unlock(void);

/* Logs every responding address (0x08-0x77), naming the expected ones. Returns
 * the count; takes the lock itself. Up to ~1 s on an empty bus. */
int i2c_bus_scan(void);
