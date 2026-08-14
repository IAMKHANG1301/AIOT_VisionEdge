idf_component_register(SRCS "vision.cpp" "db_manager.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_http_server esp_timer esp_partition board_config hardware_control audio_dialogue network_cloud json espressif__esp32-camera espressif__esp-dl edge_impulse)

