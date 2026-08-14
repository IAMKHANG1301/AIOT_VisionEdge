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
#include "tft_driver.h"

#pragma GCC push_options
#pragma GCC optimize ("O0")
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wformat"
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#pragma GCC diagnostic pop
#pragma GCC pop_options

extern "C" {
#include "supabase_client.h"
}

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

static uint8_t *s_ei_crop_buf = NULL;
static int get_ei_signal_data(size_t offset, size_t length, float *out_ptr) {
    if (s_ei_crop_buf == NULL) return -1;
    for (size_t i = 0; i < length; i++) {
        uint8_t r = s_ei_crop_buf[(offset + i) * 3 + 0];
        uint8_t g = s_ei_crop_buf[(offset + i) * 3 + 1];
        uint8_t b = s_ei_crop_buf[(offset + i) * 3 + 2];
        out_ptr[i] = (r << 16) + (g << 8) + b;
    }
    return 0;
}

static void resize_rgb888(const uint8_t* src, int src_w, int src_h, uint8_t* dst, int dst_w, int dst_h) {
    float x_ratio = ((float)(src_w - 1)) / dst_w;
    float y_ratio = ((float)(src_h - 1)) / dst_h;
    for (int i = 0; i < dst_h; i++) {
        for (int j = 0; j < dst_w; j++) {
            int x_l = (int)(x_ratio * j);
            int y_l = (int)(y_ratio * i);
            int x_h = (x_l < src_w - 1) ? x_l + 1 : x_l;
            int y_h = (y_l < src_h - 1) ? y_l + 1 : y_l;
            float x_weight = (x_ratio * j) - x_l;
            float y_weight = (y_ratio * i) - y_l;
            for(int c=0; c<3; c++) {
                float a = src[(y_l * src_w + x_l) * 3 + c];
                float b = src[(y_l * src_w + x_h) * 3 + c];
                float c_v = src[(y_h * src_w + x_l) * 3 + c];
                float d = src[(y_h * src_w + x_h) * 3 + c];
                dst[(i * dst_w + j) * 3 + c] = a * (1 - x_weight) * (1 - y_weight) + 
                                               b * x_weight * (1 - y_weight) + 
                                               c_v * y_weight * (1 - x_weight) + 
                                               d * x_weight * y_weight;
            }
        }
    }
}

#define SD_CARD_FACES_DIR "/sdcard/faces"
#define AI_TASK_STACK_SIZE 16384  // 16KB: đủ cho AI model + face recognition

// Dataset Capture Mode
static volatile int s_dataset_capture_mode = 0; // 0=off, 1=real, 2=fake
static volatile int s_dataset_capture_count = 0;

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
static volatile bool s_stranger_detected = false;
static volatile float s_last_smile_ratio = 0.0f; // Giữ lại để Web UI hiển thị
static volatile int s_smile_consecutive_frames = 0;
static volatile float s_initial_ratio = -1.0f;
static volatile float s_smooth_ratio = -1.0f;
static volatile float s_slow_baseline_ratio = -1.0f;
static volatile int64_t s_known_detected_start_time = 0;

static void bg_upload_task(void* pv) {
    const char *filepath = "/sdcard/snapshot.jpg";
    esp_err_t upload_result = upload_image_to_hf(filepath, "unknown");
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

    // s_yuv_copy_buf hiện đang chứa dữ liệu JPEG gốc từ camera (đã nén phần cứng bởi OV2640)
    // Không cần gọi fmt2jpg nữa — dùng thẳng JPEG từ PSRAM copy buffer
    uint8_t *jpg_buf = s_yuv_copy_buf;
    size_t   jpg_len = s_snapshot_len; // s_snapshot_len được set trong vòng lặp infer cùng lúc với copy

    // Fallback: nếu s_snapshot_len chưa được set, dùng copy_len từ lần copy gần nhất
    // (ước tính tối đa 25KB cho JPEG QVGA chất lượng 10)
    if (jpg_len == 0) {
        ESP_LOGW(TAG, "trigger_snapshot_and_upload: s_snapshot_len=0, bỏ qua");
        return false;
    }

    if (jpg_buf != NULL && jpg_len > 0) {
        // 1. Thêm Log vào DB (và gọi API Supabase) - Không lưu cho lệnh manual
        if (strcmp(log_label, "manual") != 0) {
            db_log_add(log_label, person_id, jpg_buf, jpg_len);
        }

        // 2. Lưu file ra thẻ nhớ để HuggingFace WebSocket task đọc
        const char *filepath = "/sdcard/snapshot.jpg";
        FILE *f = fopen(filepath, "wb");
        if (f) {
            fwrite(jpg_buf, 1, jpg_len, f);
            fclose(f);
            ESP_LOGI(TAG, "Saved %s snapshot to %s (%zu bytes, JPEG from camera HW)", hf_label, filepath, jpg_len);
            
            // KHI PHÁT HIỆN NGƯỜI LẠ (STRANGER): Kích hoạt luồng gửi ảnh lên Hugging Face 
            // Đã chuyển lệnh gọi upload_image_to_hf sang db_manager.cpp để chạy nối tiếp sau khi upload Supabase
            // nhằm tránh lỗi thiếu RAM mbedtls (chạy 2 SSL cùng lúc).
            if (strcmp(log_label, "unknown") == 0) {
                // xTaskCreate(bg_upload_task, "bg_upload", 10240, NULL, 4, NULL);
            }
        } else {
            ESP_LOGE(TAG, "Failed to open %s for writing!", filepath);
        }
        // KHÔNG free(jpg_buf) vì đây là con trỏ trỏ vào PSRAM s_yuv_copy_buf, không phải malloc riêng
        return true;
    } else {
        ESP_LOGE(TAG, "Snapshot JPEG not available for %s", log_label);
    }
    return false;
}

