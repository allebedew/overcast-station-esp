#pragma once

/* Запускает HTTP-сервер, mDNS (http://weather.local) и датчик
 * температуры чипа. Вызывать после wifi_connect(). */
void webserver_start(void);
