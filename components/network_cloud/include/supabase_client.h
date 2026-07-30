#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t supabase_upload_image(const char* bucket, const char* filename, const uint8_t* image_data, size_t image_size);
esp_err_t supabase_insert_nguoiquen(const char* id, const char* name, const char* facevector, const char* imagepath);
esp_err_t supabase_insert_nhatkyravao(const char* eventtype, const char* personid, const char* capturedimage);
esp_err_t supabase_delete_nguoiquen(const char* person_id);

// --- Remote Command Polling ---
esp_err_t supabase_get_pending_command(char* out_id, size_t id_len, char* out_cmd, size_t cmd_len);
esp_err_t supabase_update_command_status(const char* id, const char* status);


#ifdef __cplusplus
}
#endif

