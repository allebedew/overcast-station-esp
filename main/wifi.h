#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    WIFI_STATUS_DISCONNECTED,  /* не подключено */
    WIFI_STATUS_CONNECTING,    /* активная попытка подключения */
    WIFI_STATUS_WAITING_RETRY, /* пауза перед следующей попыткой */
    WIFI_STATUS_CONNECTED,     /* подключено, IP получен */
    WIFI_STATUS_FAILED,        /* попытки исчерпаны */
    WIFI_STATUS_AP_MODE,       /* работаем как точка доступа */
} wifi_status_t;

typedef struct {
    char ssid[33];
    int rssi;
    int authmode; /* wifi_auth_mode_t */
} wifi_scan_ap_t;

typedef void (*wifi_status_cb_t)(wifi_status_t status);

/* Регистрирует колбэк на смену статуса. Вызывать до wifi_connect().
 * Колбэк приходит из системных задач (sys_evt, esp_timer) —
 * внутри нельзя блокироваться. */
void wifi_set_status_callback(wifi_status_cb_t cb);

/* Запускает подключение (STA) и сразу возвращает управление.
 * Сохранённые сети перебираются по кругу; после исчерпания
 * попыток устройство само переходит в режим точки доступа. */
esp_err_t wifi_connect(void);

/* Переключает в режим точки доступа. */
void wifi_start_ap(void);

/* Возвращает в режим станции и заново запускает перебор сетей. */
void wifi_reconnect(void);

wifi_status_t wifi_get_status(void);

/* Число клиентов, подключённых к нашей точке доступа (0 вне режима AP).
 * При изменении числа клиентов вызывается статусный колбэк. */
int wifi_get_ap_client_count(void);

/* SSID сети: в режиме станции — текущей, в режиме AP — собственный. */
const char *wifi_get_ssid(void);

/* Уровень сигнала точки доступа в дБм; 0, если не подключены как станция. */
int wifi_get_rssi(void);

/* Текущий Wi-Fi канал (в любом режиме); 0, если радио не запущено. */
int wifi_get_channel(void);

/* Блокирующее сканирование эфира (~2-3 с). Возвращает число найденных
 * сетей (без дублей SSID) или -1 при ошибке. */
int wifi_scan(wifi_scan_ap_t *out, int max_count);
