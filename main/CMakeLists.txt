idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES mqttsn node_cfg objdict plc serial_phy nvs_flash esp_timer)

# Software version in the device type: git tag vX.Y.Z -> "XYZ" (X13: "320" = 3.2.0). PROJECT_VER is
# "git describe" of ESP-IDF; without such a tag CONFIG_ESPNODE_SW_VERSION stays
idf_build_get_property(project_ver PROJECT_VER)
if(project_ver MATCHES "^v([0-9])\\.([0-9])\\.([0-9])")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
                               "ESPNODE_SW_VERSION=\"${CMAKE_MATCH_1}${CMAKE_MATCH_2}${CMAKE_MATCH_3}\"")
endif()