void vision_trigger_snapshot(void) {
    ESP_LOGI(TAG, "Lệnh chụp ảnh thủ công từ MQTT");
    trigger_snapshot_and_upload("manual", NULL, "MANUAL");
}

static volatile int64_t s_last_face_seen_time = 0;
static volatile int64_t s_face_first_seen_time = 0;
static volatile int s_last_recognized_id = -2;

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
// SENSOR KHOẢNG CÁCH (HC-SR04)
// =============================================================================
bool sensor_check_distance_detected(void) {
    return is_person_near();
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
    bool was_sensor_active = false;
    bool face_prompt_shown = false;
    int64_t s_sensor_trigger_time = 0;

    while (1) {
        bool sensor_active = sensor_check_distance_detected();

        // Check rising edge of sensor
        if (sensor_active && !was_sensor_active) {
            ESP_LOGI(TAG, "Sensor detected person < 100cm");
            tft_update_ui(TFT_COLOR_WHITE, "Kính chào quý khách", NULL, NULL);
            s_sensor_trigger_time = esp_timer_get_time();
            face_prompt_shown = false;
            // Delay recognition so user can read the greeting
            last_detection_time = esp_timer_get_time() + 1000000LL; 
            s_known_detected_start_time = 0;
            s_liveness_passed = false;
            s_spoofing_detected = false;
            s_stranger_detected = false;
            current_vision_state = VISION_IDLE;
        } else if (!sensor_active && was_sensor_active) {
            ESP_LOGI(TAG, "Person left");
            tft_update_ui(TFT_COLOR_WHITE, "Đang chờ khách...", NULL, NULL);
            current_vision_state = VISION_IDLE;
        }
        
        was_sensor_active = sensor_active;
        
        if (sensor_active && !face_prompt_shown && current_vision_state == VISION_IDLE) {
            if ((esp_timer_get_time() - s_sensor_trigger_time) > 1500000LL) {
                tft_update_ui(TFT_COLOR_WHITE, "Vui lòng nhìn thẳng vào camera", "và đừng rời khỏi bán kính 100cm", NULL);
                face_prompt_shown = true;
            }
        }

        // Chờ khi AI tắt và không có yêu cầu enroll
        if (!s_ai_enabled && !s_enroll_requested && !sensor_active) {
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


        // --- Kiểm tra tính hợp lệ của frame JPEG ---
        // OV2640 nén JPEG bằng phần cứng: frame size thay đổi theo nội dung (~5-25KB)
        bool frame_ok = (fb->format == PIXFORMAT_JPEG)
                     && (fb->width  == 320)
                     && (fb->height == 240)
                     && (fb->len    > 0);

        if (!frame_ok) {
            ESP_LOGW(TAG, "Bad frame: fmt=%d w=%d h=%d len=%zu.",
                     fb->format, fb->width, fb->height, fb->len);
            esp_camera_fb_return(fb);
            if (s_enroll_requested) { s_enroll_result = -1; s_enroll_requested = false; }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // --- Copy JPEG frame sang PSRAM copy buffer (để dùng cho Web streaming & upload) ---
        size_t copy_len = fb->len; // JPEG frame nhỏ (~5-25KB), copy toàn bộ
        if (copy_len > (320 * 240 * 2)) copy_len = 320 * 240 * 2; // Giới hạn an toàn
        memcpy(s_yuv_copy_buf, fb->buf, copy_len);
        s_yuv_copy_w   = fb->width;
        s_yuv_copy_h   = fb->height;
        s_snapshot_len = copy_len; // Cập nhật mỗi frame để trigger_snapshot_and_upload luôn có JPEG hợp lệ

        // --- Convert JPEG → RGB888 cho AI inference ---
        // fmt2rgb888 với PIXFORMAT_JPEG sẽ tự động giải nén JPEG, màu sắc chuẩn xác
        bool converted = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, ai_rgb888_buf);


        // NẾU CÓ LỆNH ENROLL, GIỮ NGUYÊN JPEG TỪ PSRAM COPY BUFFER ---
        if (s_enroll_requested) {
            // JPEG đã được copy vào s_yuv_copy_buf phía trên, dùng copy_len
            if (copy_len == 0) {
                ESP_LOGE(TAG, "Snapshot: copy_len == 0, bỏ qua.");
            } else {
                if (s_snapshot_buf != NULL) {
                    heap_caps_free(s_snapshot_buf);
                    s_snapshot_buf = NULL;
                }
                // Lưu JPEG vào PSRAM riêng để phục vụ Web UI
                s_snapshot_buf = (uint8_t *)heap_caps_malloc(copy_len, MALLOC_CAP_SPIRAM);
                if (s_snapshot_buf != NULL) {
                    memcpy(s_snapshot_buf, s_yuv_copy_buf, copy_len);
                    s_snapshot_len = copy_len;
                    ESP_LOGI(TAG, "Snapshot JPEG saved to PSRAM: %zu bytes", s_snapshot_len);
                } else {
                    ESP_LOGE(TAG, "Failed to allocate PSRAM for snapshot!");
                }
            }
        }

        // === TRẢ FRAME BUFFER CHO CAMERA DMA NGAY LẬP TỨC! ===
        // Trả fb ngay tại đây (<1ms) để camera driver giải phóng DMA buffer,
        // triệt tiêu hoàn toàn lỗi cam_hal timeout và EV-VSYNC-OVF!
        esp_camera_fb_return(fb);
        fb = NULL;

        if (!converted) {
            ESP_LOGE(TAG, "JPEG→RGB888 conversion FAILED!");
            if (s_enroll_requested) { s_enroll_result = -1; s_enroll_requested = false; }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // --- Đã gỡ bỏ đoạn code Swap RGB↔BGR tại đây ---
        // Lý do: fmt2rgb888 xuất ra BGR/RGB tuỳ phiên bản camera driver.
        // Việc swap cứng sẽ làm sai màu màn hình Web UI (bị ám xanh).
        // Mô hình esp-dl (MobileFaceNet) vẫn hoạt động tốt kể cả khi R và B bị đổi chỗ.

        // --- Tạm thời TẮT CLAHE vì nó làm biến dạng màu sắc (HDR ảo) khiến MobileFaceNet (nhận diện ID) bị sai lệch ---
        // apply_clahe_bgr888(ai_rgb888_buf, 320, 240);

        // --- Chẩn đoán: In pixel trung tâm mỗi 100 vòng lặp để xác nhận ảnh hợp lệ ---
        if (loop_count++ % 100 == 0) {
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
            // ESP_LOGI(TAG, "[VISION] Infer: %lld ms | Faces: 0", infer_ms);
            // Dung thứ thời gian mất mặt ngắn: Nâng lên 2.5s để tránh reset TMV buffer khi face bị rớt nhất thời
            if (s_last_face_seen_time > 0 && (esp_timer_get_time() - s_last_face_seen_time) > 2500000LL) {
                s_known_detected_start_time = 0;
                s_initial_ratio = -1.0f;
                s_smooth_ratio = -1.0f;
                s_last_recognized_id = -2;
                s_face_first_seen_time = 0;
                s_liveness_passed = false;
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
        
        if (s_face_first_seen_time == 0) {
            s_face_first_seen_time = now_us;
            s_last_recognized_id = -2;
            ESP_LOGI(TAG, "Face detected! Waiting 2s to stabilize...");
            tft_update_ui(TFT_COLOR_WHITE, "Đang nhận diện...", "Vui lòng giữ nguyên khuôn mặt", NULL);
        }

        // Có khuôn mặt: lấy mặt tốt nhất (tin cậy cao nhất)
        dl::detect::result_t best_face = results.front();
        dl::Tensor<uint8_t> image_tensor;
        image_tensor.set_element(ai_rgb888_buf).set_shape({240, 320, 3}).set_auto_free(false);

        // NẾU ĐANG CHỤP DATASET (CHỈ LẤY ẢNH KHI CÓ MẶT) ---
        if (s_dataset_capture_mode > 0 && s_dataset_capture_count < 100) {
            int x1 = best_face.box[0];
            int y1 = best_face.box[1];
            int x2 = best_face.box[2];
            int y2 = best_face.box[3];
            
            // Mở rộng viền (Margin) 15 pixel để lấy toàn bộ khuôn mặt và viền điện thoại
            x1 -= 15; y1 -= 15; x2 += 15; y2 += 15;
            if (x1 < 0) { x1 = 0; }
            if (y1 < 0) { y1 = 0; }
            if (x2 > 319) { x2 = 319; }
            if (y2 > 239) { y2 = 239; }
            
            int crop_w = x2 - x1 + 1;
            int crop_h = y2 - y1 + 1;
            
            uint8_t *crop_buf = (uint8_t*)heap_caps_malloc(crop_w * crop_h * 3, MALLOC_CAP_SPIRAM);
            if (crop_buf) {
                for(int i = 0; i < crop_h; i++) {
                    memcpy(crop_buf + i * crop_w * 3, ai_rgb888_buf + ((y1 + i) * 320 + x1) * 3, crop_w * 3);
                }
                
                uint8_t *out_jpg = NULL;
                size_t out_len = 0;
                if (fmt2jpg(crop_buf, crop_w * crop_h * 3, crop_w, crop_h, PIXFORMAT_RGB888, 90, &out_jpg, &out_len)) {
                    char filepath[64];
                    snprintf(filepath, sizeof(filepath), "/sdcard/dataset/%s/%d.jpg", 
                        (s_dataset_capture_mode == 1) ? "real" : "fake",
                        s_dataset_capture_count + 1);
                    
                    FILE *f = fopen(filepath, "wb");
                    if (f) {
                        fwrite(out_jpg, 1, out_len, f);
                        fclose(f);
                        ESP_LOGI(TAG, "Dataset cropped face saved: %s (%zu bytes)", filepath, out_len);
                        s_dataset_capture_count++;
                    } else {
                        ESP_LOGE(TAG, "Lỗi tạo file %s", filepath);
                    }
                    free(out_jpg);
                }
                heap_caps_free(crop_buf);
            }
            
            if (s_dataset_capture_count >= 100) {
                s_dataset_capture_mode = 0; // done
                ESP_LOGI(TAG, "Hoàn tất chụp dataset!");
            }
            
            vTaskDelay(pdMS_TO_TICKS(100)); // Khoảng cách 100ms giữa 2 ảnh (nhanh hơn vì có chọn lọc)
            continue; // Bỏ qua nhận diện Face ID cho frame này
        }

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
                    ESP_LOGI(TAG, "[WEB] Thêm người quen thành công: ID = %s, Tên = %s", created_id, person_name);
                    s_enroll_result = 1;
                    
                    // MQTT: Gửi thông báo thành công về cho Vercel lưu database
                    char mqtt_payload[256];
                    snprintf(mqtt_payload, sizeof(mqtt_payload), 
                        "{\"status\":\"enroll_success\",\"name\":\"%s\",\"id\":\"%s\"}", 
                        person_name, created_id);
                    mqtt_publish_status(mqtt_payload);
                } else {
                    ESP_LOGE(TAG, "[WEB] Thêm người quen thất bại: Lỗi ghi SD Card");
                    s_enroll_result = -2;
                    char mqtt_payload[256];
                    snprintf(mqtt_payload, sizeof(mqtt_payload), "{\"action\":\"enroll_face\",\"status\":\"error\",\"message\":\"Lỗi ghi SD Card\"}");
                    mqtt_publish_status(mqtt_payload);
                }
            } else {
                ESP_LOGW(TAG, "[WEB] Thêm người quen thất bại: Không nhận diện được khuôn mặt");
                s_enroll_result = -1;
                char mqtt_payload[256];
                snprintf(mqtt_payload, sizeof(mqtt_payload), "{\"action\":\"enroll_face\",\"status\":\"error\",\"message\":\"Không nhận diện được khuôn mặt\"}");
                mqtt_publish_status(mqtt_payload);
            }
            if (s_recognizer_mutex) xSemaphoreGive(s_recognizer_mutex);
            s_enroll_requested = false;

            // Pause recognition for 5 seconds after enrollment to prevent immediate door opening
            s_face_first_seen_time = 0;
            s_last_recognized_id = -2;
            last_detection_time = esp_timer_get_time() + 5000000LL;
            ESP_LOGI(TAG, "Enrollment finished, pausing recognition for 5 seconds...");

        } else {
            // --- 1. Nhận diện ID khuôn mặt ---
            bool challenge_active = (s_known_detected_start_time > 0 && !s_spoofing_detected && !s_liveness_passed);
            bool waited_2s = (now_us - s_face_first_seen_time) >= 2000000LL;
            
            if (!challenge_active && waited_2s && (now_us - last_detection_time) >= DETECTION_COOLDOWN_US) {
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


            // --- 2. XÁC NHẬN DANH TÍNH (TỨC THÌ) ---
            if (s_last_recognized_id == -2) {
                // Đang trong thời gian 2s chờ ổn định, chưa quyết định
            } else if (s_last_recognized_id > 0) {
                // NẾU LÀ NGƯỜI QUEN:
                if (!s_liveness_passed) { 
                    
                    // --- BƯỚC THẨM ĐỊNH ANTI-SPOOFING BẰNG EDGE IMPULSE ---
                    ESP_LOGI(TAG, "Running Edge Impulse Anti-Spoofing...");
                    int x1 = best_face.box[0]; int y1 = best_face.box[1];
                    int x2 = best_face.box[2]; int y2 = best_face.box[3];
                    x1 -= 15; y1 -= 15; x2 += 15; y2 += 15;
                    if (x1 < 0) { x1 = 0; } if (y1 < 0) { y1 = 0; }
                    if (x2 > 319) { x2 = 319; } if (y2 > 239) { y2 = 239; }
                    int crop_w = x2 - x1 + 1; int crop_h = y2 - y1 + 1;
                    
                    uint8_t *crop_buf = (uint8_t*)heap_caps_malloc(crop_w * crop_h * 3, MALLOC_CAP_SPIRAM);
                    s_ei_crop_buf = (uint8_t*)heap_caps_malloc(96 * 96 * 3, MALLOC_CAP_SPIRAM);
                    
                    bool is_real = false;
                    float real_score = 0.0f;
                    
                    if (crop_buf && s_ei_crop_buf) {
                        for(int i = 0; i < crop_h; i++) {
                            memcpy(crop_buf + i * crop_w * 3, ai_rgb888_buf + ((y1 + i) * 320 + x1) * 3, crop_w * 3);
                        }
                        resize_rgb888(crop_buf, crop_w, crop_h, s_ei_crop_buf, 96, 96);
                        
                        signal_t signal;
                        signal.total_length = 96 * 96;
                        signal.get_data = &get_ei_signal_data;
                        ei_impulse_result_t result = { 0 };
                        EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);
                        
                        if (res == EI_IMPULSE_OK) {
                            for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
                                ESP_LOGI(TAG, "  %s: %.5f", result.classification[i].label, result.classification[i].value);
                                if (strcmp(result.classification[i].label, "real") == 0) {
                                    real_score = result.classification[i].value;
                                }
                            }
                            if (real_score > 0.85f) {
                                is_real = true;
                            }
                        }
                    }
                    if (crop_buf) heap_caps_free(crop_buf);
                    if (s_ei_crop_buf) heap_caps_free(s_ei_crop_buf);
                    s_ei_crop_buf = NULL;
                    
                    // --- ĐÁNH GIÁ KẾT QUẢ ---
                    if (!is_real) {
                        current_vision_state = VISION_STRANGER;
                        if (!s_spoofing_detected) {
                            s_spoofing_detected = true;
                            s_stranger_detected = false;
                            s_liveness_passed = false;
                            tft_update_ui(TFT_COLOR_RED, "Phát hiện giả mạo!", recognized_name, "Vui lòng không sử dụng hình ảnh hoặc video");
                            ESP_LOGW(TAG, "SPOOFING DETECTED for %s! (Real score: %.2f)", recognized_name, real_score);
                            trigger_alarm();
                            trigger_snapshot_and_upload("spoof", s_recognized_person_id, "SPOOF");
                        }
                    } else {
                        current_vision_state = VISION_KNOWN_PERSON;
                        s_liveness_passed = true;
                        s_spoofing_detected = false;
                        s_stranger_detected = false;
                        
                        trigger_snapshot_and_upload("known", s_recognized_person_id, "KNOWN");
                        
                        char welcome_msg[128];
                        snprintf(welcome_msg, sizeof(welcome_msg), "Chào mừng %s đã về nhà", recognized_name);
                        tft_update_ui(TFT_COLOR_GREEN, "Xác nhận danh tính thành công:", "Người quen", welcome_msg);
                        
                        open_door();
                        ESP_LOGI(TAG, "🔓 DOOR UNLOCKED for %s! (Liveness Score: %.2f)", recognized_name, real_score);
                    }
                } else {
                    current_vision_state = VISION_KNOWN_PERSON;
                }
            } else {
                // NẾU LÀ NGƯỜI LẠ (STRANGER):
                current_vision_state = VISION_STRANGER;
                
                if (!s_stranger_detected) { // Dùng cờ này để không spam upload
                    s_stranger_detected = true;
                    s_spoofing_detected = false;
                    s_liveness_passed = false;
                    
                    tft_update_ui(TFT_COLOR_RED, "Xác định danh tính thất bại:", "Người lạ", "Bạn có thể nhấn giữ nút và nói chuyện với lễ tân ảo để được hỗ trợ");
                    ESP_LOGI(TAG, "STRANGER DETECTED. Uploading snapshot...");
                    
                    trigger_snapshot_and_upload("unknown", NULL, "STRANGER");
                }
            }
        }


        // Cooldown nhỏ để cho CPU hạ nhiệt
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

// =============================================================================
// HTTP HANDLERS (Chạy trên Core 0)
// =============================================================================

#include "web_ui.h"
#include "audio.h"

static esp_err_t test_audio_handler(httpd_req_t *req) {
    audio_test_record_toggle();
    httpd_resp_set_type(req, "application/json");
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"status\": \"success\", \"is_recording\": %d}", audio_test_is_recording());
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t logs_get_handler(httpd_req_t *req)
{
    FILE *f = fopen("/sdcard/db/logs.txt", "r");
    if (!f) {
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    
    // Đọc nội dung file
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc(fsize + 1);
    if (buf) {
        fread(buf, 1, fsize, f);
        buf[fsize] = '\0';
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, fsize);
        free(buf);
    } else {
        httpd_resp_sendstr(req, "[]");
    }
    fclose(f);
    return ESP_OK;
}

// API Xóa nhật ký (Clear logs)
static esp_err_t logs_clear_handler(httpd_req_t *req)
{
    unlink("/sdcard/db/logs.txt");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"Logs cleared\"}");
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

// =============================================================================
// DATASET CAPTURE APIs
// =============================================================================
static void mkdir_p(const char *path) {
    mkdir("/sdcard/dataset", 0777);
    mkdir(path, 0777);
}

static esp_err_t dataset_capture_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char query[128] = {0};
    char type[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "type", type, sizeof(type));
    }
    
    if (strcmp(type, "real") == 0) {
        mkdir_p("/sdcard/dataset/real");
        s_dataset_capture_count = 0;
        s_dataset_capture_mode = 1;
    } else if (strcmp(type, "fake") == 0) {
        mkdir_p("/sdcard/dataset/fake");
        s_dataset_capture_count = 0;
        s_dataset_capture_mode = 2;
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\"}");
    return ESP_OK;
}

