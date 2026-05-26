#include "nvs_config.h"

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>
#include "mbedtls/base64.h"

#define TAG "nvs_config"
#define NVS_NAMESPACE_MAX_LEN 16

static char s_namespace[NVS_NAMESPACE_MAX_LEN] = {0};

esp_err_t nvs_config_init(const char *namespace)
{
    ESP_RETURN_ON_FALSE(namespace != NULL, ESP_ERR_INVALID_ARG, TAG, "namespace is NULL");
    ESP_RETURN_ON_FALSE(strlen(namespace) < NVS_NAMESPACE_MAX_LEN, ESP_ERR_INVALID_ARG, TAG,
                        "namespace too long (max %d chars)", NVS_NAMESPACE_MAX_LEN - 1);
    strlcpy(s_namespace, namespace, sizeof(s_namespace));
    ESP_LOGI(TAG, "initialized with namespace \"%s\"", s_namespace);
    return ESP_OK;
}

esp_err_t nvs_config_get(const char *key, char *buf, size_t buf_len)
{
    ESP_RETURN_ON_FALSE(s_namespace[0] != '\0', ESP_ERR_INVALID_STATE, TAG, "nvs_config_init() not called");
    ESP_RETURN_ON_FALSE(key != NULL && buf != NULL && buf_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(s_namespace, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        /* Namespace may not exist yet (first boot) — treat as not found */
        return ESP_ERR_NVS_NOT_FOUND;
    }

    ret = nvs_get_str(handle, key, buf, &buf_len);
    nvs_close(handle);
    return ret;
}

esp_err_t nvs_config_set(const char *key, const char *value)
{
    ESP_RETURN_ON_FALSE(s_namespace[0] != '\0', ESP_ERR_INVALID_STATE, TAG, "nvs_config_init() not called");
    ESP_RETURN_ON_FALSE(key != NULL && value != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(s_namespace, NVS_READWRITE, &handle), TAG, "failed to open namespace \"%s\"",
                        s_namespace);

    esp_err_t ret = nvs_set_str(handle, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to set key \"%s\": %s", key, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t nvs_config_erase_key(const char *key)
{
    ESP_RETURN_ON_FALSE(s_namespace[0] != '\0', ESP_ERR_INVALID_STATE, TAG, "nvs_config_init() not called");
    ESP_RETURN_ON_FALSE(key != NULL, ESP_ERR_INVALID_ARG, TAG, "key is NULL");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(s_namespace, NVS_READWRITE, &handle), TAG, "failed to open namespace \"%s\"",
                        s_namespace);

    esp_err_t ret = nvs_erase_key(handle, key);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

esp_err_t nvs_config_erase_all(void)
{
    ESP_RETURN_ON_FALSE(s_namespace[0] != '\0', ESP_ERR_INVALID_STATE, TAG, "nvs_config_init() not called");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(s_namespace, NVS_READWRITE, &handle), TAG, "failed to open namespace \"%s\"",
                        s_namespace);

    esp_err_t ret = nvs_erase_all(handle);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "namespace \"%s\" erased", s_namespace);
    }
    return ret;
}

/* Maximum raw blob size accepted for backup/restore */
#define NVS_BACKUP_BLOB_MAX (64 * 1024)
/* Maximum base64-encoded size for a max-size blob */
#define NVS_BACKUP_B64_MAX (88 * 1024)

