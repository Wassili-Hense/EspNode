idf_component_register(SRCS "od_clock.c" "objdict.c" "od_pack.c" "od_storage.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mqttsn
                       PRIV_REQUIRES esp_timer nvs_flash log)
