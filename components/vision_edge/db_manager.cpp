#include "db_manager.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_random.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern "C" {
#include "supabase_client.h"
#include "supabase_config.h"
}

static const char* TAG = "DB_MANAGER";

static SemaphoreHandle_t s_db_mutex = NULL;

#define DB_ROOT "/sdcard/db"
#define PERSONS_DIR "/sdcard/db/persons"
#define LOGS_DIR "/sdcard/db/logs"

// Sử dụng đuôi .txt (3 ký tự) để tuân thủ chuẩn FATFS 8.3, tránh lỗi nếu LFN bị tắt
#define PERSONS_JSON "/sdcard/db/persons.txt"
#define LOGS_JSON "/sdcard/db/logs.txt"

static void ensure_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        if (mkdir(path, 0777) != 0) {
            ESP_LOGE(TAG, "Failed to create directory %s", path);
        } else {
            ESP_LOGI(TAG, "Created directory %s", path);
        }
    }
}

esp_err_t db_manager_init(void) {
    ESP_LOGI(TAG, "Initializing Database...");
    
    if (s_db_mutex == NULL) {
        s_db_mutex = xSemaphoreCreateMutex();
    }
    
    ensure_dir(DB_ROOT);
    ensure_dir(PERSONS_DIR);
    ensure_dir(LOGS_DIR);
    
    // Ensure json files exist
    struct stat st;
    if (stat(PERSONS_JSON, &st) != 0) {
        FILE* f = fopen(PERSONS_JSON, "w");
        if (f) { fputs("[]", f); fclose(f); }
    }
    if (stat(LOGS_JSON, &st) != 0) {
        FILE* f = fopen(LOGS_JSON, "w");
        if (f) { fputs("[]", f); fclose(f); }
    }
    ESP_LOGI(TAG, "Database Initialized.");
    return ESP_OK;
}

static cJSON* read_json_array(const char* filepath) {
    FILE* f = fopen(filepath, "r");
    if (!f) return cJSON_CreateArray();
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) {
        fclose(f);
        return cJSON_CreateArray();
    }
    char* buf = (char*)malloc(fsize + 1);
    if (!buf) {
        fclose(f);
        return cJSON_CreateArray();
    }
    fread(buf, 1, fsize, f);
    buf[fsize] = 0;
    fclose(f);
    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) return cJSON_CreateArray();
    return root;
}

static void save_json_array(const char* filepath, cJSON* arr) {
    char* out = cJSON_PrintUnformatted(arr);
    if (out) {
        FILE* f = fopen(filepath, "w");
        if (!f) {
            ESP_LOGW(TAG, "save_json_array: fopen w failed, trying to unlink %s", filepath);
            unlink(filepath);
            f = fopen(filepath, "w");
        }
        if (f) {
            fputs(out, f);
            fclose(f);
            ESP_LOGI(TAG, "save_json_array: Wrote to %s successfully.", filepath);
        } else {
            ESP_LOGE(TAG, "save_json_array: Failed to open %s for writing even after unlink!", filepath);
        }
        free(out);
    }
}

cJSON* db_person_get_all_json(void) {
    cJSON* res = NULL;
    if (xSemaphoreTake(s_db_mutex, portMAX_DELAY)) {
        res = read_json_array(PERSONS_JSON);
        xSemaphoreGive(s_db_mutex);
    }
    return res;
}

typedef struct {
    char id[32];
    char name[64];
    char vec_path[128];
    uint8_t* image_data;
    size_t image_size;
} enroll_upload_args_t;