esp_err_t nvs_config_serialize(char **json_out)
{
    esp_err_t ret = ESP_OK;
    nvs_iterator_t it = NULL;
    nvs_handle_t ns_handle = 0;
    bool ns_open = false;
    char cur_ns[16] = "";
    uint8_t *blob_buf = NULL;
    unsigned char *b64_buf = NULL;
    cJSON *root = NULL;
    cJSON *entries = NULL;

    ESP_RETURN_ON_FALSE(json_out != NULL, ESP_ERR_INVALID_ARG, TAG, "json_out is NULL");
    *json_out = NULL;

    root = cJSON_CreateObject();
    ESP_GOTO_ON_FALSE(root, ESP_ERR_NO_MEM, exit_no_root, TAG, "OOM building NVS backup root");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "partition", "nvs");
    entries = cJSON_AddObjectToObject(root, "entries");
    ESP_GOTO_ON_FALSE(entries, ESP_ERR_NO_MEM, exit, TAG, "OOM building NVS backup entries");

    blob_buf = malloc(NVS_BACKUP_BLOB_MAX);
    b64_buf = malloc(NVS_BACKUP_B64_MAX);
    if (!blob_buf || !b64_buf) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }

    esp_err_t it_ret = nvs_entry_find("nvs", NULL, NVS_TYPE_ANY, &it);
    while (it_ret == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        cJSON *ns_obj = cJSON_GetObjectItemCaseSensitive(entries, info.namespace_name);
        if (!ns_obj) {
            ns_obj = cJSON_AddObjectToObject(entries, info.namespace_name);
        }

        if (strcmp(cur_ns, info.namespace_name) != 0) {
            if (ns_open) {
                nvs_close(ns_handle);
                ns_open = false;
            }
            if (nvs_open(info.namespace_name, NVS_READONLY, &ns_handle) == ESP_OK) {
                ns_open = true;
                strlcpy(cur_ns, info.namespace_name, sizeof(cur_ns));
            } else {
                cur_ns[0] = '\0';
            }
        }

        if (ns_obj && ns_open) {
            cJSON *entry_obj = cJSON_CreateObject();
            bool entry_ok = false;

            switch (info.type) {
            case NVS_TYPE_U8: {
                uint8_t v = 0;
                if (nvs_get_u8(ns_handle, info.key, &v) == ESP_OK) {
                    cJSON_AddStringToObject(entry_obj, "type", "u8");
                    cJSON_AddNumberToObject(entry_obj, "value", (double)v);
                    entry_ok = true;
                }
                break;
            }
            case NVS_TYPE_I8: {
                int8_t v = 0;
                if (nvs_get_i8(ns_handle, info.key, &v) == ESP_OK) {
                    cJSON_AddStringToObject(entry_obj, "type", "i8");
                    cJSON_AddNumberToObject(entry_obj, "value", (double)v);
                    entry_ok = true;
                }
                break;
            }
            case NVS_TYPE_U16: {
                uint16_t v = 0;
                if (nvs_get_u16(ns_handle, info.key, &v) == ESP_OK) {
                    cJSON_AddStringToObject(entry_obj, "type", "u16");
                    cJSON_AddNumberToObject(entry_obj, "value", (double)v);
                    entry_ok = true;
                }
                break;
            }
            case NVS_TYPE_I16: {
                int16_t v = 0;
                if (nvs_get_i16(ns_handle, info.key, &v) == ESP_OK) {
                    cJSON_AddStringToObject(entry_obj, "type", "i16");
                    cJSON_AddNumberToObject(entry_obj, "value", (double)v);
                    entry_ok = true;
                }
                break;
            }
            case NVS_TYPE_U32: {
                uint32_t v = 0;
                if (nvs_get_u32(ns_handle, info.key, &v) == ESP_OK) {
                    cJSON_AddStringToObject(entry_obj, "type", "u32");
                    cJSON_AddNumberToObject(entry_obj, "value", (double)v);
                    entry_ok = true;
                }
                break;
            }
            case NVS_TYPE_I32: {
                int32_t v = 0;
                if (nvs_get_i32(ns_handle, info.key, &v) == ESP_OK) {
                    cJSON_AddStringToObject(entry_obj, "type", "i32");
                    cJSON_AddNumberToObject(entry_obj, "value", (double)v);
                    entry_ok = true;
                }
                break;
            }
            case NVS_TYPE_U64: {
                uint64_t v = 0;
                if (nvs_get_u64(ns_handle, info.key, &v) == ESP_OK) {
                    char num_str[24];
                    snprintf(num_str, sizeof(num_str), "%llu", (unsigned long long)v);
                    cJSON_AddStringToObject(entry_obj, "type", "u64");
                    cJSON_AddStringToObject(entry_obj, "value", num_str);
                    entry_ok = true;
                }
                break;
            }
            case NVS_TYPE_I64: {
                int64_t v = 0;
                if (nvs_get_i64(ns_handle, info.key, &v) == ESP_OK) {
                    char num_str[24];
                    snprintf(num_str, sizeof(num_str), "%lld", (long long)v);
                    cJSON_AddStringToObject(entry_obj, "type", "i64");
                    cJSON_AddStringToObject(entry_obj, "value", num_str);
                    entry_ok = true;
                }
                break;
            }
            case NVS_TYPE_STR: {
                size_t str_len = 0;
                if (nvs_get_str(ns_handle, info.key, NULL, &str_len) == ESP_OK && str_len > 0 &&
                    str_len <= NVS_BACKUP_BLOB_MAX) {
                    if (nvs_get_str(ns_handle, info.key, (char *)blob_buf, &str_len) == ESP_OK) {
                        cJSON_AddStringToObject(entry_obj, "type", "str");
                        cJSON_AddStringToObject(entry_obj, "value", (char *)blob_buf);
                        entry_ok = true;
                    }
                }
                break;
            }
            case NVS_TYPE_BLOB: {
                size_t blob_len = 0;
                if (nvs_get_blob(ns_handle, info.key, NULL, &blob_len) == ESP_OK && blob_len > 0 &&
                    blob_len <= NVS_BACKUP_BLOB_MAX) {
                    if (nvs_get_blob(ns_handle, info.key, blob_buf, &blob_len) == ESP_OK) {
                        size_t b64_len = 0;
                        int b64_ret = mbedtls_base64_encode(b64_buf, NVS_BACKUP_B64_MAX, &b64_len, blob_buf, blob_len);
                        if (b64_ret == 0 && b64_len > 0) {
                            b64_buf[b64_len] = '\0';
                            cJSON_AddStringToObject(entry_obj, "type", "blob");
                            cJSON_AddStringToObject(entry_obj, "value", (char *)b64_buf);
                            entry_ok = true;
                        }
                    }
                }
                break;
            }
            default:
                break;
            }

            if (entry_ok) {
                cJSON_AddItemToObject(ns_obj, info.key, entry_obj);
            } else {
                cJSON_Delete(entry_obj);
            }
        }

        it_ret = nvs_entry_next(&it);
    }

    if (ns_open) {
        nvs_close(ns_handle);
        ns_open = false;
    }
    nvs_release_iterator(it);

    *json_out = cJSON_PrintUnformatted(root);
    if (!*json_out) {
        ret = ESP_ERR_NO_MEM;
    }

