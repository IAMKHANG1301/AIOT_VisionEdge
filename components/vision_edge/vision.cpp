#include "vision.h"
#include "esp_log.h"
#include "esp_camera.h"
#include <esp_http_server.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

#include "img_converters.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "web_ui.h"
#include "db_manager.h"
#include "mqtt_service.h"
#include "network.h"

extern "C" {
#include "supabase_client.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#include "human_face_detect_msr01.hpp"
#include "human_face_detect_mnp01.hpp"
#include "face_recognition_112_v1_s8.hpp"
#pragma GCC diagnostic pop

static const char *TAG = "VISION";

httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;

static HumanFaceDetectMSR01 *s_detector = NULL;
static HumanFaceDetectMNP01 *s_detector2 = NULL;
static FaceRecognition112V1S8 *s_recognizer = NULL;
static SemaphoreHandle_t s_recognizer_mutex = NULL;

#define SD_CARD_FACES_DIR "/sdcard/faces"
#define AI_TASK_STACK_SIZE 16384  // 16KB: đủ cho AI model + face recognition

// =============================================================================
// TRẠNG THÁI TOÀN CỤC (Shared State - thread-safe via volatile)
// =============================================================================
static volatile vision_state_t current_vision_state = VISION_IDLE;
static volatile bool s_ai_enabled = false;
static volatile bool s_liveness_enabled = true;

// Enroll: Giao tiếp an toàn giữa HTTP handler (Core 0) và AI task (Core 1)
// Dùng 3 giá trị: 0=chờ, 1=thành công, -1=lỗi không thấy mặt, -2=lỗi SD
static volatile bool s_enroll_requested = false;
static volatile int  s_enroll_result    = 0;

#include "db_manager.h"
#include <vector>
#include <string>

// Tên và ID người quen nhận diện được (chỉ AI task ghi, HTTP handler đọc)
static char recognized_name[64] = {0};
static char s_recognized_person_id[16] = {0};
static char s_enroll_name[64] = {0};
static std::vector<std::string> s_enrolled_person_ids;

// Biến lưu trữ ảnh JPEG lúc enroll để gửi lên Web
static uint8_t *s_snapshot_buf = NULL;
static size_t s_snapshot_len = 0;

// RGB888 buffer cấp phát trong PSRAM để chứa ảnh giải nén
static uint8_t *ai_rgb888_buf = NULL;

// YUV422 raw copy buffer trong PSRAM để trả camera DMA ngay lập tức
// 320x240x2 = 153600 bytes, được copy từ fb trước khi xử lý
static uint8_t *s_yuv_copy_buf = NULL;
static uint16_t s_yuv_copy_w = 0;
static uint16_t s_yuv_copy_h = 0;

#include "hardware.h"

// 10-Second Dynamic Expression Liveness Challenge State
static volatile bool s_liveness_passed = false;
static volatile bool s_spoofing_detected = false;
static volatile float s_last_smile_ratio = 0.0f; // Giữ lại để Web UI hiển thị
static volatile int s_smile_consecutive_frames = 0;
static volatile float s_initial_ratio = -1.0f;
static volatile float s_smooth_ratio = -1.0f;
static volatile float s_slow_baseline_ratio = -1.0f;
static volatile int64_t s_known_detected_start_time = 0;

static void bg_upload_task(void* pv) {
    const char *filepath = "/sdcard/snapshot.jpg";
    esp_err_t upload_result = upload_image_to_hf(filepath);
    if (upload_result == ESP_OK) {
        ESP_LOGI(TAG, "Image upload OK — deleting %s", filepath);
    } else {
        ESP_LOGW(TAG, "Image upload FAILED (err=%d) — deleting %s", upload_result, filepath);
    }
    remove(filepath);
    vTaskDelete(NULL);
}

static bool trigger_snapshot_and_upload(const char* log_label, const char* person_id, const char* hf_label) {
    if (!s_yuv_copy_buf || s_yuv_copy_w == 0 || s_yuv_copy_h == 0) return false;
    size_t yuv_len = (size_t)s_yuv_copy_w * s_yuv_copy_h * 2;
    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    // Nén ảnh ra JPEG (từ YUV422 copy buf)
    bool ok = fmt2jpg(s_yuv_copy_buf, yuv_len, s_yuv_copy_w, s_yuv_copy_h, PIXFORMAT_YUV422, 30, &jpg_buf, &jpg_len);
    if (ok && jpg_buf && jpg_len > 0) {
        // 1. Thêm Log vào DB (và gọi API Supabase)
        db_log_add(log_label, person_id, jpg_buf, jpg_len);

        // 2. Lưu file ra thẻ nhớ để HuggingFace WebSocket task đọc
        const char *filepath = "/sdcard/snapshot.jpg";
        FILE *f = fopen(filepath, "wb");
        if (f) {
            fwrite(jpg_buf, 1, jpg_len, f);
            fclose(f);
            ESP_LOGI(TAG, "Saved %s snapshot to %s (%zu bytes, from PSRAM copy)", hf_label, filepath, jpg_len);
            
            // Khởi tạo luồng ngầm gửi ảnh lên HuggingFace
            xTaskCreate(bg_upload_task, "bg_upload", 10240, NULL, 4, NULL);
        } else {
            ESP_LOGE(TAG, "Failed to open %s for writing!", filepath);
        }
        
        free(jpg_buf);
        return true;
    } else {
        ESP_LOGE(TAG, "Snapshot compression FAILED for %s", log_label);
    }
    return false;
}

static volatile int64_t s_last_face_seen_time = 0;
static volatile int s_last_recognized_id = -1;

// =============================================================================
// =============================================================================
// LIVENESS: Behavioral 2-Stage Challenge
// Giai đoạn 1 (0-5s): Bắt buộc mặt lạnh - chống video đang cười / video chạy
// Giai đoạn 2 (5-10s): Yêu cầu nụ cười thật sự - chống ảnh tĩnh / mặt nạ giấy
// =============================================================================
#define SMILE_THRESHOLD        0.02f  // Ngưỡng dưới nụ cười thật: delta >= 0.02 (khoảng cách mép miệng giãn ra)
#define SMILE_MAX_THRESHOLD    0.45f  // Ngưỡng trên nụ cười thật
#define NEUTRAL_MAX_THRESHOLD  0.02f // Ngưỡng mặt lạnh: delta > 0.02 trong 5s đầu = GIẢ MẠO


// =============================================================================
// SENSOR KHOẢNG CÁCH (HC-SR04 - chưa kết nối, trả về false)
// =============================================================================
bool sensor_check_distance_detected(void) {
    return false;
}

// =============================================================================
// TẢI KHUÔN MẶT TỪ THẺ NHỚ KHI KHỞI ĐỘNG
// =============================================================================
static void load_faces_from_sd_locked(void) {
    s_enrolled_person_ids.clear();
    cJSON *db = db_person_get_all_json();
    int loaded = 0;
    int size = cJSON_GetArraySize(db);
    for (int i = 0; i < size; i++) {
        cJSON *item = cJSON_GetArrayItem(db, i);
        cJSON *id_obj = cJSON_GetObjectItem(item, "id");
        cJSON *name_obj = cJSON_GetObjectItem(item, "name");
        cJSON *vec_obj = cJSON_GetObjectItem(item, "face_vector");

        if (vec_obj && vec_obj->valuestring) {
            FILE *f = fopen(vec_obj->valuestring, "rb");
            if (f) {
                float emb[FACE_VECTOR_SIZE];
                if (fread(emb, sizeof(float), FACE_VECTOR_SIZE, f) == FACE_VECTOR_SIZE) {
                    dl::Tensor<float> tensor_emb;
                    tensor_emb.set_element(emb).set_shape({FACE_VECTOR_SIZE}).set_auto_free(false);
                    
                    const char *person_name = (name_obj && name_obj->valuestring) ? name_obj->valuestring : "Unknown";
                    const char *person_id = (id_obj && id_obj->valuestring) ? id_obj->valuestring : "P0";

                    s_recognizer->enroll_id(tensor_emb, std::string(person_name), false);
                    s_enrolled_person_ids.push_back(std::string(person_id));
                    loaded++;
                    ESP_LOGI(TAG, "Loaded face [%s] - %s from SD Card", person_id, person_name);
                }
                fclose(f);
            }
        }
    }
    cJSON_Delete(db);
    ESP_LOGI(TAG, "Total %d face(s) loaded from SD Card DB.", loaded);
}

static void load_faces_from_sd(void) {
    if (s_recognizer_mutex) xSemaphoreTake(s_recognizer_mutex, portMAX_DELAY);
    load_faces_from_sd_locked();
    if (s_recognizer_mutex) xSemaphoreGive(s_recognizer_mutex);
}

// =============================================================================
// THUẬT TOÁN CLAHE (Contrast Limited Adaptive Histogram Equalization)
// Phân bổ lại độ sáng thích ứng hạn chế độ tương phản trên ảnh BGR888
// Tối ưu cho ESP32-S3 (Khung hình 320x240, 8x8 lưới Tiles)
// =============================================================================
static void __attribute__((unused)) apply_clahe_bgr888(uint8_t *img, int width, int height) {
    const int GRID_X = 8;
    const int GRID_Y = 8;
    const int tile_w = width / GRID_X;   // 40px
    const int tile_h = height / GRID_Y;  // 30px
    const int tile_pixels = tile_w * tile_h; // 1200 pixels
    const float clip_limit_factor = 1.5f;
    const int clip_limit = (int)(clip_limit_factor * (tile_pixels / 256.0f)); // ~70

    static uint8_t lut[GRID_Y][GRID_X][256];

    // 1. Tính toán Histogram & CDF cho từng Tile 8x8
    for (int gy = 0; gy < GRID_Y; gy++) {
        for (int gx = 0; gx < GRID_X; gx++) {
            int hist[256] = {0};
            int start_x = gx * tile_w;
            int start_y = gy * tile_h;

            for (int y = 0; y < tile_h; y++) {
                int px_y = start_y + y;
                uint8_t *ptr = img + (px_y * width + start_x) * 3;
                for (int x = 0; x < tile_w; x++) {
                    uint8_t b = ptr[0];
                    uint8_t g = ptr[1];
                    uint8_t r = ptr[2];
                    uint8_t lum = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
                    hist[lum]++;
                    ptr += 3;
                }
            }

            // Clip histogram để tránh khuếch đại nhiễu
            int excess = 0;
            for (int i = 0; i < 256; i++) {
                if (hist[i] > clip_limit) {
                    excess += (hist[i] - clip_limit);
                    hist[i] = clip_limit;
                }
            }

            // Phân bổ phần thừa đều cho 256 bin
            int bonus = excess / 256;
            for (int i = 0; i < 256; i++) {
                hist[i] += bonus;
            }

            // Tính CDF (Cumulative Distribution Function)
            int sum = 0;
            float scale = 255.0f / tile_pixels;
            for (int i = 0; i < 256; i++) {
                sum += hist[i];
                int val = (int)(sum * scale);
                lut[gy][gx][i] = (val > 255) ? 255 : ((val < 0) ? 0 : val);
            }
        }
    }

    // 2. Nội suy Bilinear số nguyên (Fixed Point 8-bit - Siêu nhanh ~2ms)
    for (int y = 0; y < height; y++) {
        int gy_fixed = (y * 256 / tile_h) - 128;
        if (gy_fixed < 0) gy_fixed = 0;
        int gy1 = gy_fixed >> 8;
        int gy2 = gy1 + 1;
        if (gy1 >= GRID_Y - 1) { gy1 = GRID_Y - 1; gy2 = GRID_Y - 1; }
        int wy = gy_fixed & 255;
        int inv_wy = 256 - wy;

        for (int x = 0; x < width; x++) {
            int gx_fixed = (x * 256 / tile_w) - 128;
            if (gx_fixed < 0) gx_fixed = 0;
            int gx1 = gx_fixed >> 8;
            int gx2 = gx1 + 1;
            if (gx1 >= GRID_X - 1) { gx1 = GRID_X - 1; gx2 = GRID_X - 1; }
            int wx = gx_fixed & 255;
            int inv_wx = 256 - wx;

            uint8_t *ptr = img + (y * width + x) * 3;
            uint8_t b = ptr[0];
            uint8_t g = ptr[1];
            uint8_t r = ptr[2];
            uint8_t lum = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);

            int map11 = lut[gy1][gx1][lum];
            int map12 = lut[gy1][gx2][lum];
            int map21 = lut[gy2][gx1][lum];
            int map22 = lut[gy2][gx2][lum];

            int lum_new = (inv_wy * (inv_wx * map11 + wx * map12) + wy * (inv_wx * map21 + wx * map22)) >> 16;

            if (lum > 0) {
                int new_b = (b * lum_new) / lum;
                int new_g = (g * lum_new) / lum;
                int new_r = (r * lum_new) / lum;

                ptr[0] = (new_b > 255) ? 255 : new_b;
                ptr[1] = (new_g > 255) ? 255 : new_g;
                ptr[2] = (new_r > 255) ? 255 : new_r;
            }
        }
    }
}

