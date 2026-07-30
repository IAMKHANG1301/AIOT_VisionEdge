#ifndef DB_MANAGER_H
#define DB_MANAGER_H

#include "cJSON.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the database (ensure directories exist)
esp_err_t db_manager_init(void);

// Person Management
cJSON* db_person_get_all_json(void);
esp_err_t db_person_add(const char* name, const float* embedding, const uint8_t* image_data, size_t image_size, char* out_id);
esp_err_t db_person_delete(const char* person_id);

// Log Management
cJSON* db_log_get_all_json(void);
esp_err_t db_log_add(const char* event, const char* person_id, const uint8_t* image_data, size_t image_size);

#ifdef __cplusplus
}
#endif

#endif // DB_MANAGER_H