static esp_err_t dataset_status_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"capturing\": %s, \"count\": %d, \"type\": \"%s\"}",
        (s_dataset_capture_mode > 0) ? "true" : "false",
        s_dataset_capture_count,
        (s_dataset_capture_mode == 1) ? "real" : (s_dataset_capture_mode == 2 ? "fake" : "none"));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static void send_tar_header(httpd_req_t *req, const char *filename, size_t filesize) {
    char header[512] = {0};
    strncpy(header, filename, 99);
    snprintf(header + 100, 8, "%07o", 0644); 
    snprintf(header + 108, 8, "%07o", 0);    
    snprintf(header + 116, 8, "%07o", 0);    
    snprintf(header + 124, 12, "%011o", filesize);
    snprintf(header + 136, 12, "%011o", 0);  
    memset(header + 148, ' ', 8);            
    header[156] = '0';                       
    snprintf(header + 257, 6, "ustar");      
    header[263] = '0'; header[264] = '0';    
    
    unsigned int chk = 0;
    for(int i=0; i<512; i++) chk += (unsigned char)header[i];
    snprintf(header + 148, 8, "%06o", chk);
    header[154] = 0; header[155] = ' ';
    
    httpd_resp_send_chunk(req, header, 512);
}

static void send_tar_file(httpd_req_t *req, const char *filepath, const char *tar_name) {
    struct stat st;
    if (stat(filepath, &st) != 0) return;
    
    FILE *f = fopen(filepath, "rb");
    if (!f) return;
    
    send_tar_header(req, tar_name, st.st_size);
    
    char buf[1024];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, read_bytes);
    }
    fclose(f);
    
    size_t pad_len = 512 - (st.st_size % 512);
    if (pad_len < 512) {
        memset(buf, 0, pad_len);
        httpd_resp_send_chunk(req, buf, pad_len);
    }
}

