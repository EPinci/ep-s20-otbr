#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Well-known key constants */
#define NVS_CONFIG_KEY_HOSTNAME "hostname"
#define NVS_CONFIG_KEY_WIFI_SSID "wifi_ssid"
#define NVS_CONFIG_KEY_WIFI_PASS "wifi_pass"
#define NVS_CONFIG_KEY_TH_TXPWR "th_txpwr"
#define NVS_CONFIG_KEY_TH_LDR_WT "th_ldr_wt"
#define NVS_CONFIG_KEY_TH_TREL "th_trel"

/**
 * @brief Initialize the NVS config component with a given namespace.
 *
 * Must be called after nvs_flash_init(). Stores the namespace name
 * internally; all subsequent get/set/erase calls operate on it.
 *
 * @param[in] namespace  NVS namespace string (max 15 characters).
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t nvs_config_init(const char *namespace);

/**
 * @brief Retrieve a string value from NVS config.
 *
 * @param[in]  key      NVS key string.
 * @param[out] buf      Buffer to receive the null-terminated value.
 * @param[in]  buf_len  Size of buf in bytes.
 * @return
 *   - ESP_OK              on success
 *   - ESP_ERR_NVS_NOT_FOUND  when the key has no stored value
 *   - ESP_ERR_INVALID_STATE  when nvs_config_init() has not been called
 *   - other esp_err_t codes  on NVS errors
 */
esp_err_t nvs_config_get(const char *key, char *buf, size_t buf_len);

/**
 * @brief Store a string value in NVS config.
 *
 * Opens the namespace, writes the value, commits, then closes.
 *
 * @param[in] key    NVS key string.
 * @param[in] value  Null-terminated string to store.
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t nvs_config_set(const char *key, const char *value);

/**
 * @brief Erase a single key from NVS config.
 *
 * @param[in] key  NVS key string to erase.
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t nvs_config_erase_key(const char *key);

/**
 * @brief Erase all keys in the NVS config namespace.
 *
 * Only erases the namespace set during nvs_config_init().
 * Does NOT call nvs_flash_erase() — OpenThread and other namespaces
 * are not affected.
 *
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t nvs_config_erase_all(void);

/**
 * @brief Serialize the entire "nvs" partition to a JSON string.
 *
 * Iterates all namespaces and keys in the NVS partition and encodes them
 * into a structured JSON object. Blobs are base64-encoded.
 *
 * Format:
 * @code
 * {
 *   "version": 1,
 *   "partition": "nvs",
 *   "entries": {
 *     "<namespace>": { "<key>": { "type": "<type>", "value": <value> }, ... },
 *     ...
 *   }
 * }
 * @endcode
 *
 * The returned string is allocated with malloc() and must be freed by the
 * caller using free().
 *
 * @param[out] json_out  Receives a pointer to the allocated JSON string.
 *                       Set to NULL on error.
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t nvs_config_serialize(char **json_out);

/**
 * @brief Restore NVS entries from a JSON string produced by nvs_config_serialize().
 *
 * Parses the JSON, iterates all namespaces and keys, and writes each value
 * back to NVS. Existing keys are overwritten; unrecognised type strings are
 * silently skipped.
 *
 * @param[in] json_str  Null-terminated JSON string to restore from.
 * @return
 *   - ESP_OK              on success
 *   - ESP_ERR_INVALID_ARG if json_str is NULL or the JSON structure is invalid
 *   - other esp_err_t codes on NVS errors
 */
esp_err_t nvs_config_deserialize(const char *json_str);

#ifdef __cplusplus
}
#endif
