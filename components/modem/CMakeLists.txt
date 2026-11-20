idf_component_register(SRCS "modem.c" INCLUDE_DIRS "include"
                       REQUIRES esp_driver_uart PRIV_REQUIRES esp_timer log)
