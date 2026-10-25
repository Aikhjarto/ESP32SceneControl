file(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.c)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_driver_ledc esp_driver_mcpwm esp_driver_i2c esp_driver_gpio
             esp_timer esp_wifi esp_netif esp_event esp_http_server
)
