#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include "secrets.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

typedef struct {
    char wifi_ssid[32];
    char wifi_pass[64];
    char discord_webhook[256];
    char hf_space_url[256];
    char dashboard_url[256];
} app_config_t;

static inline void config_manager_init(void) {}

static inline void config_manager_get(app_config_t *cfg) {
    // Default values from secrets.h
    strncpy(cfg->wifi_ssid, WIFI_SSID, sizeof(cfg->wifi_ssid));
    strncpy(cfg->wifi_pass, WIFI_PASS, sizeof(cfg->wifi_pass));
    strncpy(cfg->discord_webhook, DISCORD_WEBHOOK_URL, sizeof(cfg->discord_webhook));
    strncpy(cfg->hf_space_url, HF_SPACE_API_URL, sizeof(cfg->hf_space_url));
    strncpy(cfg->dashboard_url, WEB_DASHBOARD_URL, sizeof(cfg->dashboard_url));

    // Try reading WiFi config from NVS
    nvs_handle_t my_handle;
    if (nvs_open("wifi_cfg", NVS_READONLY, &my_handle) == ESP_OK) {
        size_t required_size;
        
        required_size = sizeof(cfg->wifi_ssid);
        if (nvs_get_str(my_handle, "ssid", cfg->wifi_ssid, &required_size) == ESP_OK) {
            ESP_LOGI("CONFIG", "Loaded SSID from NVS: %s", cfg->wifi_ssid);
        }

        required_size = sizeof(cfg->wifi_pass);
        if (nvs_get_str(my_handle, "pass", cfg->wifi_pass, &required_size) == ESP_OK) {
            ESP_LOGI("CONFIG", "Loaded Password from NVS");
        }
        nvs_close(my_handle);
    }
}

static inline void config_manager_save_wifi(const char *ssid, const char *pass) {
    nvs_handle_t my_handle;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_str(my_handle, "ssid", ssid);
        nvs_set_str(my_handle, "pass", pass);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI("CONFIG", "Saved new Wi-Fi credentials to NVS");
    }
}

static inline void config_manager_start_serial_listener(void) {}

#endif
