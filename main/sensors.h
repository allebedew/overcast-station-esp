#pragma once

void sensors_init(void);

/* Температура чипа, °C; -273 при ошибке чтения. */
float sensors_chip_temp(void);
