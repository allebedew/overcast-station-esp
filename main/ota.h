#pragma once

#include "esp_http_server.h"

/* Обработчик POST /api/ota: принимает бинарник прошивки в теле запроса,
 * пишет его в свободный OTA-раздел и перезагружает устройство.
 * Требует заголовок X-OTA-Key (см. OTA_KEY в ota.c и flash-ota.sh). */
esp_err_t ota_post_handler(httpd_req_t *req);

/* Подтверждает работоспособность прошивки после OTA-обновления (отменяет
 * автоматический откат). Звать в конце app_main, когда всё поднялось. */
void ota_confirm_running_image(void);
