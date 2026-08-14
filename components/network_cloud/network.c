#include "network.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "config_manager.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>
#include "secrets.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_mac.h"

#include "network.h"
#include "config_manager.h"
#include "supabase_config.h"
#include "mqtt_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "mdns.h"

// UDP Logging
#include "lwip/sockets.h"
#include <stdarg.h>

static int g_udp_log_sock = -1;
static struct sockaddr_in g_udp_log_addr;
static vprintf_like_t s_original_vprintf = NULL;

#define LOG_PREFIX "[REC] " 

static int udp_log_vprintf(const char *fmt, va_list args) {
    char buf[512];
    int len = vsnprintf(buf, sizeof(buf)-1, fmt, args);
    if (len > 0 && g_udp_log_sock >= 0) {
        char packet[530];
        int plen = snprintf(packet, sizeof(packet), "%s%s", LOG_PREFIX, buf);
        sendto(g_udp_log_sock, packet, plen, MSG_DONTWAIT, (struct sockaddr *)&g_udp_log_addr, sizeof(g_udp_log_addr));
    }
    if (s_original_vprintf) {
        return s_original_vprintf(fmt, args);
    }
    return len;
}

static void start_udp_logging() {
    if (g_udp_log_sock >= 0) return; // Already started

    g_udp_log_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (g_udp_log_sock < 0) return;
    
    int flags = fcntl(g_udp_log_sock, F_GETFL, 0);
    fcntl(g_udp_log_sock, F_SETFL, flags | O_NONBLOCK);

    int opt_val = 1;
    setsockopt(g_udp_log_sock, SOL_SOCKET, SO_BROADCAST, &opt_val, sizeof(opt_val));

    g_udp_log_addr.sin_family = AF_INET;
    g_udp_log_addr.sin_port = htons(5555);
    g_udp_log_addr.sin_addr.s_addr = inet_addr("255.255.255.255"); // Broadcast
    
    s_original_vprintf = esp_log_set_vprintf(udp_log_vprintf);
    ESP_LOGI("UDP_LOG", "Started UDP Broadcast Logging on port 5555");
}


static const char *TAG = "NETWORK";
static app_config_t s_app_cfg;
static int s_retry_num = 0;
static volatile bool g_wifi_connected = false;
#define MAXIMUM_RETRY 5

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGE(TAG, "Wi-Fi Disconnected. Reason: %d", disconnected->reason);
        g_wifi_connected = false;
        if (s_retry_num < MAXIMUM_RETRY) {
            s_retry_num++;
            ESP_LOGI(TAG, "Wi-Fi disconnected. Reconnecting (%d/%d)...", s_retry_num, MAXIMUM_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "==================================================================");
            ESP_LOGW(TAG, " Wi-Fi connection failed! Device is waiting for Serial USB Config.");
            ESP_LOGW(TAG, " Plug USB cable into PC & open serial_config.html or config_tool.py");
            ESP_LOGW(TAG, "==================================================================");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        s_retry_num = 0;
        g_wifi_connected = true;
        ESP_LOGI(TAG, "=================================================");
        ESP_LOGI(TAG, "CONNECTED! IP ADDRESS: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "=================================================");
    }
}

bool network_is_connected(void) {
    return g_wifi_connected;
}

void network_init(void) {
    ESP_LOGI(TAG, "Initializing Wi-Fi configuration...");
    
    // Khởi tạo NVS Flash (Rất quan trọng vì esp_wifi_init cần NVS)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 1. Initialize config manager and load parameters
    config_manager_init();
    config_manager_get(&s_app_cfg);

    // 2. Start Serial USB Config Listener Task (listening on USB cable 24/7)
    config_manager_start_serial_listener();

    // 3. Setup Wi-Fi Station
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, s_app_cfg.wifi_ssid, 32);
    strncpy((char *)wifi_config.sta.password, s_app_cfg.wifi_pass, 64);
    
    ESP_LOGI(TAG, "Connecting to target WiFi SSID: '%s'...", s_app_cfg.wifi_ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Initialize Network Services outside of the Wi-Fi Event Handler
    // to prevent stack overflow in the small sys_evt task
    mqtt_app_start();

    ESP_LOGI(TAG, "Initializing SNTP to sync time...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    setenv("TZ", "VST-7", 1);
    tzset();

    int retry = 0;
    const int retry_count = 5;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry <= retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    
    time_t now = 0;
    struct tm timeinfo = { 0 };
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year < (2024 - 1900)) {
        ESP_LOGW(TAG, "Time could not be synchronized.");
    } else {
        char strftime_buf[64];
        strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
        ESP_LOGI(TAG, "The current date/time is: %s", strftime_buf);
    }

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("aiot-cam");
        mdns_instance_name_set("AIoT Receptionist Camera");
        ESP_LOGI(TAG, "mDNS initialized. You can access UI via http://aiot-cam.local/");
    }

    start_udp_logging();
}

