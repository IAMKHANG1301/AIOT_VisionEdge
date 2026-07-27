idf_component_register(SRCS "vision.cpp" "test_interface.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_http_server esp_timer esp_partition board_config hardware_control audio_dialogue db_manager json network_cloud)

