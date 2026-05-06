idf_component_register(SRCS "cellular.c" "main.c" "CAN.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_twai esp_driver_uart esp_timer)