void network_send_discord_alert(const uint8_t *image_buffer, size_t image_len, const char *message) {
    if (strlen(s_app_cfg.discord_webhook) == 0 || strcmp(s_app_cfg.discord_webhook, "YOUR_DISCORD_WEBHOOK_URL") == 0) {
        ESP_LOGW(TAG, "Discord Webhook URL not configured. Skipping alert.");
        return;
    }
    ESP_LOGI(TAG, "Sending Discord Alert to URL: %s - Message: %s", s_app_cfg.discord_webhook, message);
}

void network_send_access_log(const char *id, const char *status) {
    if (strlen(s_app_cfg.dashboard_url) == 0 || strcmp(s_app_cfg.dashboard_url, "http://YOUR_SERVER_IP/api/logs") == 0) {
        ESP_LOGW(TAG, "Web Dashboard URL not configured. Skipping access log.");
        return;
    }
    ESP_LOGI(TAG, "Sending Access Log to URL: %s - ID: %s, Status: %s", s_app_cfg.dashboard_url, id, status);
}

bool network_hf_process_audio(const uint8_t *audio_in, size_t in_len, 
                              char *out_guest_text, size_t max_text_len,
                              uint8_t *audio_out, size_t *out_len, size_t max_out_len) {
    if (strlen(s_app_cfg.hf_space_url) == 0 || strcmp(s_app_cfg.hf_space_url, "https://your-space-name.hf.space/api/dialogue") == 0) {
        ESP_LOGW(TAG, "Hugging Face Space API URL not configured. Using Mock response.");
        snprintf(out_guest_text, max_text_len, "[MOCK] Bạn chưa cấu hình HF Space URL.");
        *out_len = 0;
        return false;
    }

    ESP_LOGI(TAG, "Sending %d bytes of audio to Hugging Face API URL: %s", in_len, s_app_cfg.hf_space_url);
    snprintf(out_guest_text, max_text_len, "Tôi là shipper giao hàng.");
    *out_len = 16000; 
    if (*out_len > max_out_len) *out_len = max_out_len;
    memset(audio_out, 0, *out_len);
    return true;
}

/* =============================================================================
 * [DEPRECATED] WebSocket Code Commented Out per User Request
 * =============================================================================
volatile bool g_ai_reply_complete = false;
static void websocket_event_handler(...) {}
void* hf_ws_connect(void) {}
void hf_ws_disconnect(void *client_handle) {}
void hf_ws_send_text(void *client_handle, const char *text) {}
esp_err_t upload_image_to_hf(const char* filepath) {}
esp_err_t upload_audio_to_hf(const char* filepath) {}
esp_err_t upload_audio_to_hf_buffer(const uint8_t *pcm_buf, size_t pcm_size) {}
============================================================================= */

