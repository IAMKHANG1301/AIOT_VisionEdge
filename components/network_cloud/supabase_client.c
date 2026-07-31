#include "supabase_client.h"
#include "supabase_config.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"
static const char *TAG = "SUPABASE";

static esp_err_t supabase_http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // If it's a 4xx or 5xx error, print the body
                int status_code = esp_http_client_get_status_code(evt->client);
                if (status_code >= 400) {
                    ESP_LOGE(TAG, "Supabase Error Body: %.*s", evt->data_len, (char*)evt->data);
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

esp_err_t supabase_upload_image(const char* bucket, const char* filename, const uint8_t* image_data, size_t image_size) {
    if (!image_data || image_size == 0) return ESP_ERR_INVALID_ARG;
    
    char url[256];
    snprintf(url, sizeof(url), "%s/storage/v1/object/%s/%s", SUPABASE_URL, bucket, filename);
    
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .buffer_size = 8192,
        .buffer_size_tx = 4096,
        .event_handler = supabase_http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for storage");
        return ESP_FAIL;
    }
    
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", SUPABASE_ANON_KEY);
    
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "apikey", SUPABASE_ANON_KEY);
    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    
    esp_http_client_set_post_field(client, (const char*)image_data, image_size);
    
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200 || status_code == 201) {
            ESP_LOGI(TAG, "Upload image SUCCESS! URL: %s/storage/v1/object/public/%s/%s", SUPABASE_URL, bucket, filename);
        } else {
            ESP_LOGE(TAG, "Upload image FAILED! Status = %d", status_code);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t supabase_post_json(const char* table, const char* json_data) {
    char url[256];
    snprintf(url, sizeof(url), "%s/rest/v1/%s", SUPABASE_URL, table);
    
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .buffer_size = 8192,
        .crt_bundle_attach = esp_crt_bundle_attach
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;
    
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", SUPABASE_ANON_KEY);
    
    esp_http_client_set_header(client, "apikey", SUPABASE_ANON_KEY);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Prefer", "return=minimal");
    
    esp_http_client_set_post_field(client, json_data, strlen(json_data));
    
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 201 || status_code == 200 || status_code == 204) {
            ESP_LOGI(TAG, "Insert %s SUCCESS", table);
        } else {
            ESP_LOGE(TAG, "Insert %s FAILED, Status = %d", table, status_code);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t supabase_insert_nguoiquen(const char* id, const char* name, const char* facevector, const char* imagepath) {
    char json[512];
    snprintf(json, sizeof(json), "{\"id\":\"%s\",\"name\":\"%s\",\"facevector\":\"%s\",\"imagepath\":\"%s\"}", id, name, facevector, imagepath);
    return supabase_post_json("nguoiquen", json);
}

esp_err_t supabase_insert_nhatkyravao(const char* eventtype, const char* personid, const char* capturedimage) {
    char json[512];
    if (personid && strlen(personid) > 0 && strcmp(personid, "NULL") != 0) {
        snprintf(json, sizeof(json), "{\"eventtype\":\"%s\",\"personid\":\"%s\",\"capturedimage\":\"%s\"}", eventtype, personid, capturedimage);
    } else {
        snprintf(json, sizeof(json), "{\"eventtype\":\"%s\",\"capturedimage\":\"%s\"}", eventtype, capturedimage);
    }
    return supabase_post_json("nhatkyravao", json);
}

esp_err_t supabase_delete_nguoiquen(const char* person_id) {
    char url[256];
    snprintf(url, sizeof(url), "%s/rest/v1/nguoiquen?personid=eq.%s", SUPABASE_URL, person_id);
    
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_DELETE,
        .timeout_ms = 10000,
        .buffer_size = 8192,
        .crt_bundle_attach = esp_crt_bundle_attach
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;
    
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", SUPABASE_ANON_KEY);
    
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "apikey", SUPABASE_ANON_KEY);
    
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    return err;
}

// =============================================================================
// REMOTE COMMAND POLLING (dieukhienthietbi)
// =============================================================================

static char s_get_resp_buf[2048];
static int s_get_resp_len = 0;

static esp_err_t supabase_get_http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                int copy_len = evt->data_len;
                if (s_get_resp_len + copy_len < sizeof(s_get_resp_buf) - 1) {
                    memcpy(s_get_resp_buf + s_get_resp_len, evt->data, copy_len);
                    s_get_resp_len += copy_len;
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

esp_err_t supabase_get_pending_command(char* out_id, size_t id_len, char* out_cmd, size_t cmd_len) {
    s_get_resp_len = 0;
    memset(s_get_resp_buf, 0, sizeof(s_get_resp_buf));
    
    char url[256];
    snprintf(url, sizeof(url), "%s/rest/v1/dieukhienthietbi?status=eq.pending&select=id,command&order=created_at.asc&limit=1", SUPABASE_URL);
    
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 5000,
        .buffer_size = 8192,
        .event_handler = supabase_get_http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;
    
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", SUPABASE_ANON_KEY);
    
    esp_http_client_set_header(client, "apikey", SUPABASE_ANON_KEY);
    esp_http_client_set_header(client, "Authorization", auth_header);
    
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200) {
            s_get_resp_buf[s_get_resp_len] = '\0';
            cJSON *root = cJSON_Parse(s_get_resp_buf);
            if (root) {
                if (cJSON_IsArray(root) && cJSON_GetArraySize(root) > 0) {
                    cJSON *item = cJSON_GetArrayItem(root, 0);
                    cJSON *cid = cJSON_GetObjectItem(item, "id");
                    cJSON *ccmd = cJSON_GetObjectItem(item, "command");
                    
                    if (cid && ccmd && ccmd->valuestring) {
                        if (cJSON_IsNumber(cid)) {
                            snprintf(out_id, id_len, "%lld", (long long)cid->valuedouble);
                        } else if (cJSON_IsString(cid)) {
                            snprintf(out_id, id_len, "%s", cid->valuestring);
                        }
                        snprintf(out_cmd, cmd_len, "%s", ccmd->valuestring);
                    } else {
                        err = ESP_ERR_NOT_FOUND;
                    }
                } else {
                    err = ESP_ERR_NOT_FOUND;
                }
                cJSON_Delete(root);
            } else {
                err = ESP_FAIL;
            }
        } else {
            err = ESP_FAIL;
        }
    }
    
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t supabase_update_command_status(const char* id, const char* status) {
    char url[256];
    snprintf(url, sizeof(url), "%s/rest/v1/dieukhienthietbi?id=eq.%s", SUPABASE_URL, id);
    
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_PATCH,
        .timeout_ms = 5000,
        .buffer_size = 8192,
        .event_handler = supabase_get_http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;
    
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", SUPABASE_ANON_KEY);
    
    esp_http_client_set_header(client, "apikey", SUPABASE_ANON_KEY);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    
    char json[128];
    snprintf(json, sizeof(json), "{\"status\":\"%s\"}", status);
    
    esp_http_client_set_post_field(client, json, strlen(json));
    
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code >= 200 && status_code < 300) {
            ESP_LOGI(TAG, "Command %s updated to %s", id, status);
        } else {
            err = ESP_FAIL;
        }
    }
    
    esp_http_client_cleanup(client);
    return err;
}
