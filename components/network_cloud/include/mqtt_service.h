#ifndef MQTT_SERVICE_H
#define MQTT_SERVICE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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
