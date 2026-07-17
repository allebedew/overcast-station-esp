idf_component_register(SRCS "main.c" "wifi.c" "wifi_store.c" "led.c" "button.c"
                            "webserver.c" "sensors.c" "settings.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash
                                     esp_http_server esp_driver_tsens
                                     esp_app_format spi_flash json
                       EMBED_TXTFILES "index.html")
