#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include "secrets.h"
#include <string.h>

typedef struct {
    char wifi_ssid[32];
    char wifi_pass[64];
    char discord_webhook[256];
    char hf_space_url[256];
    char dashboard_url[256];
} app_config_t;

static inline void config_manager_init(void) {}

static inline void config_manager_get(app_config_t *cfg) {
    strncpy(cfg->wifi_ssid, WIFI_SSID, sizeof(cfg->wifi_ssid));
    strncpy(cfg->wifi_pass, WIFI_PASS, sizeof(cfg->wifi_pass));
    strncpy(cfg->discord_webhook, DISCORD_WEBHOOK_URL, sizeof(cfg->discord_webhook));
    strncpy(cfg->hf_space_url, HF_SPACE_API_URL, sizeof(cfg->hf_space_url));
    strncpy(cfg->dashboard_url, WEB_DASHBOARD_URL, sizeof(cfg->dashboard_url));
}

static inline void config_manager_start_serial_listener(void) {}

#endif