// =============================================================================
// upload_image_to_hf:
//   Gửi ảnh JPG qua HTTP POST lên Hugging Face (Endpoint /api/upload-image)
// =============================================================================
esp_err_t upload_image_to_hf(const char* filepath, const char* event_type) {
    ESP_LOGI(TAG, "[IMG] Uploading %s to HF via HTTP POST (Event: %s)...", filepath, event_type ? event_type : "unknown");

    struct stat st;
    if (stat(filepath, &st) != 0 || st.st_size == 0) {
        ESP_LOGE(TAG, "[IMG] File not found or empty: %s", filepath);
        return ESP_FAIL;
    }
    size_t img_size = (size_t)st.st_size;

    uint8_t *img_buf = (uint8_t *)heap_caps_malloc(img_size, MALLOC_CAP_SPIRAM);
    if (!img_buf) img_buf = (uint8_t *)malloc(img_size);
    if (!img_buf) {
        ESP_LOGE(TAG, "[IMG] Not enough RAM for image buffer (%zu bytes)", img_size);
        return ESP_FAIL;
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) { free(img_buf); return ESP_FAIL; }
    size_t read_bytes = fread(img_buf, 1, img_size, f);
    fclose(f);
    if (read_bytes != img_size) {
        ESP_LOGE(TAG, "[IMG] Read incomplete");
        free(img_buf);
        return ESP_FAIL;
    }

    if (strlen(s_app_cfg.hf_space_url) == 0 || strcmp(s_app_cfg.hf_space_url, "https://your-space-name.hf.space/api/dialogue") == 0) {
        ESP_LOGE(TAG, "Hugging Face Space API URL not configured!");
        free(img_buf);
        return ESP_FAIL;
    }

    char upload_url[256];
    // Thay thế /api/dialogue hoặc /api/process-visitor thành /api/upload-image
    // Lấy base url
    snprintf(upload_url, sizeof(upload_url), "%s", s_app_cfg.hf_space_url);
    char *api_ptr = strstr(upload_url, "/api/");
    if (api_ptr) {
        strcpy(api_ptr, "/api/upload-image");
    }

    ESP_LOGI(TAG, "[IMG] Sending HTTP POST to %s (size: %zu bytes)", upload_url, img_size);

    esp_http_client_config_t config = {
        .url = upload_url,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    const char *boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_method(client, HTTP_METHOD_POST);

    const char *header_part_fmt = 
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"type\"\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"image_file\"; filename=\"snapshot.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n";
    char header_part[512];
    snprintf(header_part, sizeof(header_part), header_part_fmt, boundary, event_type ? event_type : "unknown", boundary);

    const char *footer_part_fmt = "\r\n--%s--\r\n";
    char footer_part[128];
    snprintf(footer_part, sizeof(footer_part), footer_part_fmt, boundary);

    int total_len = strlen(header_part) + img_size + strlen(footer_part);

    esp_err_t err = esp_http_client_open(client, total_len);
    if (err == ESP_OK) {
        esp_http_client_write(client, header_part, strlen(header_part));
        esp_http_client_write(client, (const char *)img_buf, img_size);
        esp_http_client_write(client, footer_part, strlen(footer_part));

        int content_length = esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP POST Image Status = %d, length = %d", status_code, content_length);
    } else {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
    }
    
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(img_buf);

    return err;
}

// =============================================================================
// network_upload_audio_to_cloud:
//   Gửi trực tiếp raw PCM buffer qua HTTP POST lên Hugging Face (Endpoint /api/process-visitor)
// =============================================================================
esp_err_t network_upload_audio_to_cloud(const uint8_t *pcm_buf, size_t pcm_size) {
    if (!pcm_buf || pcm_size == 0) return ESP_FAIL;

    if (strlen(s_app_cfg.hf_space_url) == 0 || strcmp(s_app_cfg.hf_space_url, "https://your-space-name.hf.space/api/dialogue") == 0) {
        ESP_LOGE(TAG, "Hugging Face Space API URL not configured!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[AUD] Sending HTTP POST to %s (size: %zu bytes PCM)", s_app_cfg.hf_space_url, pcm_size);

    esp_http_client_config_t config = {
        .url = s_app_cfg.hf_space_url,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    const char *boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_method(client, HTTP_METHOD_POST);

    const char *header_part_fmt = 
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"audio_file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    char header_part[256];
    snprintf(header_part, sizeof(header_part), header_part_fmt, boundary);

    const char *footer_part_fmt = "\r\n--%s--\r\n";
    char footer_part[128];
    snprintf(footer_part, sizeof(footer_part), footer_part_fmt, boundary);

    // Dummy WAV header for 16kHz 16-bit Mono
    uint8_t wav_header[44] = {
        'R', 'I', 'F', 'F',
        0, 0, 0, 0, 
        'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ',
        16, 0, 0, 0, 1, 0, 1, 0, 
        0x80, 0x3E, 0x00, 0x00, 
        0x00, 0x7D, 0x00, 0x00, 
        2, 0, 16, 0, 
        'd', 'a', 't', 'a',
        0, 0, 0, 0 
    };
    uint32_t data_len = pcm_size;
    uint32_t file_len = data_len + 36;
    memcpy(&wav_header[4], &file_len, 4);
    memcpy(&wav_header[40], &data_len, 4);

    int total_len = strlen(header_part) + sizeof(wav_header) + pcm_size + strlen(footer_part);

    esp_err_t err = esp_http_client_open(client, total_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_write(client, header_part, strlen(header_part));
    esp_http_client_write(client, (const char *)wav_header, sizeof(wav_header));
    esp_http_client_write(client, (const char *)pcm_buf, pcm_size);
    esp_http_client_write(client, footer_part, strlen(footer_part));

    ESP_LOGI(TAG, "[AUD] Data uploaded. Waiting for AI response...");

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "HTTP client fetch headers failed");
    } else {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %d", status_code, content_length);

        if (status_code == 200) {
            char read_buf[4096];
            int read_len;
            bool is_first_chunk = true;
            extern void audio_feed_ringbuffer(const uint8_t *data, size_t len);
            
            while ((read_len = esp_http_client_read(client, read_buf, sizeof(read_buf))) > 0) {
                int offset = 0;
                if (is_first_chunk && read_len >= 44) {
                    if (strncmp(read_buf, "RIFF", 4) == 0) {
                        offset = 44;
                        ESP_LOGI(TAG, "Skipped 44 bytes WAV header in AI response");
                    }
                    is_first_chunk = false;
                }
                if (read_len - offset > 0) {
                    audio_feed_ringbuffer((const uint8_t *)(read_buf + offset), read_len - offset);
                }
            }
            ESP_LOGI(TAG, "[AUD] Finished streaming AI reply to ringbuffer");
        } else {
            ESP_LOGE(TAG, "Cloud API returned error %d", status_code);
        }
    }
    
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return ESP_OK;
}

