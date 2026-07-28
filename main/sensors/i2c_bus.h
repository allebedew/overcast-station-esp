#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

/* The one I2C master bus of the board (SDA GPIO2 / SCL GPIO3) and the lock
 * that arbitrates it.
 *
 * Everything on the bus goes through here: the sensors, the LCD, anything
 * added later. The bus itself is thread-safe per transaction, but almost every
 * device needs *sequences* to stay atomic — a command followed by a read, an
 * address write followed by the data, a config change bracketed by a power
 * cycle. A foreign transfer slipped in between corrupts those.
 *
 * Rule: take the lock around a complete device operation, at whatever level
 * owns the sequence. The lock is recursive, so a helper that locks on its own
 * and a caller that brackets a longer sequence compose without deadlocking.
 * Do not hold it across a vTaskDelay() that merely waits out a device's
 * settling time — release it, the bus is free during that wait. */

/* Creates the bus and the lock, then logs a scan of the address space.
 * Call once, before any module that touches the bus. */
void i2c_bus_init(void);

/* NULL before i2c_bus_init(). */
i2c_master_bus_handle_t i2c_bus_handle(void);

void i2c_bus_lock(void);
void i2c_bus_unlock(void);

/* Scans the bus and logs every responding address (0x08-0x77), naming the
 * ones we expect. Returns the number of devices found; takes the lock itself.
 * Blocks for up to ~1 s on an empty bus. */
int i2c_bus_scan(void);