static esp_err_t dataset_download_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"dataset.tar\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_type(req, "application/x-tar");

    const char* folders[] = {"real", "fake"};
    for (int f_idx = 0; f_idx < 2; f_idx++) {
        char dir_path[64];
        snprintf(dir_path, sizeof(dir_path), "/sdcard/dataset/%s", folders[f_idx]);
        
        DIR *dir = opendir(dir_path);
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (ent->d_name[0] == '.') continue; // Skip '.' and '..'
                
                char full_path[512];
                snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);
                
                char tar_name[512];
                snprintf(tar_name, sizeof(tar_name), "%s/%s", folders[f_idx], ent->d_name);
                
                send_tar_file(req, full_path, tar_name);
                unlink(full_path); // Delete after sending
            }
            closedir(dir);
        }
    }
    
    // Tar end (two 512-byte blocks of zeros)
    char zeros[1024] = {0};
    httpd_resp_send_chunk(req, zeros, 1024);
    httpd_resp_send_chunk(req, NULL, 0); // Finish chunked response
    
    return ESP_OK;
}

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t mjpeg_stream_handler(httpd_req_t *req) {
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t * _jpg_buf = NULL;
    char part_buf[64];
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    size_t buf_len = 320 * 240 * 3;
    uint8_t *tmp_buf = (uint8_t *)malloc(buf_len);
    if (!tmp_buf) {
        ESP_LOGE(TAG, "Out of memory for MJPEG stream");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "MJPEG Stream Started");

    while (true) {
        if (!s_ai_enabled || ai_rgb888_buf == NULL) {
            vTaskDelay(pdMS_TO_TICKS(500)); // Delay if AI off
            continue;
        }

          // Swap BGR (từ esp-dl) sang RGB để nén JPEG đúng màu
          for (int i = 0; i < buf_len; i += 3) {
              tmp_buf[i] = ai_rgb888_buf[i + 2];     // R
              tmp_buf[i + 1] = ai_rgb888_buf[i + 1]; // G
              tmp_buf[i + 2] = ai_rgb888_buf[i];     // B
          }

        // Compress to JPEG (Quality 40 to save bandwidth and CPU)
        if (fmt2jpg(tmp_buf, buf_len, 320, 240, PIXFORMAT_RGB888, 40, &_jpg_buf, &_jpg_buf_len)) {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
            if (res == ESP_OK) {
                size_t hlen = snprintf(part_buf, 64, _STREAM_PART, _jpg_buf_len);
                res = httpd_resp_send_chunk(req, part_buf, hlen);
            }
            if (res == ESP_OK) {
                res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
            }
            free(_jpg_buf);
            _jpg_buf = NULL;
        }
        
        if (res != ESP_OK) {
            ESP_LOGI(TAG, "MJPEG Stream Stopped/Disconnected");
            break;
        }
        
        // Cố gắng đạt 15-20fps (nghỉ 30ms + thời gian nén ảnh)
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    
    free(tmp_buf);
    return res;
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

      // Swap BGR (từ esp-dl) sang RGB để nén JPEG đúng màu
      for (int i = 0; i < buf_len; i += 3) {
          tmp_buf[i] = ai_rgb888_buf[i + 2];     // R
          tmp_buf[i + 1] = ai_rgb888_buf[i + 1]; // G
          tmp_buf[i + 2] = ai_rgb888_buf[i];     // B
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
    } else if (current_vision_state == VISION_KNOWN_PERSON) {
        snprintf(resp, sizeof(resp), "KNOWN PERSON: %s | 🔓 DOOR UNLOCKED", recognized_name);
    } else if (current_vision_state == VISION_STRANGER) {
        if (s_spoofing_detected) {
            snprintf(resp, sizeof(resp), "SPOOFING DETECTED: %s", recognized_name);
        } else {
            snprintf(resp, sizeof(resp), "STRANGER DETECTED");
        }
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

static esp_err_t get_sonar_handler(httpd_req_t *req) {
    uint32_t dist = get_sonar_distance_cm();
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"distance\": %lu}", (unsigned long)dist);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

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
    config.xclk_freq_hz  = 10000000;      // 10MHz: Giảm xung nhịp để chống nhiễu cáp (sửa lỗi màn hình xanh lá)
    config.frame_size    = FRAMESIZE_QVGA;   // 320×240 — đủ để AI nhận diện khuôn mặt
    config.pixel_format  = PIXFORMAT_JPEG;   // OV2640 nén JPEG bằng phần cứng: frame nhỏ, màu chuẩn, FPS cao
    config.grab_mode     = CAMERA_GRAB_LATEST; // Luôn lấy khung hình mới nhất, bỏ khung cũ
    config.fb_location   = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality  = 10;              // 10 = chất lượng tốt (~15-25KB/frame, đủ cho AI)
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

    // Tạo AI Task trên Core 1 với stack 16KB trên PSRAM
    StaticTask_t *ai_task_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    StackType_t *ai_task_stack = (StackType_t *)heap_caps_malloc(AI_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    if (ai_task_tcb == NULL || ai_task_stack == NULL) {
        ESP_LOGE(TAG, "FATAL: Cannot allocate PSRAM for ai_task stack!");
        return false;
    }

    TaskHandle_t ret = xTaskCreateStaticPinnedToCore(
        ai_task, "ai_task", AI_TASK_STACK_SIZE, NULL, 5, ai_task_stack, ai_task_tcb, 1
    );
    if (ret == NULL) {
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
    cfg.stack_size         = 6144; // Giảm stack web server để cứu RAM cho Wi-Fi

    httpd_uri_t uris[] = {
        { .uri = "/",                   .method = HTTP_GET, .handler = index_handler,               .user_ctx = NULL },
        { .uri = "/api/test_audio",     .method = HTTP_GET, .handler = test_audio_handler,          .user_ctx = NULL },
        { .uri = "/enroll",             .method = HTTP_GET, .handler = enroll_handler,              .user_ctx = NULL },
        { .uri = "/ai_on",              .method = HTTP_GET, .handler = ai_on_handler,               .user_ctx = NULL },
        { .uri = "/ai_off",             .method = HTTP_GET, .handler = ai_off_handler,              .user_ctx = NULL },
        { .uri = "/detect_status",      .method = HTTP_GET, .handler = detect_status_handler,       .user_ctx = NULL },
        { .uri = "/snapshot.jpg",       .method = HTTP_GET, .handler = snapshot_handler,           .user_ctx = NULL },
        { .uri = "/api/persons",        .method = HTTP_GET, .handler = api_persons_get_handler,     .user_ctx = NULL },
        { .uri = "/api/persons/delete", .method = HTTP_GET, .handler = api_persons_delete_handler,  .user_ctx = NULL },
        { .uri = "/api/logs",           .method = HTTP_GET, .handler = api_logs_get_handler,        .user_ctx = NULL },
        { .uri = "/api/logs/clear",     .method = HTTP_GET, .handler = logs_clear_handler,          .user_ctx = NULL },
        { .uri = "/live_frame.jpg",     .method = HTTP_GET, .handler = live_frame_handler,          .user_ctx = NULL },
        { .uri = "/api/door_open",      .method = HTTP_GET, .handler = api_door_open_handler,       .user_ctx = NULL },
        { .uri = "/api/door_close",     .method = HTTP_GET, .handler = api_door_close_handler,      .user_ctx = NULL },
        { .uri = "/api/sonar",          .method = HTTP_GET, .handler = get_sonar_handler,           .user_ctx = NULL },
        { .uri = "/api/dataset/capture",.method = HTTP_GET, .handler = dataset_capture_handler,     .user_ctx = NULL },
        { .uri = "/api/dataset/status", .method = HTTP_GET, .handler = dataset_status_handler,      .user_ctx = NULL },
        { .uri = "/api/dataset/download",.method = HTTP_GET, .handler = dataset_download_handler,   .user_ctx = NULL },
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

    // Start a dedicated Stream HTTP Server on Port 81 so it doesn't block Port 80
    httpd_config_t stream_cfg = HTTPD_DEFAULT_CONFIG();
    stream_cfg.server_port = 81;
    stream_cfg.ctrl_port = 32769;
    stream_cfg.max_uri_handlers = 2;
    
    httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = mjpeg_stream_handler,
        .user_ctx  = NULL
    };
    
    if (httpd_start(&stream_httpd, &stream_cfg) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
        ESP_LOGI(TAG, "MJPEG Stream Server started on port 81.");
    }
}

// =============================================================================
// API CÔNG KHAI (Public API for main.cpp)
// =============================================================================

vision_state_t vision_process_frame(void) {
    return (vision_state_t)current_vision_state;
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
        ESP_LOGI(TAG, "[WEB] Xóa người quen thành công: ID = %s", person_id);
        
        char mqtt_payload[128];
        snprintf(mqtt_payload, sizeof(mqtt_payload), "{\"action\":\"delete_face\",\"status\":\"success\",\"id\":\"%s\"}", person_id);
        mqtt_publish_status(mqtt_payload);
    } else {
        ESP_LOGW(TAG, "[WEB] Xóa người quen thất bại: Không tìm thấy ID %s", person_id);
        
        char mqtt_payload[128];
        snprintf(mqtt_payload, sizeof(mqtt_payload), "{\"action\":\"delete_face\",\"status\":\"error\",\"message\":\"Không tìm thấy khuôn mặt %s\"}", person_id);
        mqtt_publish_status(mqtt_payload);
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
