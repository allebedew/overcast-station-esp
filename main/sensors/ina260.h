#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* TI INA260 — bus voltage, current and power through its own 2 mΩ shunt. On
 * this board it sits in the battery lead, IN+ on the BQ25185's BAT and IN− on
 * the cell, so a positive current is charge. Transport only; polling lives in
 * sensors.c. */

typedef struct {
    float voltage_v;  /* battery terminal */
    float current_ma; /* + into the cell, − out of it */
    float power_mw;   /* the chip's own |V·I|: unsigned */
} ina260_data_t;

/* Probes the address, verifies both ID registers and starts continuous
 * averaged conversion. Safe to call again after a failure. */
esp_err_t ina260_start(void);

/* The averaged result. ESP_ERR_NOT_FINISHED until a new one has completed. */
esp_err_t ina260_read(ina260_data_t *out);
