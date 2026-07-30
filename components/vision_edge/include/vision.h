#ifndef VISION_H
#define VISION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "board_config.h"

#define FACE_VECTOR_SIZE 512
#define THRESHOLD_DISTANCE 0.60f
#define THRESHOLD_SMILE_RATIO 0.88f

typedef struct {
    float vector[FACE_VECTOR_SIZE];
} face_vector_t;

typedef enum {
    VISION_IDLE,
    VISION_FACE_DETECTED,
    VISION_KNOWN_PERSON,
    VISION_STRANGER,
    VISION_SPOOFING_DETECTED
} vision_state_t;

/**
 * @brief Initialize the camera peripheral
 */
bool vision_init(void);

/**
 * @brief Capture a frame, run face detection and recognition
 * @return current vision state based on recognition results
 */
vision_state_t vision_process_frame(void);

/**
 * @brief Add a face vector to the known database in memory
 * @param id The ID to assign to the person
 * @param vec Pointer to the 128D face vector
 * @return true if added successfully, false if database is full
 */
bool vision_add_known_face(int id, face_vector_t *vec);

/**
 * @brief Check if the current detected face is smiling (Liveness test)
 * @return true if smile detected, false otherwise
 */
bool vision_check_liveness(void);

/**
 * @brief Start a simple local Web Server for MJPEG streaming
 */
void vision_start_web_server(void);

/**
 * @brief Get the active HTTP server handle (returns httpd_handle_t cast to void*)
 */
void* vision_get_web_server_handle(void);

/**
 * @brief Tự động xóa khuôn mặt có ID được chỉ định khỏi bộ nhớ và thẻ SD
 * @param person_id ID của người cần xóa (vd: "P3")
 * @return true nếu xóa thành công
 */
bool vision_delete_enrolled_face(const char* person_id);

/**
 * @brief Kích hoạt chế độ Enroll khuôn mặt từ xa qua MQTT
 * @param name Tên của người cần đăng ký
 */
void vision_trigger_remote_enroll(const char* name);

#ifdef __cplusplus
}
#endif

#endif // VISION_H
