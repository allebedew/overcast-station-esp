#pragma once

#include <stdint.h>

typedef enum {
    LED_STATUS_OFF,
    LED_STATUS_WIFI_CONNECTING,   /* жёлтый моргает */
    LED_STATUS_WIFI_CONNECTED,    /* зелёный */
    LED_STATUS_WIFI_FAILED,       /* красный */
    LED_STATUS_WIFI_DISCONNECTED, /* жёлтый: не подключено или пауза между попытками */
    LED_STATUS_WIFI_AP,           /* синий: режим точки доступа */
    LED_STATUS_OTA,               /* фиолетовый моргает: идёт обновление */
} led_status_t;

/* Настраивает светодиод и запускает задачу индикации. */
void led_init(void);

/* Меняет отображаемый статус. Можно звать из любой задачи. */
void led_set_status(led_status_t status);

led_status_t led_get_status(void);

/* Отмечает сетевую активность: в режиме LED_STATUS_WIFI_CONNECTED
 * светодиод коротко гаснет. Можно звать из любой задачи. */
void led_notify_activity(void);

/* Число коротких гашений за цикл в режиме LED_STATUS_WIFI_AP
 * (например, количество подключённых клиентов). 0 — ровное свечение. */
void led_set_pulse_count(int count);

/* Яркость (1-255). Сохраняется в NVS и применяется сразу. */
void led_set_brightness(uint8_t brightness);

uint8_t led_get_brightness(void);
