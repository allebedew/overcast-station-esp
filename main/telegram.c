#include "telegram.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "wifi.h"

/* Bot credentials are baked into the firmware, same approach as OTA_KEY.
 * Get the token from @BotFather, the chat id from @userinfobot. */
#define TELEGRAM_TOKEN   "8789784700:AAEXInU2cZnTdKwlax0hp1JPQvfRDqEqzG4"
#define TELEGRAM_CHAT_ID "51137287"

#define MAX_TEXT       200
#define QUEUE_LEN      8
#define MAX_ATTEMPTS   5     /* per message, only counting actual HTTP failures */
#define RETRY_DELAY_MS 5000

static const char *TAG = "telegram";

static QueueHandle_t s_queue;

/* Sends one message. Returns true if it must not be retried:
 * delivered, or rejected by the API (bad token/chat id). */
static bool send_message(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "chat_id", TELEGRAM_CHAT_ID);
    cJSON_AddStringToObject(root, "text", text);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return true;
    }

    esp_http_client_config_t cfg = {
        .url = "https://api.telegram.org/bot" TELEGRAM_TOKEN "/sendMessage",
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    bool done = false;
    if (client) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "send failed: %s", esp_err_to_name(err));
        } else if (status != 200) {
            ESP_LOGE(TAG, "API rejected message: HTTP %d", status);
            done = true; /* permanent (auth/format) — retry would not help */
        } else {
            ESP_LOGI(TAG, "sent: %s", text);
            done = true;
        }
        esp_http_client_cleanup(client);
    }
    cJSON_free(body);
    return done;
}

static void telegram_task(void *arg)
{
    static char text[MAX_TEXT];
    for (;;) {
        xQueueReceive(s_queue, text, portMAX_DELAY);
        int attempts = 0;
        while (attempts < MAX_ATTEMPTS) {
            /* no network — just wait, does not count as an attempt */
            if (wifi_get_status() != WIFI_STATUS_CONNECTED) {
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
                continue;
            }
            if (send_message(text)) {
                break;
            }
            attempts++;
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        }
        if (attempts == MAX_ATTEMPTS) {
            ESP_LOGE(TAG, "giving up on message: %s", text);
        }
    }
}

void telegram_init(void)
{
    if (TELEGRAM_TOKEN[0] == '\0' || TELEGRAM_CHAT_ID[0] == '\0') {
        ESP_LOGW(TAG, "token/chat id not set, notifications disabled");
        return;
    }
    s_queue = xQueueCreate(QUEUE_LEN, MAX_TEXT);
    /* 8 KiB stack: TLS handshake runs in this task */
    xTaskCreate(telegram_task, "telegram", 8192, NULL, 2, NULL);
}

bool telegram_notify(const char *fmt, ...)
{
    if (!s_queue) {
        return false;
    }
    char text[MAX_TEXT];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    if (xQueueSend(s_queue, text, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropped: %s", text);
        return false;
    }
    return true;
}
