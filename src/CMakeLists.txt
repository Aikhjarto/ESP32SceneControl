file(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.c)

# web/dist/*.gz are produced by scripts/embed_web.py, keys/ota_verify_key.bin by scripts/ota_sign.py
idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    REQUIRES nvs_flash spi_flash esp_driver_ledc esp_driver_mcpwm esp_driver_i2c esp_driver_gpio esp_driver_uart
             esp_timer esp_wifi esp_netif esp_event esp_http_server esp_http_client esp_app_format
             app_update mbedtls
    EMBED_FILES "../web/dist/index.html.gz" "../web/dist/app.js.gz" "../web/dist/style.css.gz"
                "../keys/ota_verify_key.bin"
)
