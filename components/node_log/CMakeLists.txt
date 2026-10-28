idf_component_register(SRCS "node_log.c" "node_log_esp32.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mqttsn
                       PRIV_REQUIRES log freertos)
