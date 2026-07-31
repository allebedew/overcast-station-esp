#pragma once

#include <stdbool.h>

#include "esp_http_server.h"

/* True while a firmware image is being received. */
bool ota_is_active(void);

/* How far the upload has got, 0-100, for the display's progress bar. Only
 * meaningful while ota_is_active(); 0 otherwise. */
int ota_progress_percent(void);

/* POST /api/ota: writes the image from the request body into the free OTA
 * partition and reboots. Requires the X-OTA-Key header (see OTA_KEY). */
esp_err_t ota_post_handler(httpd_req_t *req);

/* Marks the image valid, cancelling the automatic rollback. Call at the end of
 * app_main, once everything is up. */
void ota_confirm_running_image(void);