static void enroll_upload_task(void* pv) {
    enroll_upload_args_t* args = (enroll_upload_args_t*)pv;
    if (args->image_data && args->image_size > 0) {
        char filename[64];
        snprintf(filename, sizeof(filename), "%s.jpg", args->id);
        
        esp_err_t up_err = supabase_upload_image("known_faces", filename, args->image_data, args->image_size);
        if (up_err == ESP_OK) {
            char public_url[256];
            snprintf(public_url, sizeof(public_url), "%s/storage/v1/object/public/known_faces/%s", SUPABASE_URL, filename);
            
            supabase_insert_nguoiquen(args->id, args->name, args->vec_path, public_url);
        } else {
            ESP_LOGE(TAG, "Lỗi tải ảnh lên Supabase Storage (known_faces)");
        }
        free(args->image_data);
    } else {
        supabase_insert_nguoiquen(args->id, args->name, args->vec_path, "");
    }
    free(args);
    vTaskDelete(NULL);
}

typedef struct {
    long timestamp;
    char event[128];
    char person_id[32];
    uint8_t* image_data;
    size_t image_size;
} log_upload_args_t;

static void log_upload_task(void* pv) {
    log_upload_args_t* args = (log_upload_args_t*)pv;
    char public_url[256] = "";
    if (args->image_data && args->image_size > 0) {
        char filename[64];
        snprintf(filename, sizeof(filename), "L%ld.jpg", args->timestamp);
        
        esp_err_t up_err = supabase_upload_image("access_logs", filename, args->image_data, args->image_size);
        if (up_err == ESP_OK) {
            snprintf(public_url, sizeof(public_url), "%s/storage/v1/object/public/access_logs/%s", SUPABASE_URL, filename);
        } else {
            ESP_LOGE(TAG, "Lỗi tải ảnh lên Supabase Storage (access_logs)");
        }
        free(args->image_data);
    }
    supabase_insert_nhatkyravao(strlen(args->event) > 0 ? args->event : "UNKNOWN", args->person_id, public_url);
    free(args);
    vTaskDelete(NULL);
}

esp_err_t db_person_add(const char* name, const float* embedding, const uint8_t* image_data, size_t image_size, char* out_id) {
    if (!out_id || !embedding) return ESP_ERR_INVALID_ARG;

    // Generate ID: P + 5 digits
    uint32_t r = esp_random() % 100000;
    sprintf(out_id, "P%05lu", (unsigned long)r);

    // Save vector
    char vec_path[128];
    sprintf(vec_path, "%s/%s.bin", PERSONS_DIR, out_id);
    FILE* f = fopen(vec_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", vec_path);
        return ESP_FAIL;
    }
    // 512-dim ESP-DL vector (2048 bytes)
    fwrite(embedding, sizeof(float), 512, f);
    fclose(f);

    // No longer saving .jpg to save SD Card space
    char img_path[1] = "";

    // Update JSON
    if (xSemaphoreTake(s_db_mutex, portMAX_DELAY)) {
        cJSON* arr = read_json_array(PERSONS_JSON);
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", out_id);
        cJSON_AddStringToObject(obj, "name", name);
        cJSON_AddStringToObject(obj, "face_vector", vec_path);
        if (strlen(img_path) > 0) {
            cJSON_AddStringToObject(obj, "image", img_path);
        }
        cJSON_AddItemToArray(arr, obj);
        save_json_array(PERSONS_JSON, arr);
        cJSON_Delete(arr);
        xSemaphoreGive(s_db_mutex);
    }

    ESP_LOGI(TAG, "Added person %s (ID: %s) to DB", name, out_id);
    
    // Nâng cấp: Tải ảnh lên Supabase Storage trong background task để không block camera
    enroll_upload_args_t* args = (enroll_upload_args_t*)malloc(sizeof(enroll_upload_args_t));
    if (args) {
        strncpy(args->id, out_id, sizeof(args->id));
        strncpy(args->name, name, sizeof(args->name));
        strncpy(args->vec_path, vec_path, sizeof(args->vec_path));
        args->image_size = image_size;
        args->image_data = NULL;
        if (image_size > 0 && image_data) {
            args->image_data = (uint8_t*)malloc(image_size);
            if (args->image_data) {
                memcpy(args->image_data, image_data, image_size);
            }
        }
        xTaskCreate(enroll_upload_task, "enroll_upload", 10240, args, 5, NULL);
    }

    return ESP_OK;
}

