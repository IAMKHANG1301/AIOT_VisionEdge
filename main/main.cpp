
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

// Custom components
#include "hardware.h"
#include "vision.h"
#include "audio.h"
#include "network.h"
#include "mqtt_service.h"

static const char *TAG = "MAIN";

#if 0
// =============================================================================
// Phase 0 Hardware Test Task
//   Runs once after boot to validate Camera, Mic, and Speaker.
//   Tests run sequentially with clear PASS/FAIL logs.
//   Remove or disable this task when moving to Phase 1 integration.
// =============================================================================
static void hardware_test_task(void *arg) {
    // Give system 2s to fully stabilize before starting tests
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "   PHASE 0 HARDWARE VALIDATION TESTS");
    ESP_LOGI(TAG, "============================================");

    // ------------------------------------------------------------------
    // TEST 1: Camera → already validated by vision_start_web_server()
    //   Open http://<IP>/ in browser and click camera preview
    // ------------------------------------------------------------------
    ESP_LOGI(TAG, "[TEST 1] Camera: open http://<IP>/ in browser → verify image shows");

    // ------------------------------------------------------------------
    // TEST 2: Speaker — Play 440 Hz sine tone for 2 seconds
    // ------------------------------------------------------------------
    ESP_LOGI(TAG, "[TEST 2] Speaker: playing 440 Hz sine wave...");
    audio_test_play_sine();
    vTaskDelay(pdMS_TO_TICKS(500));

    // ------------------------------------------------------------------
    // TEST 3: Microphone → Record 3 seconds to SD card
    // ------------------------------------------------------------------
    ESP_LOGI(TAG, "[TEST 3] Mic: recording 3s to /sdcard/test_mic.raw");
    ESP_LOGI(TAG, "         >>> SPEAK INTO THE MICROPHONE NOW <<<");
    audio_test_record_to_sd();

    // ------------------------------------------------------------------
    // TEST 4: Playback the recorded mic audio through the speaker
    // ------------------------------------------------------------------
    ESP_LOGI(TAG, "[TEST 4] Playback: playing back recorded mic audio...");
    const int BUF_SIZE = 16000 * 3 * 2;   // 3s at 16kHz 16-bit mono
    uint8_t *playback_buf = (uint8_t *)heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (playback_buf) {
        FILE *f = fopen("/sdcard/test_mic.raw", "rb");
        if (f) {
            size_t read_bytes = fread(playback_buf, 1, BUF_SIZE, f);
            fclose(f);
            if (read_bytes > 0) {
                audio_play(playback_buf, (int)read_bytes);
                ESP_LOGI(TAG, "[TEST 4] Playback complete — did you hear your voice?");
            } else {
                ESP_LOGE(TAG, "[TEST 4] FAIL: Could not read /sdcard/test_mic.raw");
            }
        } else {
            ESP_LOGE(TAG, "[TEST 4] FAIL: /sdcard/test_mic.raw not found. Did TEST 3 pass?");
        }
        free(playback_buf);
    } else {
        ESP_LOGE(TAG, "[TEST 4] FAIL: Not enough PSRAM to allocate playback buffer");
    }

    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "   PHASE 1 PERIPHERAL VALIDATION TESTS");
    ESP_LOGI(TAG, "============================================");

    // ------------------------------------------------------------------
    // TEST 5: Sonar HC-SR04 distance readings
    //   Expected: Output changes as you move your hand in front of sensor
    // ------------------------------------------------------------------
    ESP_LOGI(TAG, "[TEST 5] Sonar: Reading distance 5 times...");
    for (int i = 0; i < 5; i++) {
        uint32_t dist = get_sonar_distance_cm();
        ESP_LOGI(TAG, "         Read %d: Distance = %u cm", i + 1, (unsigned int)dist);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // ------------------------------------------------------------------
    // TEST 6: Relay (Solenoid Lock control)
    //   Expected: Click sound from Relay, GPIO output goes to active state for 5s
    // ------------------------------------------------------------------
    ESP_LOGI(TAG, "[TEST 6] Relay: Activating solenoid lock for 5s (GPIO %d)...", RELAY_PIN);
    open_door();
    vTaskDelay(pdMS_TO_TICKS(500));

    // ------------------------------------------------------------------
    // TEST 7: Read matrix keypad for 10 seconds
    //   Expected: Pressed keys are logged to console
    // ------------------------------------------------------------------
    ESP_LOGI(TAG, "[TEST 7] Keypad: Press keys now! Reading for 10s...");
    for (int i = 0; i < 100; i++) {
        char key = read_keypad();
        if (key != '\0') {
            ESP_LOGI(TAG, "         Key pressed: %c", key);
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // 100ms * 100 = 10s
    }

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "   TEST PHASE COMPLETE");
    ESP_LOGI(TAG, "============================================");
    vTaskDelete(NULL);
}
#endif

