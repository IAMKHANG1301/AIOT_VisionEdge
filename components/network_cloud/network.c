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


static const char *TAG = "NETWORK";
static app_config_t s_app_cfg;
static int s_retry_num = 0;
static volatile bool g_wifi_connected = false;
#define MAXIMUM_RETRY 10

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
        
        static bool s_mqtt_started = false;
        if (!s_mqtt_started) {
            mqtt_app_start();
            s_mqtt_started = true;
        }
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
            .threshold.rssi = -127,
            .scan_method = WIFI_FAST_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, s_app_cfg.wifi_ssid, 32);
    strncpy((char *)wifi_config.sta.password, s_app_cfg.wifi_pass, 64);
    
    ESP_LOGI(TAG, "Connecting to target WiFi SSID: '%s'...", s_app_cfg.wifi_ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
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


volatile bool g_ai_reply_complete = false;

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 0x01 && data->data_len > 0) { // Text frame
                ESP_LOGI(TAG, "[WS_RECV] JSON: %.*s", data->data_len, (char *)data->data_ptr);
                if (strstr((char *)data->data_ptr, "\"action\":\"complete\"") != NULL || strstr((char *)data->data_ptr, "\"action\": \"complete\"") != NULL) {
                    g_ai_reply_complete = true;
                }
            } else if ((data->op_code == 0x02 || data->op_code == 0x00) && data->data_len > 0) { // Binary or Continuation
                // Buffer the remainder byte if length is odd to prevent byte-shifting (which causes loud static)
                static uint8_t remainder_byte = 0;
                static bool has_remainder = false;
                
                int total_len = data->data_len + (has_remainder ? 1 : 0);
                uint8_t *aligned_buf = malloc(total_len);
                if (aligned_buf) {
                    int offset = 0;
                    if (has_remainder) {
                        aligned_buf[0] = remainder_byte;
                        offset = 1;
                        has_remainder = false;
                    }
                    memcpy(aligned_buf + offset, data->data_ptr, data->data_len);
                    
                    if (total_len % 2 != 0) {
                        remainder_byte = aligned_buf[total_len - 1];
                        has_remainder = true;
                        total_len--; // Process only the even part
                    }
                    
                    extern void audio_play_chunk(const uint8_t *data, int len);
                    audio_play_chunk(aligned_buf, total_len);
                    free(aligned_buf);
                }
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "[WS_RECV] Error event!");
            break;
        default:
            break;
    }
}

// =============================================================================
// Helper: Tạo WebSocket client kết nối tới HF_WS_URL
// =============================================================================
void* hf_ws_connect(void) {
    esp_websocket_client_config_t ws_cfg = {
        .uri                      = HF_WS_URL,
        .buffer_size              = 4096,
        .crt_bundle_attach        = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .reconnect_timeout_ms     = 10000,
        .network_timeout_ms       = 30000,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    if (!client) {
        ESP_LOGE(TAG, "WS init failed");
        return NULL;
    }
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);
    if (esp_websocket_client_start(client) != ESP_OK) {
        ESP_LOGE(TAG, "WS start failed");
        esp_websocket_client_destroy(client);
        return NULL;
    }
    // Chờ kết nối tối đa 10s
    for (int i = 0; i < 100 && !esp_websocket_client_is_connected(client); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!esp_websocket_client_is_connected(client)) {
        ESP_LOGE(TAG, "WS connection timeout");
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        return NULL;
    }
    ESP_LOGI(TAG, "WS connected to HF Space");
    return client;
}

void hf_ws_disconnect(void *client_handle) {
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)client_handle;
    vTaskDelay(pdMS_TO_TICKS(500)); // chờ server xử lý
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
}

void hf_ws_send_text(void *client_handle, const char *text) {
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)client_handle;
    esp_websocket_client_send_text(client, text, strlen(text), portMAX_DELAY);
}

