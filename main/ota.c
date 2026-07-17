#include "ota.h"

#include <string.h>
#include <sys/param.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "led.h"
#include "esp_system.h"
#include "esp_timer.h"

/* Общий секрет для прошивки по сети — тот же указан в flash-ota.sh.
 * Защищает от случайной/чужой заливки из локальной сети или из режима AP. */
#define OTA_KEY "weather-ota"

#define OTA_REBOOT_DELAY_MS 1000

static const char *TAG = "ota";

static void reboot_cb(void *arg)
{
    esp_restart();
}

/* Перезагрузка с задержкой, чтобы HTTP-ответ успел уйти клиенту. */
static void schedule_reboot(void)
{
    const esp_timer_create_args_t args = {
        .callback = reboot_cb,
        .name = "ota_reboot",
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_once(timer, OTA_REBOOT_DELAY_MS * 1000ULL));
}

esp_err_t ota_post_handler(httpd_req_t *req)
{
    char key[sizeof(OTA_KEY) + 1] = "";
    httpd_req_get_hdr_value_str(req, "X-OTA-Key", key, sizeof(key));
    if (strcmp(key, OTA_KEY) != 0) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad ota key");
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no ota partition");
    }
    if (req->content_len == 0 || req->content_len > target->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad image size");
    }

    ESP_LOGI(TAG, "OTA start: %u bytes -> %s",
             (unsigned)req->content_len, target->label);

    /* на время загрузки — фиолетовое мигание; при ошибке вернём как было */
    led_status_t prev_led = led_get_status();
    led_set_status(LED_STATUS_OTA);

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        led_set_status(prev_led);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "ota begin failed");
    }

    /* буфер статический: обработчики выполняются в единственной задаче httpd */
    static char buf[4096];
    size_t received = 0;
    int last_decile = -1;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf,
                               MIN(sizeof(buf), req->content_len - received));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buf, r);
        if (err != ESP_OK) {
            break;
        }
        received += r;
        int decile = 10 * received / req->content_len;
        if (decile != last_decile) {
            last_decile = decile;
            ESP_LOGI(TAG, "OTA: %d%%", decile * 10);
        }
    }

    if (err == ESP_OK) {
        err = esp_ota_end(ota); /* проверяет целостность образа */
    } else {
        esp_ota_abort(ota);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(target);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        led_set_status(prev_led);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid image");
    }

    ESP_LOGI(TAG, "OTA done, rebooting into %s", target->label);
    esp_err_t ret = httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    schedule_reboot();
    return ret;
}

void ota_confirm_running_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        ESP_LOGI(TAG, "New firmware confirmed (%s)", running->label);
    }
}