// =============================================================================
// TÁC VỤ AI NHẬN DIỆN (Chạy hoàn toàn trên Core 1)
// =============================================================================
static int64_t last_detection_time = 0;
#define DETECTION_COOLDOWN_US  2000000LL  // 2 giây: tránh spam nhận diện

static void ai_task(void *arg) {
    ESP_LOGI(TAG, "AI Task started on Core %d", xPortGetCoreID());
    int loop_count = 0;

    while (1) {
        // Chờ khi AI tắt và không có yêu cầu enroll
        if (!s_ai_enabled && !s_enroll_requested && !sensor_check_distance_detected()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Nút nhấn enroll: Xóa hết các frame cũ đang nằm trong hàng đợi camera để lấy frame tươi mới nhất
        if (s_enroll_requested) {
            ESP_LOGI(TAG, "Flushing camera buffers for fresh snapshot...");
            for (int i = 0; i < 2; i++) {
                camera_fb_t *stale_fb = esp_camera_fb_get();
                if (stale_fb) {
                    esp_camera_fb_return(stale_fb);
                }
            }
        }

        // --- Chụp ảnh từ Camera DMA ---
        camera_fb_t *fb = esp_camera_fb_get();

        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed!");
            if (s_enroll_requested) {
                s_enroll_result   = -1;
                s_enroll_requested = false;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }


        // --- Kiểm tra tính hợp lệ của frame YUV422 ---
        // OV2640 xuất YUV422 native tại QVGA: 320*240*2 = 153600 bytes
        bool frame_ok = (fb->format == PIXFORMAT_YUV422)
                     && (fb->width  == 320)
                     && (fb->height == 240)
                     && (fb->len    >= (320 * 240 * 2));

        if (!frame_ok) {
            ESP_LOGW(TAG, "Bad frame: fmt=%d w=%d h=%d len=%zu.",
                     fb->format, fb->width, fb->height, fb->len);
            esp_camera_fb_return(fb);
            if (s_enroll_requested) { s_enroll_result = -1; s_enroll_requested = false; }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // --- Copy raw YUV422 sang PSRAM copy buffer ---
        // Giới hạn copy_len tối đa 153600 bytes để tránh tràn bộ nhớ nếu fb->len lớn hơn do padding
        size_t max_copy_len = 320 * 240 * 2;
        size_t copy_len = (fb->len > max_copy_len) ? max_copy_len : fb->len;
        memcpy(s_yuv_copy_buf, fb->buf, copy_len);
        s_yuv_copy_w = fb->width;
        s_yuv_copy_h = fb->height;

        // --- Convert YUV422 → RGB888 (màu chính xác 100% cho AI) ---
        bool converted = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_YUV422, ai_rgb888_buf);

        // NẾU CÓ LỆNH ENROLL, NÉN ÁNH JPEG TỪ PSRAM ----
        if (s_enroll_requested) {
            if (s_snapshot_buf != NULL) {
                free(s_snapshot_buf);
                s_snapshot_buf = NULL;
            }
            bool ok = fmt2jpg(s_yuv_copy_buf, copy_len, fb->width, fb->height, PIXFORMAT_YUV422, 30, &s_snapshot_buf, &s_snapshot_len);
            if (!ok) {
                ESP_LOGE(TAG, "Snapshot compression (fmt2jpg) FAILED!");
            } else {
                ESP_LOGI(TAG, "Snapshot compressed successfully for Enroll: %zu bytes", s_snapshot_len);
            }
        }

        // === TRẢ FRAME BUFFER CHO CAMERA DMA NGAY LẬP TỨC! ===
        // Trả fb ngay tại đây (<1ms) để camera driver giải phóng DMA buffer,
        // triệt tiêu hoàn toàn lỗi cam_hal timeout và EV-VSYNC-OVF!
        esp_camera_fb_return(fb);
        fb = NULL;

        if (!converted) {
            ESP_LOGE(TAG, "YUV422→RGB888 conversion FAILED!");
            if (s_enroll_requested) { s_enroll_result = -1; s_enroll_requested = false; }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // --- QUAN TRỌNG: Đổi RGB → BGR vì esp-dl yêu cầu BGR888 ---
        // Theo tài liệu esp-dl: recognize() và enroll_id() đều nhận "bgr888"
        // fmt2rgb888() xuất RGB, phải swap R↔B tại chỗ
        {
            uint8_t *p = ai_rgb888_buf;
            uint8_t *end = p + (320 * 240 * 3);
            while (p < end) {
                uint8_t tmp = p[0]; // R
                p[0] = p[2];        // R ← B
                p[2] = tmp;         // B ← R
                p += 3;
            }
        }

        // --- Tạm thời TẮT CLAHE vì nó làm biến dạng màu sắc (HDR ảo) khiến MobileFaceNet (nhận diện ID) bị sai lệch ---
        // apply_clahe_bgr888(ai_rgb888_buf, 320, 240);

        // --- Chẩn đoán: In pixel trung tâm mỗi 30 frame để xác nhận ảnh hợp lệ ---
        if (loop_count % 30 == 0) {
            // Pixel tại tọa độ trung tâm (160, 120) — sau khi đã swap thành BGR
            int cx = (120 * 320 + 160) * 3;
            ESP_LOGI(TAG, "[DIAG] Center pixel B=%d G=%d R=%d (skin~B120,G150,R200)",
                     ai_rgb888_buf[cx], ai_rgb888_buf[cx+1], ai_rgb888_buf[cx+2]);
        }

        // --- Chạy mô hình phát hiện khuôn mặt Stage 1 (MSR01) ---
        int64_t t1 = esp_timer_get_time();
        std::list<dl::detect::result_t> &candidates = s_detector->infer(ai_rgb888_buf, {240, 320, 3});
        
        // --- Chạy mô hình phát hiện khuôn mặt Stage 2 (MNP01) để lấy 5 keypoints căn chỉnh khuôn mặt ---
        std::list<dl::detect::result_t> results;
        if (!candidates.empty() && s_detector2 != NULL) {
            results = s_detector2->infer(ai_rgb888_buf, {240, 320, 3}, candidates);
        }
        int64_t infer_ms = (esp_timer_get_time() - t1) / 1000;

        // Log số lượng khuôn mặt khi không có mặt nào (0)
        if (results.empty()) {
            ESP_LOGI(TAG, "[VISION] Infer: %lld ms | Faces: 0", infer_ms);
            // Dung thứ thời gian mất mặt ngắn: Nâng lên 2.5s để tránh reset TMV buffer khi face bị rớt nhất thời
            if (s_known_detected_start_time > 0 && (esp_timer_get_time() - s_last_face_seen_time) > 2500000LL) {
                s_known_detected_start_time = 0;
                s_initial_ratio = -1.0f;
                s_smooth_ratio = -1.0f;
                s_last_recognized_id = -1;
                // Reset Challenge state khi mất mặt
                s_smile_consecutive_frames = 0;
            }
            if (s_enroll_requested) {
                s_enroll_result   = -1;
                s_enroll_requested = false;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Có khuôn mặt trong frame -> cập nhật mốc thời gian nhìn thấy mặt gần nhất
        int64_t now_us = esp_timer_get_time();
        s_last_face_seen_time = now_us;

        // Có khuôn mặt: lấy mặt tốt nhất (tin cậy cao nhất)
        dl::detect::result_t best_face = results.front();
        dl::Tensor<uint8_t> image_tensor;
        image_tensor.set_element(ai_rgb888_buf).set_shape({240, 320, 3}).set_auto_free(false);

        // --- Enroll (Đăng ký khuôn mặt mới) ---
        if (s_enroll_requested) {
            if (s_recognizer_mutex) xSemaphoreTake(s_recognizer_mutex, portMAX_DELAY);
            int before_count = s_recognizer->get_enrolled_id_num();
            const char *person_name = (strlen(s_enroll_name) > 0) ? s_enroll_name : "Thành viên";

            s_recognizer->enroll_id(image_tensor, best_face.keypoint, std::string(person_name), false);
            int after_count = s_recognizer->get_enrolled_id_num();

            if (after_count > before_count) {
                dl::Tensor<float> &emb = s_recognizer->get_face_emb(after_count);
                char created_id[16];
                
                // Gọi db_person_add tự động sinh ID "P" + số ngẫu nhiên 5 chữ số (Ví dụ: P48291), lưu ảnh .jpg và vector .bin vào SD Card
                esp_err_t err = db_person_add(person_name, emb.get_element_ptr(), s_snapshot_buf, s_snapshot_len, created_id);
                if (err == ESP_OK) {
                    s_enrolled_person_ids.push_back(std::string(created_id));
                    ESP_LOGI(TAG, "ENROLL SUCCESS: Registered Person [%s] - %s", created_id, person_name);
                    s_enroll_result = 1;
                    
                    // MQTT: Gửi thông báo thành công về cho Vercel lưu database
                    char mqtt_payload[256];
                    snprintf(mqtt_payload, sizeof(mqtt_payload), 
                        "{\"status\":\"enroll_success\",\"name\":\"%s\",\"id\":\"%s\"}", 
                        person_name, created_id);
                    mqtt_publish_status(mqtt_payload);
                } else {
                    ESP_LOGE(TAG, "ENROLL: Failed to write Database to SD Card");
                    s_enroll_result = -2;
                }
            } else {
                ESP_LOGW(TAG, "ENROLL: enroll_id returned no new face entry.");
                s_enroll_result = -1;
            }
            if (s_recognizer_mutex) xSemaphoreGive(s_recognizer_mutex);
            s_enroll_requested = false;

        } else {
            // --- 1. Nhận diện ID khuôn mặt ---
            bool challenge_active = (s_known_detected_start_time > 0 && !s_spoofing_detected && !s_liveness_passed);
            if (!challenge_active && (now_us - last_detection_time) >= DETECTION_COOLDOWN_US) {
                if (s_recognizer_mutex) xSemaphoreTake(s_recognizer_mutex, portMAX_DELAY);
                face_info_t result = s_recognizer->recognize(image_tensor, best_face.keypoint);
                s_last_recognized_id = result.id;
                if (result.id > 0) {
                    if (result.id <= (int)s_enrolled_person_ids.size()) {
                        strncpy(s_recognized_person_id, s_enrolled_person_ids[result.id - 1].c_str(), sizeof(s_recognized_person_id) - 1);
                    } else {
                        snprintf(s_recognized_person_id, sizeof(s_recognized_person_id), "P%d", result.id);
                    }
                    strncpy(recognized_name, result.name.c_str(), sizeof(recognized_name) - 1);
                    recognized_name[sizeof(recognized_name) - 1] = '\0';
                } else {
                    strcpy(s_recognized_person_id, "NULL");
                    strncpy(recognized_name, "Unknown", sizeof(recognized_name) - 1);
                }
                if (s_recognizer_mutex) xSemaphoreGive(s_recognizer_mutex);
                last_detection_time = now_us;
            }


            // --- 2. LIVENESS CHALLENGE & TIMELINE (CHẠY TRÊN MỌI FRAME, KHÔNG BỊ KHỰNG THEO COOLDOWN) ---
            if (s_last_recognized_id > 0) {
                // NẾU LÀ NGƯỜI QUEN:
                current_vision_state = VISION_KNOWN_PERSON;

                if (best_face.keypoint.size() >= 10) {
                    float left_eye_x = (float)best_face.keypoint[0];
                    float left_eye_y = (float)best_face.keypoint[1];
                    float right_eye_x = (float)best_face.keypoint[2];
                    float right_eye_y = (float)best_face.keypoint[3];
                    float left_mouth_x = (float)best_face.keypoint[6];
                    float left_mouth_y = (float)best_face.keypoint[7];
                    float right_mouth_x = (float)best_face.keypoint[8];
                    float right_mouth_y = (float)best_face.keypoint[9];
                    
                    // =========================================================
                    // ĐO TỈ LỆ NỤ CƯỜI QUA LANDMARK (Mouth Width / Eye Distance)
                    // Khi cười: 2 mép miệng doãng ra → mouth_width tăng → ratio tăng
                    // Dùng Euclidean 2D để chống lỗi khi nghiêng đầu
                    // =========================================================
                    float eye_dist   = sqrtf(powf(right_eye_x - left_eye_x, 2.0f)
                                           + powf(right_eye_y - left_eye_y, 2.0f));
                    float mouth_dist = sqrtf(powf(right_mouth_x - left_mouth_x, 2.0f)
                                           + powf(right_mouth_y - left_mouth_y, 2.0f));
                    float current_ratio = (eye_dist > 1.0f) ? (mouth_dist / eye_dist) : 0.0f;

                    // Nếu là frame đầu tiên của challenge, reset luôn bộ lọc EMA về giá trị thực tế của frame này
                    if (s_known_detected_start_time == 0) {
                        s_smooth_ratio = current_ratio;
                    } else {
                        // EMA làm mượt (alpha=0.3 → phản ứng nhanh nhưng ít nhiễu)
                        s_smooth_ratio = s_smooth_ratio * 0.7f + current_ratio * 0.3f;
                    }

                    // Tính toán delta từ s_smooth_ratio thay vì current_ratio thô
                    float delta = 0.0f;
                    if (s_initial_ratio > 0) {
                        delta = s_smooth_ratio - s_initial_ratio;
                        if (delta < 0.0f) delta = 0.0f;   // chỉ tính tăng (cười rộng ra)
                    }
                    s_last_smile_ratio = delta;

                    // Log GỘP cả Infer, số lượng mặt (1) và SMILE
                    ESP_LOGI(TAG, "[VISION] Infer: %lld ms | Faces: 1 | [SMILE] eye=%.1f mouth=%.1f ratio=%.3f smooth=%.3f delta=%.3f",
                             infer_ms, eye_dist, mouth_dist, current_ratio, s_smooth_ratio, delta);

                    // Khởi tạo baseline tại frame đầu tiên nhận ra người quen
                    if (s_known_detected_start_time == 0) {
                        s_known_detected_start_time = now_us;
                        s_initial_ratio = s_smooth_ratio; // baseline = ratio lúc mặt lạnh
                        s_liveness_passed = false;
                        s_spoofing_detected = false;
                        s_smile_consecutive_frames = 0;
                        if (s_liveness_enabled) {
                            ESP_LOGI(TAG, "KNOWN '%s' - Challenge START. Giữ mặt lạnh 5s! baseline=%.3f",
                                     recognized_name, s_initial_ratio);
                        } else {
                            ESP_LOGI(TAG, "KNOWN '%s' - Liveness DISABLED. Waiting 2s to unlock...", recognized_name);
                        }
                    }

                    if (!s_liveness_passed && !s_spoofing_detected) {
                        int elapsed_sec = (int)((now_us - s_known_detected_start_time) / 1000000LL);
                        
                        if (s_liveness_enabled) {
                            // Log delta mỗi frame để theo dõi
                            ESP_LOGI(TAG, "[Challenge] t=%ds | delta=%.3f", elapsed_sec, delta);
                        }

                        if (!s_liveness_enabled) {
                            if (elapsed_sec >= 2) {
                                s_liveness_passed = true;
                                s_spoofing_detected = false;
                                trigger_snapshot_and_upload("known", s_recognized_person_id, "KNOWN");
                                open_door();
                                ESP_LOGI(TAG, "🔓 Liveness Disabled. DOOR UNLOCKED directly for %s!", recognized_name);
                            }
                        } else {
                            if (elapsed_sec < 5) {
                                // =================================================
                                // GIAI ĐOẠN 1 (0-5s): BUỘC MẶT LẠNH
                                // Không được cười. Bắt sống video đang cười / gif / TikTok.
                            // =================================================
                            if (delta > NEUTRAL_MAX_THRESHOLD) {
                                // Phát hiện môi đang chạy -> Còi ngay
                                s_spoofing_detected = true;
                                s_liveness_passed = false;
                                current_vision_state = VISION_SPOOFING_DETECTED;
                                trigger_alarm();
                                ESP_LOGE(TAG, "🚨 FAKE DETECTED (Phase 1)! delta=%.3f > %.3f trong 5s đầu. ALARM!",
                                         delta, NEUTRAL_MAX_THRESHOLD);
                                
                                static int64_t last_spoofing_upload = 0;
                                if ((esp_timer_get_time() - last_spoofing_upload) > 10000000LL) {
                                    trigger_snapshot_and_upload("spoofing", NULL, "SPOOFING");
                                    last_spoofing_upload = esp_timer_get_time();
                                }
                            }
                        } else {
                            // =================================================
                            // GIAI ĐOẠN 2 (5-10s): YÊU CẦU NỤ CƯỜI THẬT SỰ
                            // delta phải nằm trong khoảng [SMILE_THRESHOLD, SMILE_MAX]
                            // delta < SMILE_THRESHOLD: mặt lạnh -> ảnh giấy / video mặt ngầu
                            // delta > SMILE_MAX:      nhiễu AI (lắc ảnh, bóng) -> bỏ qua
                            // =================================================
                            bool valid_smile = (delta >= SMILE_THRESHOLD) && (delta <= SMILE_MAX_THRESHOLD);
                            
                            if (valid_smile) {
                                s_smile_consecutive_frames = s_smile_consecutive_frames + 1;
                                if (s_smile_consecutive_frames >= 3) {
                                    s_liveness_passed = true;
                                    s_spoofing_detected = false;
                                    stop_alarm();
                                    trigger_snapshot_and_upload("known", s_recognized_person_id, "KNOWN");
                                    open_door();
                                    ESP_LOGI(TAG, "🎉 SMILE LIVENESS PASSED! delta=%.3f in [%.2f, %.2f]. DOOR UNLOCKED!",
                                             delta, SMILE_THRESHOLD, SMILE_MAX_THRESHOLD);
                                }
                            } else if (elapsed_sec >= 10) {
                                // Hết 10 giây không cười -> ảnh tĩnh / giấy / mặt nạ
                                s_spoofing_detected = true;
                                s_liveness_passed = false;
                                current_vision_state = VISION_SPOOFING_DETECTED;
                                trigger_alarm();
                                ESP_LOGE(TAG, "🚨 LIVENESS FAIL! Quá 10s không cười. ALARM!");
                                
                                static int64_t last_spoofing_upload2 = 0;
                                if ((esp_timer_get_time() - last_spoofing_upload2) > 10000000LL) {
                                    trigger_snapshot_and_upload("spoofing", NULL, "SPOOFING");
                                    last_spoofing_upload2 = esp_timer_get_time();
                                }
                            } else {
                                s_smile_consecutive_frames = 0;
                            }
                        }
                        }
                    } else if (s_spoofing_detected) {
                        int elapsed_sec = (int)((now_us - s_known_detected_start_time) / 1000000LL);
                        if (elapsed_sec >= 15) {
                            s_spoofing_detected = false;
                            s_known_detected_start_time = 0;
                            current_vision_state = VISION_IDLE;
                            ESP_LOGI(TAG, "Spoofing alarm finished. Resetting.");
                        }
                    }
                }
            } else {
                // NẾU LÀ NGƯỜI LẠ (STRANGER): Bỏ qua Liveness check
                current_vision_state = VISION_STRANGER;
                s_liveness_passed = false;
                s_spoofing_detected = false;
                s_known_detected_start_time = 0;
                s_initial_ratio = -1.0f;
                // Only log/upload if we haven't spammed it recently (debounce using last_detection_time)
                static int64_t last_stranger_upload = 0;
                if ((esp_timer_get_time() - last_stranger_upload) > 10000000LL) {
                    trigger_snapshot_and_upload("unknown", NULL, "STRANGER");
                    last_stranger_upload = esp_timer_get_time();
                }
            }
        }


        // Cooldown nhỏ để cho CPU hạ nhiệt
        vTaskDelay(pdMS_TO_TICKS(80));
        loop_count++;
    }
}

// =============================================================================
// HTTP HANDLERS (Chạy trên Core 0)
// =============================================================================

#include "web_ui.h"

static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief Enroll handler - KHÔNG block server (non-blocking polling design)
 * Vấn đề cũ: handler block 4 giây làm toàn bộ HTTP server chết.
 * Giải pháp: Gửi yêu cầu → poll 50ms × 60 lần (3 giây) → trả kết quả.
 * Thời gian block chấp nhận được vì HTTP server chạy đa luồng (task).
 */
static void url_decode(char *dst, const char *src) {
    char a = 0, b = 0;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit((unsigned char)a) && isxdigit((unsigned char)b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static esp_err_t enroll_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    char query[256] = {0};
    char name_encoded[128] = {0};
    char name[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", name_encoded, sizeof(name_encoded));
        url_decode(name, name_encoded);
    }

    if (strlen(name) == 0) {
        httpd_resp_set_type(req, "text/plain; charset=UTF-8");
        httpd_resp_send(req, "❌ Tên người quen không được để trống!", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    strncpy(s_enroll_name, name, sizeof(s_enroll_name) - 1);
    s_enroll_name[sizeof(s_enroll_name) - 1] = '\0';

    s_enroll_result    = 0;
    s_enroll_requested = true;

    // Poll kết quả từ AI task (chạy Core 1) – tối đa 10 giây
    int timeout = 200; // 200 × 50ms = 10000ms
    while (s_enroll_requested && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
        timeout--;
    }

    char resp[256];
    if (s_enroll_result == 1) {
        snprintf(resp, sizeof(resp), "{\"status\":\"success\",\"message\":\"Đăng ký thành công người quen: '%s'!\"}", s_enroll_name);
    } else if (s_enroll_result == -1) {
        snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"Không phát hiện khuôn mặt qua hình ảnh.\"}");
    } else if (s_enroll_result == -2) {
        snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"Lỗi ghi SD Card. Kiểm tra thẻ nhớ.\"}");
    } else {
        s_enroll_requested = false;
        snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"Timeout. Camera chưa sẵn sàng hoặc không thấy mặt.\"}");
    }

    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t snapshot_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    ESP_LOGI(TAG, "Request /snapshot.jpg - s_snapshot_buf=%p, len=%zu", s_snapshot_buf, s_snapshot_len);
    if (s_snapshot_buf != NULL && s_snapshot_len > 0) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_send(req, (const char *)s_snapshot_buf, s_snapshot_len);
    } else {
        httpd_resp_send_404(req);
    }
    return ESP_OK;
}

static esp_err_t live_frame_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    if (ai_rgb888_buf == NULL) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    // Cấp phát buffer tạm để swap BGR -> RGB (ai_rgb888_buf đang chứa BGR do esp-dl yêu cầu)
    size_t buf_len = 320 * 240 * 3;
    uint8_t *tmp_buf = (uint8_t *)malloc(buf_len);
    if (!tmp_buf) {
        ESP_LOGE(TAG, "Out of memory for live frame temp buffer");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Copy và Swap
    for (size_t i = 0; i < buf_len; i += 3) {
        tmp_buf[i]     = ai_rgb888_buf[i + 2]; // R <- B
        tmp_buf[i + 1] = ai_rgb888_buf[i + 1]; // G <- G
        tmp_buf[i + 2] = ai_rgb888_buf[i];     // B <- R
    }

    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    
    // Nén ảnh RGB888 thành JPEG
    bool converted = fmt2jpg(tmp_buf, buf_len, 320, 240, PIXFORMAT_RGB888, 50, &jpg_buf, &jpg_len);
    free(tmp_buf); // Dùng xong giải phóng ngay

    if (!converted) {
        ESP_LOGE(TAG, "Live frame JPEG compression failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_send(req, (const char *)jpg_buf, jpg_len);
    free(jpg_buf);
    
    return ESP_OK;
}

static esp_err_t detect_status_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char resp[160];
    if (!s_ai_enabled && !sensor_check_distance_detected()) {
        snprintf(resp, sizeof(resp), "AI IS OFF");
    } else if (current_vision_state == VISION_SPOOFING_DETECTED || s_spoofing_detected) {
        snprintf(resp, sizeof(resp), "🚨 SPOOFING DETECTED (FAKE FACE) - ALARM ACTIVE 🚨");
    } else if (current_vision_state == VISION_KNOWN_PERSON) {
        float delta = s_last_smile_ratio;
        if (s_liveness_passed) {
            if (s_liveness_enabled) {
                snprintf(resp, sizeof(resp), "KNOWN PERSON: %s | Liveness: PASSED 😀 (DeltaSmile: %.3f)", 
                         recognized_name, delta);
            } else {
                snprintf(resp, sizeof(resp), "KNOWN PERSON: %s | 🔓 DOOR UNLOCKED (Liveness OFF)", 
                         recognized_name);
            }
        } else {
            int elapsed_sec = 0;
            if (s_known_detected_start_time > 0) {
                elapsed_sec = (int)((esp_timer_get_time() - s_known_detected_start_time) / 1000000LL);
            }
            if (!s_liveness_enabled) {
                int remaining = 2 - elapsed_sec;
                if (remaining < 0) remaining = 0;
                snprintf(resp, sizeof(resp), "KNOWN PERSON: %s | Liveness OFF ⏱ Unlocking in %ds...", 
                         recognized_name, remaining);
            } else {
                int remaining = 10 - elapsed_sec;
                if (remaining < 0) remaining = 0;
                if (elapsed_sec < 5) {
                    snprintf(resp, sizeof(resp), "KNOWN PERSON: %s | 🔴 Giữ mặt lạnh... ⏱ %ds (delta: %.3f < %.2f)", 
                             recognized_name, 5 - elapsed_sec, delta, NEUTRAL_MAX_THRESHOLD);
                } else {
                    snprintf(resp, sizeof(resp), "KNOWN PERSON: %s | Challenge: Smile! ⏱ %ds (Smile: %.3f/%.2f)", 
                             recognized_name, remaining, delta, SMILE_THRESHOLD);
                }
            }
        }
    } else if (current_vision_state == VISION_STRANGER) {
        snprintf(resp, sizeof(resp), "STRANGER DETECTED (Liveness Skipped)");
    } else {
        snprintf(resp, sizeof(resp), "Scanning...");
    }
    httpd_resp_set_type(req, "text/plain; charset=UTF-8");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t ai_on_handler(httpd_req_t *req) {
    s_ai_enabled         = true;
    current_vision_state = VISION_IDLE;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t ai_off_handler(httpd_req_t *req) {
    s_ai_enabled         = false;
    current_vision_state = VISION_IDLE;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t liveness_on_handler(httpd_req_t *req) {
    s_liveness_enabled = true;
    if (s_known_detected_start_time > 0) {
        s_known_detected_start_time = 0;
        s_last_recognized_id = -1;
        current_vision_state = VISION_IDLE;
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t liveness_off_handler(httpd_req_t *req) {
    s_liveness_enabled = false;
    if (s_known_detected_start_time > 0) {
        s_known_detected_start_time = 0;
        s_last_recognized_id = -1;
        current_vision_state = VISION_IDLE;
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

// REST API: Lấy danh sách bảng NguoiQuen
static esp_err_t api_persons_get_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    cJSON *db = db_person_get_all_json();
    char *out = cJSON_PrintUnformatted(db);
    cJSON_Delete(db);
    
    ESP_LOGI(TAG, "api_persons_get_handler returning: %s", out ? out : "NULL");
    
    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    if (out) {
        httpd_resp_send(req, out, strlen(out));
        free(out);
    } else {
        httpd_resp_send(req, "[]", 2);
    }
    return ESP_OK;
}

// REST API: Xóa NguoiQuen theo ID (ví dụ: GET /api/persons/delete?id=P1)
static esp_err_t api_persons_delete_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char person_id[16] = {0};
        if (httpd_query_key_value(query, "id", person_id, sizeof(person_id)) == ESP_OK) {
            esp_err_t err = db_person_delete(person_id);
            if (err == ESP_OK) {
                if (s_recognizer_mutex) xSemaphoreTake(s_recognizer_mutex, portMAX_DELAY);
                // Khởi tạo lại recognizer để giải phóng RAM và đồng bộ lại với SD Card
                if (s_recognizer) {
                    delete s_recognizer;
                    s_recognizer = new FaceRecognition112V1S8();
                    s_recognizer->set_thresh(THRESHOLD_DISTANCE);
                    load_faces_from_sd_locked();
                }
                if (s_recognizer_mutex) xSemaphoreGive(s_recognizer_mutex);
                httpd_resp_send(req, "{\"status\":\"success\"}", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }
        }
    }
    httpd_resp_send_404(req);
    return ESP_OK;
}

// REST API: Lấy danh sách bảng NhatKyRaVao
static esp_err_t api_logs_get_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    cJSON *logs = db_log_get_all_json();
    char *out = cJSON_PrintUnformatted(logs);
    cJSON_Delete(logs);
    
    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    if (out) {
        httpd_resp_send(req, out, strlen(out));
        free(out);
    } else {
        httpd_resp_send(req, "[]", 2);
    }
    return ESP_OK;
}

static void door_open_task(void *pv) {
    open_door();
    vTaskDelete(NULL);
}

static esp_err_t api_door_open_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    ESP_LOGI(TAG, "API: /api/door_open called");
    xTaskCreate(door_open_task, "door_open", 2048, NULL, 5, NULL);
    httpd_resp_send(req, "OK: Door Opening", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_door_close_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    ESP_LOGI(TAG, "API: /api/door_close called");
    close_door();
    httpd_resp_send(req, "OK: Door Closed", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// =============================================================================
// REMOTE COMMAND POLLING TASK (Core 0)
// =============================================================================
/* TẠM THỜI VÔ HIỆU HÓA THEO YÊU CẦU TEST PHẦN CỨNG
static void remote_command_poll_task(void *pv) {
    char id_buf[32];
    char cmd_buf[32];
    while (1) {
        if (!network_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (supabase_get_pending_command(id_buf, sizeof(id_buf), cmd_buf, sizeof(cmd_buf)) == ESP_OK) {
            ESP_LOGI(TAG, "Received remote command: %s (ID: %s)", cmd_buf, id_buf);
            if (strcmp(cmd_buf, "open_door") == 0) {
                open_door(); // This blocks for 5 seconds
                supabase_update_command_status(id_buf, "completed");
            } else {
                ESP_LOGW(TAG, "Unknown remote command: %s", cmd_buf);
                supabase_update_command_status(id_buf, "failed");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
*/

// =============================================================================
// CAMERA & I2C INIT
// =============================================================================

// =============================================================================
// KHỞI TẠO CAMERA VÀ WEB SERVER
// =============================================================================

bool vision_init(void) {
    ESP_LOGI(TAG, "Initializing Vision module...");

    // Khởi tạo hệ thống CSDL Thẻ nhớ SD Card
    db_manager_init();

    camera_config_t config = {};
    config.ledc_channel  = LEDC_CHANNEL_0;
    config.ledc_timer    = LEDC_TIMER_0;
    config.pin_d0        = CAM_PIN_D0;
    config.pin_d1        = CAM_PIN_D1;
    config.pin_d2        = CAM_PIN_D2;
    config.pin_d3        = CAM_PIN_D3;
    config.pin_d4        = CAM_PIN_D4;
    config.pin_d5        = CAM_PIN_D5;
    config.pin_d6        = CAM_PIN_D6;
    config.pin_d7        = CAM_PIN_D7;
    config.pin_xclk      = CAM_PIN_XCLK;
    config.pin_pclk      = CAM_PIN_PCLK;
    config.pin_vsync     = CAM_PIN_VSYNC;
    config.pin_href      = CAM_PIN_HREF;
    config.pin_sccb_sda  = CAM_PIN_SIOD;
    config.pin_sccb_scl  = CAM_PIN_SIOC;
    config.pin_pwdn      = CAM_PIN_PWDN;
    config.pin_reset     = CAM_PIN_RESET;
    config.xclk_freq_hz  = 10000000;      // 10MHz: cân bằng giữa FPS và chất lượng Keypoint
    config.frame_size    = FRAMESIZE_QVGA;   // 320×240
    config.pixel_format  = PIXFORMAT_YUV422; // OV2640 hỗ trợ native, màu chuẩn cho model AI
    config.grab_mode     = CAMERA_GRAB_LATEST; // Lấy khung hình mới nhất, tự bỏ khung cũ để triệt tiêu lỗi EV-VSYNC-OVF
    config.fb_location   = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality  = 12;
    config.fb_count      = 2;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init FAILED: 0x%x", err);
        return false;
    }

    // Cấu hình cảm biến OV2640 - Tối ưu phơi sáng & tăng nhạy sáng trong môi trường thiếu sáng
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_whitebal(s, 1);        // Auto white balance ON
        s->set_awb_gain(s, 1);        // AWB gain ON
        s->set_exposure_ctrl(s, 1);   // Auto exposure ON
        s->set_gain_ctrl(s, 1);       // Auto gain ON
        s->set_aec2(s, 1);            // Bật thuật toán tự động phơi sáng DSP nâng cao
        s->set_gainceiling(s, GAINCEILING_16X); // Tăng hệ số khuếch đại nhạy sáng cực đại 16X khi trời tối
        s->set_bpc(s, 1);             // Sửa điểm ảnh nhiễu tối (Black Pixel Correction)
        s->set_wpc(s, 1);             // Sửa điểm ảnh nhiễu sáng (White Pixel Correction)
        ESP_LOGI(TAG, "OV2640 sensor PID=0x%X configured (Low-light enhanced: GAINCEILING_16X, AEC2, BPC/WPC).", s->id.PID);
    }

    // Khởi tạo mô hình AI
    if (s_recognizer_mutex == NULL) {
        s_recognizer_mutex = xSemaphoreCreateMutex();
    }

    ESP_LOGI(TAG, "Loading AI models into PSRAM...");
    // Khởi tạo MSR01 với resize_scale = 0.5F (chuẩn của ESP-DL) để Face lọt vừa các Anchor Box, Score = 0.10F cực nhạy
    s_detector   = new HumanFaceDetectMSR01(0.10F, 0.5F, 10, 0.5F); // Score=0.10F, Resize=0.5F
    s_detector2  = new HumanFaceDetectMNP01(0.30F, 0.3F, 5);        // Stage 2 MNP01 (Hạ Score=0.30F để không rớt mặt khi cười quá to)
    s_recognizer = new FaceRecognition112V1S8();
    s_recognizer->set_thresh(THRESHOLD_DISTANCE);

    // Tải khuôn mặt đã đăng ký từ SD Card
    load_faces_from_sd();

    // Cấp phát buffer RGB888 trong PSRAM (320×240×3 = 230400 bytes)
    ai_rgb888_buf = (uint8_t *)heap_caps_malloc(320 * 240 * 3, MALLOC_CAP_SPIRAM);
    if (!ai_rgb888_buf) {
        ESP_LOGE(TAG, "FATAL: Cannot allocate RGB888 buffer in PSRAM!");
        return false;
    }

    // Cấp phát buffer YUV422 copy trong PSRAM (320×240×2 = 153600 bytes)
    // Dùng để copy raw frame ngay, trả camera DMA, rồi mới xử lý nén/AI
    s_yuv_copy_buf = (uint8_t *)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
    if (!s_yuv_copy_buf) {
        ESP_LOGE(TAG, "FATAL: Cannot allocate YUV copy buffer in PSRAM!");
        return false;
    }

    // Tạo AI Task trên Core 1 với stack 16KB
    BaseType_t ret = xTaskCreatePinnedToCore(
        ai_task, "ai_task", AI_TASK_STACK_SIZE, NULL, 5, NULL, 1
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "FATAL: Cannot create ai_task!");
        return false;
    }

    // Tạo Remote Command Poll Task trên Core 0
    // TẠM VÔ HIỆU HÓA: xTaskCreatePinnedToCore(remote_command_poll_task, "remote_poll", 4096, NULL, 3, NULL, 0);

    ESP_LOGI(TAG, "Vision initialized. PSRAM free: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

void vision_start_web_server(void) {
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.ctrl_port        = 32768;
    cfg.max_open_sockets = 7;
    cfg.max_uri_handlers = 30;  // vision uris (11) + test uris (11) + buffer
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout  = 10;
    cfg.send_wait_timeout  = 30;  // tăng lên để tránh EAGAIN khi poll nhanh
    cfg.stack_size         = 10240; // Tăng stack để HTTP client gọi Supabase không bị tràn

    httpd_uri_t uris[] = {
        { .uri = "/",                   .method = HTTP_GET, .handler = index_handler,               .user_ctx = NULL },
        { .uri = "/enroll",             .method = HTTP_GET, .handler = enroll_handler,              .user_ctx = NULL },
        { .uri = "/ai_on",              .method = HTTP_GET, .handler = ai_on_handler,               .user_ctx = NULL },
        { .uri = "/ai_off",             .method = HTTP_GET, .handler = ai_off_handler,              .user_ctx = NULL },
        { .uri = "/liveness_on",        .method = HTTP_GET, .handler = liveness_on_handler,         .user_ctx = NULL },
        { .uri = "/liveness_off",       .method = HTTP_GET, .handler = liveness_off_handler,        .user_ctx = NULL },
        { .uri = "/detect_status",      .method = HTTP_GET, .handler = detect_status_handler,       .user_ctx = NULL },
        { .uri = "/snapshot.jpg",       .method = HTTP_GET, .handler = snapshot_handler,           .user_ctx = NULL },
        { .uri = "/api/persons",        .method = HTTP_GET, .handler = api_persons_get_handler,     .user_ctx = NULL },
        { .uri = "/api/persons/delete", .method = HTTP_GET, .handler = api_persons_delete_handler,  .user_ctx = NULL },
        { .uri = "/api/logs",           .method = HTTP_GET, .handler = api_logs_get_handler,        .user_ctx = NULL },
        { .uri = "/live_frame.jpg",     .method = HTTP_GET, .handler = live_frame_handler,          .user_ctx = NULL },
        { .uri = "/api/door_open",      .method = HTTP_GET, .handler = api_door_open_handler,       .user_ctx = NULL },
        { .uri = "/api/door_close",     .method = HTTP_GET, .handler = api_door_close_handler,      .user_ctx = NULL },
    };

    int uri_count = sizeof(uris) / sizeof(uris[0]);
    if (httpd_start(&camera_httpd, &cfg) == ESP_OK) {
        for (int i = 0; i < uri_count; i++) {
            httpd_register_uri_handler(camera_httpd, &uris[i]);
        }

        ESP_LOGI(TAG, "Web Server started on port 80.");
    } else {
        ESP_LOGE(TAG, "Web Server FAILED to start!");
    }
}

// =============================================================================
// API CÔNG KHAI (Public API for main.cpp)
// =============================================================================

vision_state_t vision_process_frame(void) {
    return (vision_state_t)current_vision_state;
}

bool vision_check_liveness(void) {
    return s_liveness_passed;
}

bool vision_add_known_face(int id, face_vector_t *vec) {
    if (!s_recognizer || !vec) return false;
    dl::Tensor<float> tensor_emb;
    tensor_emb.set_element((float *)vec->vector).set_shape({FACE_VECTOR_SIZE}).set_auto_free(false);
    char name[16];
    snprintf(name, sizeof(name), "ID_%d", id);
    s_recognizer->enroll_id(tensor_emb, std::string(name), false);

    char path[512];
    snprintf(path, sizeof(path), "%s/%d.bin", SD_CARD_FACES_DIR, id);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(vec->vector, sizeof(float), FACE_VECTOR_SIZE, f);
        fclose(f);
        ESP_LOGI(TAG, "vision_add_known_face: Saved ID %d to SD.", id);
        return true;
    }
    ESP_LOGE(TAG, "vision_add_known_face: Failed to write SD at %s", path);
    return false;
}

bool vision_delete_enrolled_face(const char* person_id) {
    if (!person_id) return false;
    bool found = false;
    
    if (s_recognizer_mutex) xSemaphoreTake(s_recognizer_mutex, portMAX_DELAY);
    
    int index_to_delete = -1;
    for (size_t i = 0; i < s_enrolled_person_ids.size(); i++) {
        if (s_enrolled_person_ids[i] == person_id) {
            index_to_delete = i + 1; // Face ID is 1-indexed in esp-dl
            found = true;
            break;
        }
    }
    
    if (found && s_recognizer) {
        // Xóa khuôn mặt khỏi RAM
        s_recognizer->delete_id(index_to_delete, false);
        // Xóa ID khỏi danh sách mapping
        s_enrolled_person_ids.erase(s_enrolled_person_ids.begin() + (index_to_delete - 1));
        ESP_LOGI(TAG, "MQTT: Đã xóa khuôn mặt ID %s khỏi RAM.", person_id);
    }
    
    if (s_recognizer_mutex) xSemaphoreGive(s_recognizer_mutex);

    if (found) {
        // Xóa dữ liệu cứng trên Thẻ nhớ SD (gọi hàm của db_manager)
        db_person_delete(person_id);
        ESP_LOGI(TAG, "MQTT: Đã xóa toàn bộ dữ liệu %s khỏi Thẻ SD.", person_id);
    } else {
        ESP_LOGW(TAG, "MQTT: Không tìm thấy khuôn mặt ID %s để xóa.", person_id);
    }
    
    return found;
}

void vision_trigger_remote_enroll(const char* name) {
    if (!name) return;
    strncpy(s_enroll_name, name, sizeof(s_enroll_name) - 1);
    s_enroll_name[sizeof(s_enroll_name) - 1] = '\0';
    
    s_enroll_result = 0;
    s_enroll_requested = true;
    ESP_LOGI(TAG, "MQTT: Bắt đầu tiến trình Remote Enroll cho '%s'. Vui lòng nhìn vào Camera...", s_enroll_name);
}
