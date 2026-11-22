idf_component_register(SRCS "cellular_net.c" INCLUDE_DIRS "include"
                       REQUIRES esp_driver_uart
                       PRIV_REQUIRES esp_http_client esp_netif esp_timer lwip mbedtls)