esp_err_t db_person_delete(const char* id) {
    if (!id) return ESP_ERR_INVALID_ARG;
    
    char vec_path[128];
    sprintf(vec_path, "%s/%s.bin", PERSONS_DIR, id);
    unlink(vec_path);
    
    if (xSemaphoreTake(s_db_mutex, portMAX_DELAY)) {
        cJSON* arr = read_json_array(PERSONS_JSON);
        cJSON* new_arr = cJSON_CreateArray();
        int sz = cJSON_GetArraySize(arr);
        bool found = false;
        for (int i = 0; i < sz; i++) {
            cJSON* item = cJSON_GetArrayItem(arr, i);
            cJSON* pid = cJSON_GetObjectItem(item, "id");
            if (pid && strcmp(pid->valuestring, id) != 0) {
                // copy to new arr
                cJSON* cpy = cJSON_Duplicate(item, 1);
                cJSON_AddItemToArray(new_arr, cpy);
            } else {
                found = true;
            }
        }
        
        if (found) {
            save_json_array(PERSONS_JSON, new_arr);
            ESP_LOGI(TAG, "Deleted person %s from DB", id);
        } else {
            ESP_LOGW(TAG, "Person %s not found in DB JSON", id);
        }
        
        cJSON_Delete(arr);
        cJSON_Delete(new_arr);
        xSemaphoreGive(s_db_mutex);
    }
    
    // Xóa khỏi Supabase
    supabase_delete_nguoiquen(id);
    return ESP_OK;
}

cJSON* db_log_get_all_json(void) {
    return read_json_array(LOGS_JSON);
}

esp_err_t db_log_add(const char* event, const char* person_id, const uint8_t* image_data, size_t image_size) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long timestamp = tv.tv_sec;

    char img_path[128] = "";
    if (image_data && image_size > 0) {
        sprintf(img_path, "%s/L%ld.jpg", LOGS_DIR, timestamp);
        FILE* fi = fopen(img_path, "wb");
        if (fi) {
            fwrite(image_data, 1, image_size, fi);
            fclose(fi);
        }
    }

    cJSON* arr = read_json_array(LOGS_JSON);
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "timestamp", timestamp);
    if (event) cJSON_AddStringToObject(obj, "event", event);
    if (person_id) cJSON_AddStringToObject(obj, "person_id", person_id);
    if (strlen(img_path) > 0) {
        cJSON_AddStringToObject(obj, "image", img_path);
    }
    
    cJSON_AddItemToArray(arr, obj);

    // Keep only last 50 logs to prevent file from growing indefinitely
    while (cJSON_GetArraySize(arr) > 50) {
        cJSON_DeleteItemFromArray(arr, 0);
    }

    save_json_array(LOGS_JSON, arr);
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Added log event: %s", event ? event : "UNKNOWN");
    
    // Nâng cấp: Tải ảnh lên Supabase Storage trong background task để không block camera
    log_upload_args_t* args = (log_upload_args_t*)malloc(sizeof(log_upload_args_t));
    if (args) {
        args->timestamp = timestamp;
        if (event) strncpy(args->event, event, sizeof(args->event));
        else args->event[0] = '\0';
        if (person_id) strncpy(args->person_id, person_id, sizeof(args->person_id));
        else args->person_id[0] = '\0';
        
        args->image_size = image_size;
        args->image_data = NULL;
        if (image_size > 0 && image_data) {
            args->image_data = (uint8_t*)malloc(image_size);
            if (args->image_data) {
                memcpy(args->image_data, image_data, image_size);
            }
        }
        xTaskCreate(log_upload_task, "log_upload", 10240, args, 5, NULL);
    }

    return ESP_OK;
}