// =============================================================================
// upload_image_to_hf:
//   Gửi ảnh JPG qua WS dưới dạng JSON:
//   {"action":"metadata","image_b64":"<base64 string>"}
// =============================================================================
esp_err_t upload_image_to_hf(const char* filepath) {
    ESP_LOGI(TAG, "[IMG] Uploading %s to HF via WS JSON...", filepath);

    // Đọc file vào bộ nhớ
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

    // Base64 encode
    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, img_buf, img_size); // lấy kích thước cần
    char *b64_buf = (char *)heap_caps_malloc(b64_len + 1, MALLOC_CAP_SPIRAM);
    if (!b64_buf) b64_buf = (char *)malloc(b64_len + 1);
    if (!b64_buf) {
        ESP_LOGE(TAG, "[IMG] Not enough RAM for base64 buffer (%zu bytes)", b64_len);
        free(img_buf);
        return ESP_FAIL;
    }
    mbedtls_base64_encode((unsigned char*)b64_buf, b64_len + 1, &b64_len, img_buf, img_size);
    b64_buf[b64_len] = '\0';
    free(img_buf);

    // Tạo JSON payload: {"action":"metadata","image_b64":"..."}
    // Prefix + suffix: ~30 chars overhead
    size_t json_len = b64_len + 40;
    char *json_buf = (char *)heap_caps_malloc(json_len, MALLOC_CAP_SPIRAM);
    if (!json_buf) json_buf = (char *)malloc(json_len);
    if (!json_buf) {
        ESP_LOGE(TAG, "[IMG] Not enough RAM for JSON buffer");
        free(b64_buf);
        return ESP_FAIL;
    }
    snprintf(json_buf, json_len, "{\"action\":\"metadata\",\"image_b64\":\"%s\"}", b64_buf);
    free(b64_buf);

    // Gửi qua WebSocket
    esp_websocket_client_handle_t client = hf_ws_connect();
    if (!client) {
        free(json_buf);
        return ESP_FAIL;
    }

    int result = esp_websocket_client_send_text(client, json_buf, strlen(json_buf), portMAX_DELAY);
    free(json_buf);

    if (result < 0) {
        ESP_LOGE(TAG, "[IMG] WS send failed");
        hf_ws_disconnect(client);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[IMG] Sent %d bytes JSON to HF, waiting for response...", result);

    // Gửi một chunk Audio giả (khoảng 1/4 giây im lặng) để lừa backend HF 
    // vì backend yêu cầu bắt buộc phải có "dữ liệu âm thanh" mới xử lý.
    size_t dummy_audio_size = 4096;
    uint8_t *dummy_audio = (uint8_t *)calloc(1, dummy_audio_size);
    if (dummy_audio) {
        esp_websocket_client_send_bin(client, (const char *)dummy_audio, dummy_audio_size, portMAX_DELAY);
        free(dummy_audio);
        ESP_LOGI(TAG, "[IMG] Sent %zu bytes of dummy audio (silence).", dummy_audio_size);
    }

    // BẮT BUỘC: Gửi tín hiệu stop_audio để backend trên HF biết đã kết thúc phiên và bắt đầu xử lý ảnh
    const char *stop_cmd = "{\"action\":\"stop_audio\"}";
    esp_websocket_client_send_text(client, stop_cmd, strlen(stop_cmd), portMAX_DELAY);
    ESP_LOGI(TAG, "[IMG] Sent stop_audio signal. Waiting 3 seconds for backend to process...");

    // Chờ backend xử lý (backend thường trả về JSON hoặc Audio, nhưng ESP32 chỉ cần log hoặc upload thành công)
    vTaskDelay(pdMS_TO_TICKS(3000));

    hf_ws_disconnect(client);
    return ESP_OK;
}

