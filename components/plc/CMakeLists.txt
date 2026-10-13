idf_component_register(SRCS "plc.c" "plc_api.c" "plc_io.c" "plc_io_esp32.c" "plc_store.c" "plc_vm.c"
                       INCLUDE_DIRS "include"
                       REQUIRES objdict
                       PRIV_REQUIRES mqttsn esp_partition esp_timer nvs_flash log
                                     esp_driver_gpio esp_adc esp_driver_dac esp_driver_ledc
                                     esp_driver_pcnt esp_driver_i2c esp_driver_uart)
