idf_component_register(SRCS "main.c" "wifi.c" "wifi_store.c" "button.c"
                            "settings.c" "sysinfo.c" "ota.c"
                            "timesync.c" "telegram.c" "alerts.c" "history.c"
                            "forecast.c"
                            "storage.c" "weather_api.c" "weather_store.c"
                            # what the station shows: the status LED, the
                            # display's layouts and the panel's transport
                            "ui/led.c" "ui/screen_16x2.c" "ui/lcd1602_rgb.c"
                            # what it serves: the HTTP API and the page in
                            # web/index.html
                            "web/webserver.c"
                            # everything on the I2C bus: the polling task, the
                            # room-level view over it, the bus itself and one
                            # transport file per device
                            "sensors/sensors.c" "sensors/climate.c"
                            "sensors/i2c_bus.c"
                            "sensors/i2c_dev.c" "sensors/scd40.c"
                            "sensors/tmp117.c" "sensors/bmp581.c"
                            "sensors/veml7700.c"
                       INCLUDE_DIRS "." "sensors" "ui" "web"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash
                                     esp_http_server esp_http_client mbedtls
                                     esp_driver_tsens esp_driver_i2c
                                     esp_app_format spi_flash json app_update)

# Web assets are embedded pre-compressed and served with Content-Encoding: gzip
# (index.html: 107 KB -> 20 KB), which shrinks every OTA image and the page load
# over Wi-Fi. Compression happens at build time, so only the plain sources are
# in git. The loop takes more assets as they appear.
find_program(GZIP_EXE gzip REQUIRED)

foreach(asset "web/index.html")
    # Compressed into the build tree by base name, so the embedded symbol stays
    # _binary_<name>_gz_start no matter which subdirectory the asset lives in.
    get_filename_component(asset_name "${asset}" NAME)
    # gzip -f writes <asset>.gz and removes its input, so compress a copy inside
    # the build tree; -n drops name/timestamp to keep the output reproducible.
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${asset_name}.gz"
        COMMAND "${CMAKE_COMMAND}" -E copy "${COMPONENT_DIR}/${asset}"
                                           "${CMAKE_CURRENT_BINARY_DIR}/${asset_name}"
        COMMAND "${GZIP_EXE}" -9 -n -f "${CMAKE_CURRENT_BINARY_DIR}/${asset_name}"
        DEPENDS "${COMPONENT_DIR}/${asset}"
        COMMENT "Compressing ${asset}"
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/${asset_name}.gz" BINARY)
endforeach()
