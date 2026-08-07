#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"

/* True while a firmware image is being received. */
bool ota_is_active(void);

/* How far the upload has got, for the display. `total` is fixed for the whole
 * transfer, so the two are never read half a chunk apart; both are 0 unless
 * ota_is_active(). */
void ota_get_progress(size_t *received, size_t *total);

/* POST /api/ota: writes the image from the request body into the free OTA
 * partition and reboots. Requires the X-OTA-Key header (see OTA_KEY). */
esp_err_t ota_post_handler(httpd_req_t *req);

/* Marks the image valid, cancelling the automatic rollback. Call at the end of
 * app_main, once everything is up. */
void ota_confirm_running_image(void);
