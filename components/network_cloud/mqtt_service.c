#include "mqtt_service.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_mac.h"
#include <string.h>

static mqtt_open_door_cb_t s_open_door_cb = NULL;
static mqtt_delete_face_cb_t s_delete_face_cb = NULL;
static mqtt_enroll_face_cb_t s_enroll_face_cb = NULL;

void mqtt_set_open_door_cb(mqtt_open_door_cb_t cb) { s_open_door_cb = cb; }
void mqtt_set_delete_face_cb(mqtt_delete_face_cb_t cb) { s_delete_face_cb = cb; }
void mqtt_set_enroll_face_cb(mqtt_enroll_face_cb_t cb) { s_enroll_face_cb = cb; }

static const char *TAG = "MQTT_SVC";

#define MQTT_BROKER_URI "mqtt://broker.emqx.io:1883"
#define MQTT_TOPIC_CMD  "kenit1301/aiot/commands"
#define MQTT_TOPIC_STAT "kenit1301/aiot/status"

static esp_mqtt_client_handle_t s_mqtt_client = NULL;

/**
 * @brief Hàm xử lý dữ liệu JSON nhận được từ Topic Commands
 */
static void handle_mqtt_command(const char* payload, int len) {
    // payload có thể không có null-terminated
    char* json_str = malloc(len + 1);
    if (!json_str) return;
    memcpy(json_str, payload, len);
    json_str[len] = '\0';

    ESP_LOGI(TAG, "Nhận lệnh MQTT: %s", json_str);

    cJSON *root = cJSON_Parse(json_str);
    if (root) {
        cJSON *action = cJSON_GetObjectItem(root, "action");
        if (action && action->valuestring) {
            
            if (strcmp(action->valuestring, "open_door") == 0) {
                ESP_LOGI(TAG, "Lệnh Mở Cửa (MQTT)");
                if (s_open_door_cb) s_open_door_cb();
            } 
            else if (strcmp(action->valuestring, "delete_face") == 0) {
                cJSON *id_item = cJSON_GetObjectItem(root, "id");
                if (id_item && id_item->valuestring) {
                    ESP_LOGI(TAG, "Lệnh Xóa Khuôn Mặt: %s", id_item->valuestring);
                    if (s_delete_face_cb) s_delete_face_cb(id_item->valuestring);
                } else {
                    ESP_LOGW(TAG, "Lệnh delete_face thiếu tham số 'id'");
                }
            }
            else if (strcmp(action->valuestring, "enroll_face") == 0) {
                cJSON *name_item = cJSON_GetObjectItem(root, "name");
                if (name_item && name_item->valuestring) {
                    ESP_LOGI(TAG, "Lệnh Enroll Remote: %s", name_item->valuestring);
                    if (s_enroll_face_cb) s_enroll_face_cb(name_item->valuestring);
                } else {
                    ESP_LOGW(TAG, "Lệnh enroll_face thiếu tham số 'name'");
                }
            }
            else {
                ESP_LOGW(TAG, "Action MQTT không hợp lệ: %s", action->valuestring);
            }
        }
        cJSON_Delete(root);
    } else {
        ESP_LOGE(TAG, "Lỗi Parse JSON từ MQTT");
    }

    free(json_str);
}

/**
 * @brief MQTT Event Handler
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Đã kết nối thành công tới %s", MQTT_BROKER_URI);
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_CMD, 0);
            ESP_LOGI(TAG, "Đã Subscribe Topic: %s", MQTT_TOPIC_CMD);
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Mất kết nối MQTT, đang tự động kết nối lại...");
            break;
            
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "Subscribe thành công, msg_id=%d", event->msg_id);
            break;
            
        case MQTT_EVENT_DATA:
            if (event->topic_len == strlen(MQTT_TOPIC_CMD) && 
                strncmp(event->topic, MQTT_TOPIC_CMD, event->topic_len) == 0) {
                handle_mqtt_command(event->data, event->data_len);
            }
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "Lỗi MQTT!");
            break;
            
        default:
            break;
    }
}

esp_err_t mqtt_app_start(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char client_id[32];
    snprintf(client_id, sizeof(client_id), "ESP32_AIOT_%02x%02x%02x", mac[3], mac[4], mac[5]);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = client_id,
        // Dùng keepalive 60 giây (Mặc định thường là 120)
        .session.keepalive = 60,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "Khởi tạo MQTT Client thất bại");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);

    return ESP_OK;
}

esp_err_t mqtt_publish_status(const char* payload) {
    if (!s_mqtt_client) return ESP_FAIL;
    
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_STAT, payload, 0, 1, 0);
    if (msg_id >= 0) {
        ESP_LOGI(TAG, "Đã Publish Status [%s]: %s", MQTT_TOPIC_STAT, payload);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Lỗi Publish Status");
        return ESP_FAIL;
    }
}
