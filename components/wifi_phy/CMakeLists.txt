idf_component_register(SRCS "wifi_phy.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mqttsn
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer lwip log)
