set(srcs "main.c" "bt_usr.c" "slcan.c")
idf_component_register(SRCS ${srcs}
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES bt nvs_flash esp_driver_twai esp_driver_uart)