exit:
    if (ns_open) {
        nvs_close(ns_handle);
    }
    free(blob_buf);
    free(b64_buf);
    cJSON_Delete(root);
    return ret;

exit_no_root:
    return ret;
}

esp_err_t nvs_config_deserialize(const char *json_str)
{
    esp_err_t ret = ESP_OK;
    cJSON *root = NULL;
    cJSON *entries = NULL;

    ESP_RETURN_ON_FALSE(json_str != NULL, ESP_ERR_INVALID_ARG, TAG, "json_str is NULL");

    root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse NVS backup JSON");
        return ESP_ERR_INVALID_ARG;
    }

    entries = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (!cJSON_IsObject(entries)) {
        ESP_LOGE(TAG, "NVS backup JSON missing \"entries\" object");
        ret = ESP_ERR_INVALID_ARG;
        goto exit;
    }

    cJSON *ns_item = NULL;
    cJSON_ArrayForEach(ns_item, entries)
    {
        const char *ns_name = ns_item->string;
        if (!ns_name || !cJSON_IsObject(ns_item)) {
            continue;
        }

        nvs_handle_t ns_handle = 0;
        if (nvs_open(ns_name, NVS_READWRITE, &ns_handle) != ESP_OK) {
            ESP_LOGW(TAG, "NVS restore: could not open namespace '%s', skipping", ns_name);
            continue;
        }

        cJSON *key_item = NULL;
        cJSON_ArrayForEach(key_item, ns_item)
        {
            const char *key = key_item->string;
            if (!key || !cJSON_IsObject(key_item)) {
                continue;
            }

            cJSON *type_j = cJSON_GetObjectItemCaseSensitive(key_item, "type");
            cJSON *value_j = cJSON_GetObjectItemCaseSensitive(key_item, "value");
            if (!cJSON_IsString(type_j) || !value_j) {
                continue;
            }

            const char *type = type_j->valuestring;
            esp_err_t set_ret = ESP_OK;

            if (strcmp(type, "u8") == 0 && cJSON_IsNumber(value_j)) {
                set_ret = nvs_set_u8(ns_handle, key, (uint8_t)value_j->valuedouble);
            } else if (strcmp(type, "i8") == 0 && cJSON_IsNumber(value_j)) {
                set_ret = nvs_set_i8(ns_handle, key, (int8_t)value_j->valuedouble);
            } else if (strcmp(type, "u16") == 0 && cJSON_IsNumber(value_j)) {
                set_ret = nvs_set_u16(ns_handle, key, (uint16_t)value_j->valuedouble);
            } else if (strcmp(type, "i16") == 0 && cJSON_IsNumber(value_j)) {
                set_ret = nvs_set_i16(ns_handle, key, (int16_t)value_j->valuedouble);
            } else if (strcmp(type, "u32") == 0 && cJSON_IsNumber(value_j)) {
                set_ret = nvs_set_u32(ns_handle, key, (uint32_t)value_j->valuedouble);
            } else if (strcmp(type, "i32") == 0 && cJSON_IsNumber(value_j)) {
                set_ret = nvs_set_i32(ns_handle, key, (int32_t)value_j->valuedouble);
            } else if (strcmp(type, "u64") == 0 && cJSON_IsString(value_j)) {
                set_ret = nvs_set_u64(ns_handle, key, (uint64_t)strtoull(value_j->valuestring, NULL, 10));
            } else if (strcmp(type, "i64") == 0 && cJSON_IsString(value_j)) {
                set_ret = nvs_set_i64(ns_handle, key, (int64_t)strtoll(value_j->valuestring, NULL, 10));
            } else if (strcmp(type, "str") == 0 && cJSON_IsString(value_j)) {
                set_ret = nvs_set_str(ns_handle, key, value_j->valuestring);
            } else if (strcmp(type, "blob") == 0 && cJSON_IsString(value_j)) {
                const char *b64 = value_j->valuestring;
                size_t b64_len = strlen(b64);
                size_t decoded_len = 0;
                /* Call with NULL output to get required size */
                mbedtls_base64_decode(NULL, 0, &decoded_len, (const unsigned char *)b64, b64_len);
                if (decoded_len > 0 && decoded_len <= NVS_BACKUP_BLOB_MAX) {
                    uint8_t *blob = malloc(decoded_len);
                    if (blob) {
                        int b64_ret =
                            mbedtls_base64_decode(blob, decoded_len, &decoded_len, (const unsigned char *)b64, b64_len);
                        if (b64_ret == 0) {
                            set_ret = nvs_set_blob(ns_handle, key, blob, decoded_len);
                        }
                        free(blob);
                    }
                }
            }

            if (set_ret != ESP_OK) {
                ESP_LOGW(TAG, "NVS restore: failed to set %s/%s: %s", ns_name, key, esp_err_to_name(set_ret));
            }
        }

        nvs_commit(ns_handle);
        nvs_close(ns_handle);
    }

exit:
    cJSON_Delete(root);
    return ret;
}
