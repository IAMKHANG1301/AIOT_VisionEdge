idf_component_register(SRCS "hardware.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_timer fatfs sdmmc board_config)
