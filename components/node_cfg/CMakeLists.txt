idf_component_register(SRCS "node_cfg.c" "node_cfg_esp32.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_partition log)
