#include "network.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include <string.h>

static const char *TAG = "NETWORK";

void network_init(void) {
    ESP_LOGI(TAG, "Initializing Wi-Fi...");
    
    // 1. Initialize NVS (required for Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Wi-Fi Station setup
    /*
     * Implementation Pseudocode:
     * esp_netif_init();
     * esp_event_loop_create_default();
     * esp_netif_create_default_wifi_sta();
     * wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
     * esp_wifi_init(&cfg);
     * 
     * wifi_config_t wifi_config = {
     *     .sta = {
     *         .ssid = WIFI_SSID,
     *         .password = WIFI_PASS,
     *     },
     * };
     * esp_wifi_set_mode(WIFI_MODE_STA);
     * esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
     * esp_wifi_start();
     */
}

void network_send_discord_alert(const uint8_t *image_buffer, size_t image_len, const char *message) {
    ESP_LOGI(TAG, "Sending Discord Alert: %s", message);
    /*
     * Implementation Pseudocode (Multipart HTTP POST):
     * esp_http_client_config_t config = {
     *    .url = DISCORD_WEBHOOK_URL,
     * };
     * esp_http_client_handle_t client = esp_http_client_init(&config);
     * 
     * // Construct multipart/form-data payload with image_buffer and message
     * esp_http_client_set_method(client, HTTP_METHOD_POST);
     * esp_http_client_set_header(client, "Content-Type", "multipart/form-data; boundary=boundary");
     * 
     * esp_http_client_perform(client);
     * esp_http_client_cleanup(client);
     */
}

void network_send_access_log(const char *id, const char *status) {
    ESP_LOGI(TAG, "Sending Access Log - ID: %s, Status: %s", id, status);
    /*
     * Implementation Pseudocode:
     * char post_data[128];
     * snprintf(post_data, sizeof(post_data), "{\"id\":\"%s\",\"status\":\"%s\"}", id, status);
     * // HTTP POST to WEB_DASHBOARD_URL with Content-Type: application/json
     */
}

bool network_hf_process_audio(const uint8_t *audio_in, size_t in_len, 
                              char *out_guest_text, size_t max_text_len,
                              uint8_t *audio_out, size_t *out_len, size_t max_out_len) {
    ESP_LOGI(TAG, "Sending %d bytes of audio to Hugging Face API: %s", in_len, HF_SPACE_API_URL);
    
    /*
     * Pseudocode for Hugging Face Space API call:
     * 1. Create HTTP POST request to HF_SPACE_API_URL.
     * 2. Set Content-Type to "audio/wav" or "application/octet-stream".
     * 3. Send audio_in data.
     * 4. Receive JSON response containing {"guest_text": "...", "ai_audio_url": "..."} or raw audio bytes.
     * 5. Parse JSON to extract out_guest_text.
     * 6. Download or copy audio response to audio_out buffer.
     * 7. Set *out_len to the size of the received audio.
     */
     
    // MOCK IMPLEMENTATION for testing:
    snprintf(out_guest_text, max_text_len, "Tôi là shipper giao hàng.");
    
    // Simulate receiving some audio data back
    *out_len = 16000; 
    if (*out_len > max_out_len) *out_len = max_out_len;
    memset(audio_out, 0, *out_len); // dummy audio bytes
    
    ESP_LOGI(TAG, "HF API Success! AI responded with %d bytes of audio.", *out_len);
    return true;
}
