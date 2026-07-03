#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

// Include custom components
#include "hardware.h"
#include "vision.h"
#include "audio.h"
#include "network.h"

static const char *TAG = "MAIN";

// Buffer for audio processing
#define AUDIO_BUFFER_SIZE 32000
static uint8_t audio_buffer[AUDIO_BUFFER_SIZE];

// Passcode buffer for Keypad
#define PASSCODE_MAX_LEN 16
static char entered_code[PASSCODE_MAX_LEN] = {0};
static int code_idx = 0;
const char correct_passcode[] = "1234#"; // Open door code

void handle_keypad(char key) {
    if (key == '\0') return;

    ESP_LOGI(TAG, "Key received: %c", key);
    
    // Clear code on '*'
    if (key == '*') {
        memset(entered_code, 0, sizeof(entered_code));
        code_idx = 0;
        tft_display_status("CODE CLEARED");
        return;
    }

    if (code_idx < PASSCODE_MAX_LEN - 1) {
        entered_code[code_idx++] = key;
        entered_code[code_idx] = '\0';
        
        char display_buf[32];
        snprintf(display_buf, sizeof(display_buf), "CODE: %s", entered_code);
        tft_display_status(display_buf);
    }

    // Verify when '#' is pressed
    if (key == '#') {
        if (strcmp(entered_code, correct_passcode) == 0) {
            ESP_LOGI(TAG, "Correct Passcode! Opening Door.");
            tft_display_status("ACCESS GRANTED");
            open_door();
        } else {
            ESP_LOGW(TAG, "Wrong Passcode: %s", entered_code);
            tft_display_status("ACCESS DENIED");
            trigger_alarm();
            vTaskDelay(pdMS_TO_TICKS(2000));
            stop_alarm();
        }
        // Reset code
        memset(entered_code, 0, sizeof(entered_code));
        code_idx = 0;
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Starting AIoT Receptionist Project...");

    // 1. Initialize Network & NVS
    network_init();

    // 2. Initialize Hardware (Relay, Buzzer, Sonar, Keypad, TFT)
    hardware_init();

    // 3. Initialize Audio (I2S Micro, Speaker)
    audio_init();

    // 4. Initialize Vision (Camera, Models)
    vision_init();

    ESP_LOGI(TAG, "System Initialized. Entering main loop.");
    tft_display_status("SYSTEM READY");

    // State Machine Loop
    while (1) {
        // Read Keypad for manual entry
        char key = read_keypad();
        if (key != '\0') {
            handle_keypad(key);
        }

        // Step 1: Wait for person to be near
        if (is_person_near()) {
            ESP_LOGI(TAG, "Person detected near door.");
            tft_display_status("PERSON DETECTED");
            
            // Step 2: Process Vision Frame
            tft_display_status("SCANNING FACE");
            vision_state_t v_state = vision_process_frame();
            
            if (v_state == VISION_KNOWN_PERSON) {
                // Step 3a: Known Person -> Liveness Check
                ESP_LOGI(TAG, "Known person recognized. Requesting smile for liveness check.");
                tft_display_status("SMILE TO VERIFY");
                
                // Simulate playing offline audio file "Vui lòng mỉm cười"
                audio_play((const uint8_t*)"Smile", 5); 
                
                // Wait up to 5 seconds for smile
                bool liveness_passed = false;
                for (int i = 0; i < 50; i++) {
                    if (vision_check_liveness()) {
                        liveness_passed = true;
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(100)); // check every 100ms
                }

                if (liveness_passed) {
                    ESP_LOGI(TAG, "Liveness passed. Opening door.");
                    tft_display_status("ACCESS GRANTED");
                    open_door();
                    network_send_access_log("KnownUser", "Success");
                } else {
                    ESP_LOGW(TAG, "Liveness failed. Timeout.");
                    tft_display_status("LIVENESS FAILED");
                    // Simulate playing offline audio file "Timeout"
                    audio_play((const uint8_t*)"Timeout", 7); 
                }

            } else if (v_state == VISION_STRANGER) {
                // Step 3b: Stranger -> Audio Dialogue
                ESP_LOGI(TAG, "Stranger detected. Initiating AI Dialogue.");
                tft_display_status("GUEST DIALOGUE");
                
                // Play offline greeting "Xin chào, bạn cần gặp ai ạ?"
                audio_play((const uint8_t*)"Hello", 5); 
                
                // Record answer (with VAD silence detection)
                int rec_len = audio_record(audio_buffer, AUDIO_BUFFER_SIZE);
                
                if (rec_len > 0) {
                    tft_display_status("AI PROCESSING");
                    // Send to Hugging Face API (Unified STT -> LLM -> TTS)
                    char guest_text[256] = {0};
                    size_t tts_len = 0;
                    
                    if (network_hf_process_audio(audio_buffer, rec_len, guest_text, sizeof(guest_text), audio_buffer, &tts_len, AUDIO_BUFFER_SIZE)) {
                        ESP_LOGI(TAG, "Guest said: %s", guest_text);
                        
                        // Send text to Discord
                        char discord_msg[512];
                        snprintf(discord_msg, sizeof(discord_msg), "Khách lạ nói: %s", guest_text);
                        network_send_discord_alert(NULL, 0, discord_msg);
                        
                        if (tts_len > 0) {
                            audio_play(audio_buffer, tts_len);
                        }
                    }
                } else {
                    // Send default alert to Discord if no audio
                    network_send_discord_alert(NULL, 0, "Stranger at the door! Check Dashboard.");
                }
                
                tft_display_status("ALERT SENT");
                
            } else if (v_state == VISION_SPOOFING_DETECTED) {
                // Step 3c: Spoofing (Printed photo / Video on phone)
                ESP_LOGE(TAG, "Spoofing detected!");
                tft_display_status("SPOOFING DETECTED");
                trigger_alarm();
                network_send_discord_alert(NULL, 0, "SPOOFING ALERT! Alarm triggered.");
                
                vTaskDelay(pdMS_TO_TICKS(10000)); // Alarm for 10s
                stop_alarm();
            }
            
            // Debounce/Delay before next check to prevent spamming
            vTaskDelay(pdMS_TO_TICKS(3000));
            tft_display_status("SYSTEM READY");
        } else {
            // Idle state
            vTaskDelay(pdMS_TO_TICKS(100)); // Scan keypad faster (every 100ms)
        }
    }
}
