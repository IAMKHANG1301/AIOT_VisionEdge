#ifndef VISION_H
#define VISION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_VECTOR_SIZE 128
#define THRESHOLD_DISTANCE 0.65f
#define THRESHOLD_SMILE_RATIO 1.15f

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
 * @brief Initialize the camera (OV2640) and vision models (ESP-WHO)
 */
void vision_init(void);

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

#ifdef __cplusplus
}
#endif

#endif // VISION_H