// =============================================================================
// upload_audio_to_hf:
//   1. Kết nối WS
//   2. Gửi raw PCM bytes từ file WAV (bỏ qua 44-byte header)
//   3. Gửi JSON {"action":"stop_audio"} để báo server xử lý
// =============================================================================
esp_err_t upload_audio_to_hf(const char* filepath) {
    ESP_LOGI(TAG, "[AUD] Uploading %s to HF via WS raw PCM...", filepath);

    struct stat st;
    if (stat(filepath, &st) != 0 || st.st_size == 0) {
        ESP_LOGE(TAG, "[AUD] File not found or empty: %s", filepath);
        return ESP_FAIL;
    }
    size_t file_size = (size_t)st.st_size;

    // Đọc file WAV vào bộ nhớ (bỏ qua 44-byte WAV header -> chỉ gửi PCM)
    const size_t WAV_HEADER = 44;
    if (file_size <= WAV_HEADER) {
        ESP_LOGE(TAG, "[AUD] File too small to contain PCM data");
        return ESP_FAIL;
    }
    size_t pcm_size = file_size - WAV_HEADER;

    uint8_t *pcm_buf = (uint8_t *)heap_caps_malloc(pcm_size, MALLOC_CAP_SPIRAM);
    if (!pcm_buf) pcm_buf = (uint8_t *)malloc(pcm_size);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "[AUD] Not enough RAM for PCM buffer (%zu bytes)", pcm_size);
        return ESP_FAIL;
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) { free(pcm_buf); return ESP_FAIL; }
    fseek(f, WAV_HEADER, SEEK_SET); // bỏ WAV header
    size_t read_bytes = fread(pcm_buf, 1, pcm_size, f);
    fclose(f);

    if (read_bytes != pcm_size) {
        ESP_LOGE(TAG, "[AUD] Read incomplete: %zu/%zu", read_bytes, pcm_size);
        free(pcm_buf);
        return ESP_FAIL;
    }

    // Kết nối WS
    esp_websocket_client_handle_t client = hf_ws_connect();
    if (!client) {
        free(pcm_buf);
        return ESP_FAIL;
    }

    // Gửi raw PCM trong 1 binary message
    ESP_LOGI(TAG, "[AUD] Sending %zu bytes of raw PCM...", pcm_size);
    int result = esp_websocket_client_send_bin(client, (const char*)pcm_buf, pcm_size, portMAX_DELAY);
    free(pcm_buf);

    if (result < 0) {
        ESP_LOGE(TAG, "[AUD] WS send PCM failed");
        hf_ws_disconnect(client);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[AUD] PCM sent OK (%d bytes). Sending stop_audio signal...", result);

    // BẮT BUỘC: Thêm cờ toàn cục để biết khi nào Server gửi chữ "complete"
    extern volatile bool g_ai_reply_complete;
    g_ai_reply_complete = false;

    // Gửi tín hiệu stop
    const char *stop_msg = "{\"action\":\"stop_audio\"}";
    esp_websocket_client_send_text(client, stop_msg, strlen(stop_msg), pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "[AUD] stop_audio sent. Waiting for Server AI to process and return TTS Audio...");

    // Chờ server phản hồi (timeout 30s)
    int wait_cycles = 300; 
    while (!g_ai_reply_complete && wait_cycles > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_cycles--;
    }

    if (wait_cycles == 0) {
        ESP_LOGE(TAG, "[AUD] Timeout waiting for AI reply!");
    } else {
        ESP_LOGI(TAG, "[AUD] AI Reply Complete!");
    }

    hf_ws_disconnect(client);
    return ESP_OK;
}

// =============================================================================
// upload_audio_to_hf_buffer:
//   Gửi trực tiếp raw PCM buffer từ RAM lên HF
// =============================================================================
esp_err_t upload_audio_to_hf_buffer(const uint8_t *pcm_buf, size_t pcm_size) {
    if (!pcm_buf || pcm_size == 0) {
        return ESP_FAIL;
    }

    esp_websocket_client_handle_t client = hf_ws_connect();
    if (!client) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[AUD] Sending %zu bytes of raw PCM from RAM...", pcm_size);
    int result = esp_websocket_client_send_bin(client, (const char*)pcm_buf, pcm_size, portMAX_DELAY);

    if (result < 0) {
        ESP_LOGE(TAG, "[AUD] WS send PCM failed");
        hf_ws_disconnect(client);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[AUD] PCM sent OK (%d bytes). Sending stop_audio signal...", result);

    extern volatile bool g_ai_reply_complete;
    g_ai_reply_complete = false;

    const char *stop_msg = "{\"action\":\"stop_audio\"}";
    esp_websocket_client_send_text(client, stop_msg, strlen(stop_msg), pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "[AUD] stop_audio sent. Waiting for Server AI to process and return TTS Audio...");

    int wait_cycles = 300; 
    while (!g_ai_reply_complete && wait_cycles > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_cycles--;
    }

    if (wait_cycles == 0) {
        ESP_LOGE(TAG, "[AUD] Timeout waiting for AI reply!");
    } else {
        ESP_LOGI(TAG, "[AUD] AI Reply Complete!");
    }

    hf_ws_disconnect(client);
    return ESP_OK;
}

