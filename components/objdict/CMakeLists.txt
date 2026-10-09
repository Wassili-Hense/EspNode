idf_component_register(SRCS "objdict.c" "od_pack.c" "od_storage.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mqttsn
                       PRIV_REQUIRES nvs_flash log)
