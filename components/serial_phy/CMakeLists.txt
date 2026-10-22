idf_component_register(SRCS "serial_phy.c" "serial_frame.c" "rs485_frame.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mqttsn
                       PRIV_REQUIRES esp_driver_uart esp_timer log)
