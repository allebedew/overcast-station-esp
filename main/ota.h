#pragma once

#include <stdbool.h>

#include "esp_http_server.h"

/* True while a firmware image is being received (the LED task polls this to
 * show the purple "OTA in progress" pattern). */
bool ota_is_active(void);

/* How far the upload has got, 0-100, for the display's progress bar. Only
 * meaningful while ota_is_active(); 0 otherwise. */
int ota_progress_percent(void);

/* Обработчик POST /api/ota: принимает бинарник прошивки в теле запроса,
 * пишет его в свободный OTA-раздел и перезагружает устройство.
 * Требует заголовок X-OTA-Key (см. OTA_KEY в ota.c и flash-ota.sh). */
esp_err_t ota_post_handler(httpd_req_t *req);

/* Подтверждает работоспособность прошивки после OTA-обновления (отменяет
 * автоматический откат). Звать в конце app_main, когда всё поднялось. */
void ota_confirm_running_image(void);
