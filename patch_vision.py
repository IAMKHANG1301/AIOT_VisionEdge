import re

with open('components/vision_edge/vision.cpp', 'r', encoding='utf-8') as f:
    content = f.read()

# 1. Remove s_liveness_enabled and related variables
content = re.sub(r'static volatile bool s_liveness_enabled = true;\n', '', content)
content = re.sub(r'static int s_smile_consecutive_frames = 0;\n', '', content)
content = re.sub(r'static float s_initial_ratio = -1\.0f;\n', '', content)
content = re.sub(r'static float s_smooth_ratio = -1\.0f;\n', '', content)
content = re.sub(r'static float s_last_smile_ratio = 0\.0f;\n', '', content)

# 2. Add tft_driver include
if '#include "tft_driver.h"' not in content:
    content = content.replace('#include "vision.h"', '#include "vision.h"\n#include "tft_driver.h"')

# 3. Replace the entire Liveness block with 3s block
liveness_start = content.find('// --- 2. LIVENESS CHALLENGE')
liveness_end = content.find('// Cooldown nh? d? cho CPU h? nhi?t')

new_block = """// --- 2. XÁC NH?N DANH TÍNH (3 GIÂY) ---
            if (s_last_recognized_id > 0) {
                // N?U LÀ NGU?I QUEN:
                current_vision_state = VISION_KNOWN_PERSON;

                if (s_known_detected_start_time == 0) {
                    s_known_detected_start_time = now_us;
                    s_liveness_passed = false;
                    s_spoofing_detected = false;
                    tft_update_ui(TFT_COLOR_WHITE, "Ðang xác d?nh danh tính,", "vui lòng ch? trong giây lát...", NULL);
                    ESP_LOGI(TAG, "KNOWN '%s' - DETECTED. Waiting 3s to confirm.", recognized_name);
                }

                if (!s_liveness_passed && !s_spoofing_detected) {
                    int elapsed_sec = (int)((now_us - s_known_detected_start_time) / 1000000LL);
                    
                    if (elapsed_sec >= 3) {
                        s_liveness_passed = true;
                        trigger_snapshot_and_upload("known", s_recognized_person_id, "KNOWN");
                        
                        char welcome_msg[64];
                        snprintf(welcome_msg, sizeof(welcome_msg), "Chào m?ng %s dã v? nhà", recognized_name);
                        tft_update_ui(TFT_COLOR_GREEN, "Xác nh?n danh tính thành công:", "Ngu?i quen", welcome_msg);
                        
                        open_door();
                        ESP_LOGI(TAG, "?? DOOR UNLOCKED for %s!", recognized_name);
                    }
                }
            } else {
                // N?U LÀ NGU?I L? (STRANGER): B? qua Liveness check
                current_vision_state = VISION_STRANGER;
                
                if (s_known_detected_start_time == 0) {
                    s_known_detected_start_time = now_us;
                    s_liveness_passed = false;
                    s_spoofing_detected = false;
                    tft_update_ui(TFT_COLOR_WHITE, "Ðang xác d?nh danh tính,", "vui lòng ch? trong giây lát...", NULL);
                    ESP_LOGI(TAG, "STRANGER DETECTED. Waiting 3s to confirm.");
                }

                if (!s_spoofing_detected) { // Dùng spoofing_detected làm c? dánh d?u dã xác nh?n ngu?i l?
                    int elapsed_sec = (int)((now_us - s_known_detected_start_time) / 1000000LL);
                    
                    if (elapsed_sec >= 3) {
                        s_spoofing_detected = true;
                        tft_update_ui(TFT_COLOR_RED, "Xác d?nh danh tính th?t b?i:", "Ngu?i l?", "B?n có th? nh?n gi? nút và nói chuy?n v?i l? tân ?o d? du?c h? tr?");
                        // Only log/upload if we haven't spammed it recently
                        static int64_t last_stranger_upload = 0;
                        if ((esp_timer_get_time() - last_stranger_upload) > 10000000LL) {
                            trigger_snapshot_and_upload("unknown", NULL, "STRANGER");
                            last_stranger_upload = esp_timer_get_time();
                        }
                    }
                }
            }
        }


        """
content = content[:liveness_start] + new_block + content[liveness_end:]

# 4. Remove liveness handlers
handler_start = content.find('static esp_err_t liveness_on_handler(httpd_req_t *req) {')
handler_end = content.find('// REST API: L?y danh sách b?ng NguoiQuen')
content = content[:handler_start] + content[handler_end:]

# 5. Remove URI registrations
content = re.sub(r'\s*\{\s*\.uri\s*=\s*"/liveness_on".*?\},', '', content, flags=re.DOTALL)
content = re.sub(r'\s*\{\s*\.uri\s*=\s*"/liveness_off".*?\},', '', content, flags=re.DOTALL)

with open('components/vision_edge/vision.cpp', 'w', encoding='utf-8') as f:
    f.write(content)

print("Patch applied.")
