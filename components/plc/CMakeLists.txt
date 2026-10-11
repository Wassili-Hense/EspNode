idf_component_register(SRCS "plc.c" "plc_api.c" "plc_store.c" "plc_vm.c"
                       INCLUDE_DIRS "include"
                       REQUIRES objdict
                       PRIV_REQUIRES mqttsn esp_partition esp_timer nvs_flash log)
