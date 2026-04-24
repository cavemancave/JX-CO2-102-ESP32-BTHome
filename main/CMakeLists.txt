file(GLOB_RECURSE srcs
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
)

idf_component_register(SRCS "${srcs}"
                       PRIV_REQUIRES bt nvs_flash esp_driver_uart esp_driver_gpio esp_timer
                       INCLUDE_DIRS "./include")
