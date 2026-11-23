idf_component_register(SRCS "cellular_net.cpp" INCLUDE_DIRS "include"
                       REQUIRES esp_driver_uart esp_netif
                       PRIV_REQUIRES esp_http_client esp_timer lwip mbedtls)
