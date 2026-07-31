#pragma once

/* Starts the HTTP server and mDNS (http://weather.local). Call after
 * wifi_connect() and after every module the handlers read from. */
void webserver_start(void);
