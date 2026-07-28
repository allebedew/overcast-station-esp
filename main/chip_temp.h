#pragma once

/* The SoC's own temperature sensor. It is not on the I2C bus and measures the
 * die, not the room, so it lives apart from the sensors module: it is a health
 * reading like free heap or uptime, and that is where it is reported. */

void chip_temp_init(void);

/* Chip temperature, °C; -273 when the read fails. */
float chip_temp_celsius(void);
