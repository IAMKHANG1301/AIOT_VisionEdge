#ifndef MQTT_SERVICE_H
#define MQTT_SERVICE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mqtt_open_door_cb_t)(void);
typedef void (*mqtt_take_snapshot_cb_t)(void);
typedef void (*mqtt_delete_face_cb_t)(const char* id);
typedef void (*mqtt_enroll_face_cb_t)(const char* name);

void mqtt_set_open_door_cb(mqtt_open_door_cb_t cb);
void mqtt_set_take_snapshot_cb(mqtt_take_snapshot_cb_t cb);
void mqtt_set_delete_face_cb(mqtt_delete_face_cb_t cb);
void mqtt_set_enroll_face_cb(mqtt_enroll_face_cb_t cb);

/**
 * @brief Khởi tạo và bắt đầu MQTT Client.
 * Kết nối tới broker.emqx.io và tự động subscribe vào topic kenit1301/aiot/commands.
 * @return esp_err_t ESP_OK nếu khởi tạo thành công.
 */
esp_err_t mqtt_app_start(void);

/**
 * @brief Gửi thông báo báo cáo trạng thái (vd: enroll_success) lên MQTT.
 * @param payload Chuỗi JSON chứa thông tin báo cáo.
 * @return esp_err_t ESP_OK nếu publish thành công.
 */
esp_err_t mqtt_publish_status(const char* payload);

#ifdef __cplusplus
}
#endif

#endif // MQTT_SERVICE_H
