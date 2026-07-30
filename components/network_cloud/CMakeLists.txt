idf_component_register(SRCS "network.c" "supabase_client.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client esp_websocket_client espressif__esp_websocket_client esp_wifi nvs_flash esp-tls mbedtls board_config json mqtt)