// TẠM THỜI VÔ HIỆU HÓA VOICE QUERY TẠI MẠCH CHÍNH (CHUYỂN SANG MẠCH PHỤ)
// =============================================================================
// static TaskHandle_t s_voice_task = NULL;
// 
// static void voice_query_task(void *arg)
// {
//     ESP_LOGI(TAG, "=== [BOOT] Voice Query: Bắt đầu ghi âm ===");
// 
//     const int buf_size = 16000 * 10 * sizeof(int16_t);
//     uint8_t *pcm_buf = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
//     if (!pcm_buf) pcm_buf = (uint8_t *)malloc(buf_size);
//     if (!pcm_buf) {
//         ESP_LOGE(TAG, "voice_query_task: Cannot allocate record buffer!");
//         s_voice_task = NULL;
//         vTaskDelete(NULL);
//         return;
//     }
// 
//     int bytes_recorded = audio_record(pcm_buf, buf_size);
// 
//     if (bytes_recorded <= 0) {
//         ESP_LOGW(TAG, "Ghi âm thất bại hoặc trống.");
//         free(pcm_buf);
//         s_voice_task = NULL;
//         vTaskDelete(NULL);
//         return;
//     }
// 
//     ESP_LOGI(TAG, "Ghi âm xong: %d bytes. Đang gửi lên Cloud...", bytes_recorded);
// 
//     esp_err_t ret = network_upload_audio_to_cloud(pcm_buf, (size_t)bytes_recorded);
// 
//     if (ret == ESP_OK) {
//         ESP_LOGI(TAG, "Cloud xử lý thành công.");
//     } else {
//         ESP_LOGE(TAG, "Gửi Cloud thất bại.");
//     }
// 
//     free(pcm_buf);
//     s_voice_task = NULL;
//     vTaskDelete(NULL);
// }

// =============================================================================
// app_main — Application Entry Point
// =============================================================================
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  AIoT Receptionist — PHASE 0 & 1 TEST MODE");
    ESP_LOGI(TAG, "=================================================");

    // 1. Hardware — Configures GPIOs for Relay, Buzzer, and Sonar
    hardware_init();

    // 2. Network — connects WiFi, provides IP for Web UI access
    network_init();
    
    // Register MQTT Callbacks
    mqtt_set_open_door_cb([]() {
        open_door();
        char mqtt_payload[128];
        snprintf(mqtt_payload, sizeof(mqtt_payload), "{\"action\":\"open_door\",\"status\":\"success\",\"message\":\"Cửa đã mở thành công\"}");
        mqtt_publish_status(mqtt_payload);
        ESP_LOGI(TAG, "Mở cửa qua MQTT thành công.");
    });
    mqtt_set_delete_face_cb([](const char* id) {
        vision_delete_enrolled_face(id);
    });
    mqtt_set_enroll_face_cb([](const char* name) {
        vision_trigger_remote_enroll(name);
    });

    // 3. SD Card — must be mounted before audio recording or face storage
    bool sd_ok = sd_card_init();
    if (!sd_ok) {
        ESP_LOGW(TAG, "SD Card mount failed — audio recording test will fail");
    }

    // 4. Audio I2S — initialize INMP441 + MAX98357A full-duplex
    // Tạm vô hiệu hóa Audio trên mạch chính để trả lại GPIO 1 và GPIO 48 cho UART
    // audio_init();

    // 5. Vision — initialize OV2640 camera + AI models + HTTP Web UI
    if (vision_init()) {
        vision_start_web_server();
        ESP_LOGI(TAG, "Vision + Web Server OK. Go to http://<IP>/ to test camera.");
    } else {
        ESP_LOGE(TAG, "Vision init FAILED — camera may not be connected via FPC.");
    }

    // 6. UART liên mạch
    // Mạch Chính TX: GPIO 1  → Mạch Phụ RX: GPIO 35
    // Mạch Chính RX: GPIO 48 ← Mạch Phụ TX: GPIO 36
    // (Bỏ qua cấu hình UART nếu bạn muốn test độc lập nút bấm trên mạch chính trước)
    
    // Nút BOOT (GPIO_NUM_0)
    gpio_reset_pin(GPIO_NUM_0);
    gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_0, GPIO_PULLUP_ONLY);
    int last_button_state = 1;

    // Vòng lặp chính — quét nút BOOT để ghi âm Push-To-Talk (VÔ HIỆU HÓA TRÊN MẠCH CHÍNH)
    while (1) {
        /*
        int current_state = gpio_get_level(GPIO_NUM_0);
        if (current_state == 0 && last_button_state == 1) {
            vTaskDelay(pdMS_TO_TICKS(50)); // debounce
            if (gpio_get_level(GPIO_NUM_0) == 0) {
                if (s_voice_task == NULL) {
                    xTaskCreate(voice_query_task, "voice_query", 8192, NULL, 5, &s_voice_task);
                } else {
                    ESP_LOGI(TAG, "Đang xử lý, vui lòng đợi...");
                }
                while(gpio_get_level(GPIO_NUM_0) == 0) { 
                    vTaskDelay(pdMS_TO_TICKS(10)); 
                }
            }
        }
        last_button_state = current_state;
        */
        vTaskDelay(pdMS_TO_TICKS(100)); // Nhường CPU
    }
}
