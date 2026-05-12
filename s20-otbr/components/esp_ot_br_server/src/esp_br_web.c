#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sdkconfig.h"

#include "cJSON.h"
#include "esp_br_http_ota.h"
#include "esp_br_web.h"
#include "esp_br_web_api.h"
#include "esp_br_web_base.h"
#if CONFIG_OPENTHREAD_BR_SOFTAP_SETUP
#include "esp_br_wifi_config.h"
#endif
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_ot_ota_commands.h"
#include "esp_ota_ops.h"
#include "esp_rcp_firmware.h"
#include "esp_rcp_ota.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_vfs.h"
#include "http_parser.h"
#include "protocol_examples_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/base64.h"

#include "openthread/dataset.h"
#include "openthread/error.h"
#include "openthread/ip6.h"
#include "openthread/platform/radio.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"

#define MAX_FILE_SIZE (200 * 1024) // 200 KB
#define MAX_FILE_SIZE_STR "200KB"
#define SCRATCH_BUFSIZE 1024 /* Scratch buffer size */
#define VFS_PATH_MAXNUM 15
#define SERVER_IPV4_LEN 16
#define FILE_CHUNK_SIZE 4096
#define LOG_BUFFER_SIZE (16 * 1024)
#define LOG_QUERY_VALUE_LEN 32
#define LOG_SSE_POLL_MS 250
#define LOG_SSE_KEEPALIVE_MS 3000
#define OTA_TASK_STACK_SIZE 6144
#define OTA_TASK_PRIORITY 5
#define OTA_RESTART_TASK_STACK_SIZE 2048
#define OTA_RESTART_DELAY_MS 200

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
#define OTA_URL_MAX_LEN 512
#define WEB_TAG "obtr_web"

/*-----------------------------------------------------
 Note：Http Server
-----------------------------------------------------*/
/**
 * @brief The basic configuration for http_server
 */
typedef struct http_server_data {
    char base_path[ESP_VFS_PATH_MAX + 1]; /* the storaged file path */
    char scratch[SCRATCH_BUFSIZE];        /* scratch buffer for temporary storage during file transfer */
} http_server_data_t;

/**
 * @brief The basic information for http_server
 */
typedef struct http_server {
    httpd_handle_t handle;    /* server handle, unique */
    http_server_data_t data;  /* data */
    char ip[SERVER_IPV4_LEN]; /* ip */
    uint16_t port;            /* port */
} http_server_t;

static http_server_t s_server = {NULL, {"", ""}, "", 80}; /* the instance of server */

static portMUX_TYPE s_log_buffer_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_ota_mutex;
static bool s_ota_in_progress;
static bool s_log_hook_installed;
static uint64_t s_log_write_cursor;
static char s_log_buffer[LOG_BUFFER_SIZE];
static vprintf_like_t s_log_vprintf;

typedef struct ota_request_context {
    char *url;
} ota_request_context_t;

/**
 * @brief The basic parameter definition for parsing url
 */
#define PROTLOCOL_MAX_SIZE 12
#define FILENAME_MAX_SIZE 64
#define FILEPATH_MAX_SIZE (FILENAME_MAX_SIZE + ESP_VFS_PATH_MAX)
typedef struct request_url {
    char protocol[PROTLOCOL_MAX_SIZE];
    uint16_t port;
    char file_name[FILENAME_MAX_SIZE];
    char file_path[FILEPATH_MAX_SIZE];
} request_url_t;

/*-----------------------------------------------------
 Note：Http Server Thread REST API
-----------------------------------------------------*/
static esp_err_t esp_otbr_network_diagnostics_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_node_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_node_delete_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_node_rloc_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_node_rloc16_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_node_state_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_node_state_put_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_node_extaddress_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_node_network_name_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_node_leader_data_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_node_number_of_router_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_node_extpanid_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_node_baid_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_node_dataset_active_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_node_dataset_pending_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_node_dataset_handler(httpd_req_t *req, const char *dataset_type);

static httpd_uri_t s_resource_handlers[] = {
    {
        .uri = ESP_OT_REST_API_DIAGNOSTICS_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_network_diagnostics_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_NODE_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_network_node_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_NODE_PATH,
        .method = HTTP_DELETE,
        .handler = esp_otbr_network_node_delete_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_NODE_RLOC_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_network_node_rloc_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_NODE_RLOC16_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_network_node_rloc16_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_NODE_STATE_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_network_node_state_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_NODE_STATE_PATH,
        .method = HTTP_PUT,
        .handler = esp_otbr_network_node_state_put_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_NODE_EXTADDRESS_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_network_node_extaddress_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_NODE_NETWORKNAME_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_network_node_network_name_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_NODE_LEADERDATA_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_network_node_leader_data_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_NODE_NUMBEROFROUTER_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_network_node_number_of_router_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_NODE_EXTPANID_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_network_node_extpanid_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_NODE_BORDERAGENTID_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_network_node_baid_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_NODE_DATASET_ACTIVE_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_network_node_dataset_active_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_NODE_DATASET_ACTIVE_PATH,
        .method = HTTP_PUT,
        .handler = esp_otbr_network_node_dataset_active_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_NODE_DATASET_PENDING_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_network_node_dataset_pending_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_NODE_DATASET_PENDING_PATH,
        .method = HTTP_PUT,
        .handler = esp_otbr_network_node_dataset_pending_handler,
        .user_ctx = &s_server.data,
    },
};

/*-----------------------------------------------------
 Note：Http Server WEB-GUI API
-----------------------------------------------------*/
static esp_err_t esp_otbr_network_properties_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_available_networks_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_join_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_form_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_add_network_prefix_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_delete_network_prefix_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_commission_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_network_topology_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_current_node_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_logs_stream_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_ping_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_ota_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_ota_upload_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_ota_upload_app_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_ota_upload_rcp_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_ota_upload_web_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_ota_restart_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_restart_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_factory_reset_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_nvs_backup_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_nvs_restore_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_ipaddr_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_add_ipaddr_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_delete_ipaddr_post_handler(httpd_req_t *req);
static esp_err_t esp_otbr_leader_weight_get_handler(httpd_req_t *req);
static esp_err_t esp_otbr_leader_weight_put_handler(httpd_req_t *req);
static esp_err_t esp_otbr_become_leader_post_handler(httpd_req_t *req);

static httpd_uri_t s_web_gui_handlers[] = {
    {
        .uri = ESP_OT_REST_API_PROPERTIES_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_network_properties_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_AVAILABLE_NETWORK_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_available_networks_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_JOIN_NETWORK_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_network_join_post_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_FORM_NETWORK_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_network_form_post_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_ADD_NETWORK_PREFIX_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_add_network_prefix_post_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_DELETE_NETWORK_PREFIX_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_delete_network_prefix_post_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_COMMISSION_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_network_commission_post_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_TOPOLOGY_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_network_topology_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_NODE_INFORMATION_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_current_node_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_LOGS_STREAM_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_logs_stream_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_PING_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_ping_post_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_OTA_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_ota_post_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_OTA_UPLOAD_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_ota_upload_post_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_OTA_UPLOAD_APP_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_ota_upload_app_post_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_OTA_UPLOAD_RCP_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_ota_upload_rcp_post_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_OTA_UPLOAD_WEB_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_ota_upload_web_post_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_OTA_RESTART_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_ota_restart_post_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_IPADDR_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_ipaddr_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_ADD_IPADDR_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_add_ipaddr_post_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_DELETE_IPADDR_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_delete_ipaddr_post_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_LEADER_WEIGHT_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_leader_weight_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_LEADER_WEIGHT_PATH,
        .method = HTTP_PUT,
        .handler = esp_otbr_leader_weight_put_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_BECOME_LEADER_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_become_leader_post_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_RESTART_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_restart_post_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_FACTORY_RESET_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_factory_reset_post_handler,
        .user_ctx = &s_server.data,
    },
    {
        .uri = ESP_OT_REST_API_NVS_BACKUP_PATH,
        .method = HTTP_GET,
        .handler = esp_otbr_nvs_backup_get_handler,
        .user_ctx = NULL,
    },
    {
        .uri = ESP_OT_REST_API_NVS_RESTORE_PATH,
        .method = HTTP_POST,
        .handler = esp_otbr_nvs_restore_post_handler,
        .user_ctx = NULL,
    },
};

/*-----------------------------------------------------
 Note：Http Tools
-----------------------------------------------------*/
static cJSON *pack_response(cJSON *error, cJSON *result, cJSON *message)
{
    if (!error || !result || !message) {
        if (error) {
            cJSON_Delete(error);
        }
        if (result) {
            cJSON_Delete(result);
        }
        if (message) {
            cJSON_Delete(message);
        }
        ESP_LOGE(WEB_TAG, "Failed to pack response json");
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "error", error);
    cJSON_AddItemToObject(root, "result", result);
    cJSON_AddItemToObject(root, "message", message);
    return root;
}

static cJSON *resource_status(char *error, char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "ErrorCode", cJSON_CreateString(error));
    cJSON_AddItemToObject(root, "ErrorMessage", cJSON_CreateString(msg));
    return root;
}

static esp_err_t httpd_server_register_http_uri(const http_server_t *server, httpd_uri_t *uris, uint8_t size)
{
    ESP_RETURN_ON_FALSE((server->handle && uris), ESP_ERR_INVALID_ARG, WEB_TAG, "Invalid argument");
    for (int i = 0; i < size; i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server->handle, &uris[i]), WEB_TAG,
                            "Failed to register %s for %d", uris[i].uri, i);
    }
    return ESP_OK;
}

static cJSON *httpd_request_convert2_json(httpd_req_t *req, int type)
{
    char *buf = ((http_server_data_t *)(req->user_ctx))->scratch;
    int received = 0;
    int cur_len = 0;
    int total_len = req->content_len;
    int end_len = total_len;
    if (type == cJSON_String) {
        cur_len = 1;
        total_len += 2;
        buf[0] = '\"';
        buf[total_len - 1] = '\"';
        end_len = total_len - 1;
    }

    if (total_len >= SCRATCH_BUFSIZE) /* Respond with 500 Internal Server Error */
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "The content of packet is too long");
        return NULL;
    }
    while (cur_len < end_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) /* Respond with 500 Internal Server Error */
        {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal Server Error[500]");
            return NULL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    return cJSON_Parse(buf);
}

static esp_err_t httpd_send_packet(httpd_req_t *req, cJSON *root)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(root, ESP_FAIL, WEB_TAG, "Invalid Argument");
    char *packet = cJSON_PrintUnformatted(root);
    ESP_RETURN_ON_FALSE(packet, ESP_FAIL, WEB_TAG, "Invalid Packet");
    ESP_LOGD(WEB_TAG, "Properties: %s\r\n", packet);
    ESP_RETURN_ON_FALSE(packet, ESP_FAIL, WEB_TAG, "Invalid Pesponse");
    ESP_GOTO_ON_ERROR(httpd_resp_set_type(req, ESP_OT_REST_CONTENT_TYPE_JSON), exit, WEB_TAG,
                      "Failed to set http type");
    ESP_GOTO_ON_ERROR(httpd_resp_sendstr(req, packet), exit, WEB_TAG, "Failed to send http respond");
exit:
    cJSON_free(packet);
    return ret;
}

static esp_err_t httpd_send_plain_text(httpd_req_t *req, char *str)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(str, ESP_FAIL, WEB_TAG, "Invalid plain text");
    ESP_LOGD(WEB_TAG, "Properties: %s\r\n", str);
    ESP_GOTO_ON_ERROR(httpd_resp_set_type(req, ESP_OT_REST_CONTENT_TYPE_PLAIN), exit, WEB_TAG,
                      "Failed to set http type");
    ESP_GOTO_ON_ERROR(httpd_resp_sendstr(req, str), exit, WEB_TAG, "Failed to send http respond");
exit:
    return ret;
}

static void log_buffer_write(const char *text, size_t len)
{
    if (!text || len == 0) {
        return;
    }

    taskENTER_CRITICAL(&s_log_buffer_lock);
    for (size_t index = 0; index < len; ++index) {
        s_log_buffer[s_log_write_cursor % LOG_BUFFER_SIZE] = text[index];
        ++s_log_write_cursor;
    }
    taskEXIT_CRITICAL(&s_log_buffer_lock);
}

static int web_log_vprintf(const char *fmt, va_list args)
{
    int ret = 0;
    va_list print_args;
    va_list size_args;

    va_copy(print_args, args);
    if (s_log_vprintf) {
        ret = s_log_vprintf(fmt, print_args);
    }
    va_end(print_args);

    va_copy(size_args, args);
    int needed = vsnprintf(NULL, 0, fmt, size_args);
    va_end(size_args);

    if (needed <= 0) {
        return ret;
    }

    if (needed < 256) {
        char stack_buf[256];
        va_list write_args;

        va_copy(write_args, args);
        vsnprintf(stack_buf, sizeof(stack_buf), fmt, write_args);
        va_end(write_args);
        log_buffer_write(stack_buf, (size_t)needed);
        return ret;
    }

    char *heap_buf = malloc((size_t)needed + 1);
    if (!heap_buf) {
        return ret;
    }

    va_list write_args;
    va_copy(write_args, args);
    vsnprintf(heap_buf, (size_t)needed + 1, fmt, write_args);
    va_end(write_args);
    log_buffer_write(heap_buf, (size_t)needed);
    free(heap_buf);
    return ret;
}

static void ensure_log_hook_installed(void)
{
    if (s_log_hook_installed) {
        return;
    }

    s_log_vprintf = esp_log_set_vprintf(web_log_vprintf);
    s_log_hook_installed = true;
}

static esp_err_t copy_log_buffer_since(uint64_t requested_cursor, char **out_text, size_t *out_len,
                                       uint64_t *out_cursor, bool *out_truncated)
{
    uint64_t current_cursor;
    uint64_t oldest_cursor;
    uint64_t start_cursor;
    size_t available_len;
    char *buffer;

    ESP_RETURN_ON_FALSE(out_text && out_len && out_cursor && out_truncated, ESP_ERR_INVALID_ARG, WEB_TAG,
                        "Invalid log copy arguments");

    taskENTER_CRITICAL(&s_log_buffer_lock);
    current_cursor = s_log_write_cursor;
    available_len = (current_cursor > LOG_BUFFER_SIZE) ? LOG_BUFFER_SIZE : (size_t)current_cursor;
    oldest_cursor = current_cursor - available_len;
    start_cursor = (requested_cursor < oldest_cursor) ? oldest_cursor : requested_cursor;
    *out_truncated = requested_cursor < oldest_cursor;
    *out_cursor = current_cursor;
    *out_len = (size_t)(current_cursor - start_cursor);
    taskEXIT_CRITICAL(&s_log_buffer_lock);

    buffer = malloc(*out_len + 1);
    ESP_RETURN_ON_FALSE(buffer, ESP_ERR_NO_MEM, WEB_TAG, "Failed to allocate log snapshot");

    taskENTER_CRITICAL(&s_log_buffer_lock);
    for (size_t index = 0; index < *out_len; ++index) {
        buffer[index] = s_log_buffer[(start_cursor + index) % LOG_BUFFER_SIZE];
    }
    taskEXIT_CRITICAL(&s_log_buffer_lock);

    buffer[*out_len] = '\0';
    *out_text = buffer;
    return ESP_OK;
}

static uint64_t parse_log_cursor(httpd_req_t *req)
{
    char query[LOG_QUERY_VALUE_LEN * 2];
    char cursor_value[LOG_QUERY_VALUE_LEN];
    size_t last_event_id_len = httpd_req_get_hdr_value_len(req, "Last-Event-ID");

    if (last_event_id_len > 0 && last_event_id_len < sizeof(cursor_value)) {
        if (httpd_req_get_hdr_value_str(req, "Last-Event-ID", cursor_value, sizeof(cursor_value)) == ESP_OK) {
            return strtoull(cursor_value, NULL, 10);
        }
    }

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return 0;
    }

    if (httpd_query_key_value(query, "cursor", cursor_value, sizeof(cursor_value)) != ESP_OK) {
        return 0;
    }

    return strtoull(cursor_value, NULL, 10);
}

static esp_err_t httpd_send_sse_event(httpd_req_t *req, const char *event_name, uint64_t cursor, const char *data)
{
    esp_err_t ret = ESP_OK;
    char header[96];
    const char *payload = data ? data : "";
    const char *line_start = payload;
    const char *newline = NULL;
    const char *line_end = NULL;

    if (event_name && event_name[0] != '\0') {
        snprintf(header, sizeof(header), "event: %s\n", event_name);
        ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, header), WEB_TAG, "Failed to send SSE event name");
    }

    snprintf(header, sizeof(header), "id: %llu\n", (unsigned long long)cursor);
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, header), WEB_TAG, "Failed to send SSE event id");

    while (1) {
        newline = strchr(line_start, '\n');
        line_end = newline ? newline : line_start + strlen(line_start);

        ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, "data:"), WEB_TAG, "Failed to send SSE prefix");
        if (line_end > line_start) {
            ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, " "), WEB_TAG, "Failed to send SSE separator");
            ret = httpd_resp_send_chunk(req, line_start, (ssize_t)(line_end - line_start));
            ESP_RETURN_ON_ERROR(ret, WEB_TAG, "Failed to send SSE line");
        }

        if (newline) {
            ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, "\n"), WEB_TAG, "Failed to terminate SSE line");
            line_start = newline + 1;
            continue;
        }

        ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, "\n\n"), WEB_TAG, "Failed to finalize SSE event");
        return ESP_OK;
    }
}

static bool httpd_sse_stream_closed(esp_err_t err)
{
    return err == ESP_ERR_HTTPD_RESP_SEND || err == ESP_ERR_HTTPD_INVALID_REQ;
}

static cJSON *create_ota_result(const char *status, const char *url)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        return NULL;
    }
    cJSON_AddStringToObject(result, "status", status ? status : "unknown");
    if (url) {
        cJSON_AddStringToObject(result, "url", url);
    }
    return result;
}

static bool ota_url_is_valid(const char *url)
{
    size_t url_len = 0;

    if (!url) {
        return false;
    }

    url_len = strlen(url);
    if (url_len == 0 || url_len >= OTA_URL_MAX_LEN) {
        return false;
    }

    return strncmp(url, "http://", strlen("http://")) == 0 || strncmp(url, "https://", strlen("https://")) == 0;
}

static bool ota_upload_content_type_is_valid(httpd_req_t *req)
{
    size_t header_len = httpd_req_get_hdr_value_len(req, "Content-Type");
    char *content_type = NULL;
    bool valid = true;

    if (header_len == 0) {
        return true;
    }

    content_type = calloc(header_len + 1, sizeof(char));
    if (!content_type) {
        return false;
    }

    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, header_len + 1) != ESP_OK) {
        free(content_type);
        return false;
    }

    valid = strncasecmp(content_type, "application/octet-stream", strlen("application/octet-stream")) == 0;
    free(content_type);
    return valid;
}

static bool ota_try_acquire_slot(void)
{
    bool acquired = false;

    if (!s_ota_mutex) {
        return false;
    }

    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    if (!s_ota_in_progress) {
        s_ota_in_progress = true;
        acquired = true;
    }

    xSemaphoreGive(s_ota_mutex);
    return acquired;
}

static void ota_release_slot(void)
{
    if (!s_ota_mutex) {
        return;
    }

    if (xSemaphoreTake(s_ota_mutex, portMAX_DELAY) == pdTRUE) {
        s_ota_in_progress = false;
        xSemaphoreGive(s_ota_mutex);
    }
}

static void ota_update_task(void *ctx)
{
    ota_request_context_t *request = (ota_request_context_t *)ctx;
    const char *cert_pem = (const char *)server_cert_pem_start;
    esp_http_client_config_t http_config = {
        .url = request->url,
        .cert_pem = cert_pem,
        .event_handler = NULL,
        .keep_alive_enable = true,
    };
    esp_err_t err = esp_br_http_ota(&http_config);

    if (err != ESP_OK) {
        ESP_LOGE(WEB_TAG, "OTA update failed for %s: %s", request->url, esp_err_to_name(err));
        ota_release_slot();
    } else {
        ESP_LOGI(WEB_TAG, "OTA image accepted from %s, restarting", request->url);
    }

    free(request->url);
    free(request);

    if (err == ESP_OK) {
        esp_restart();
    }

    vTaskDelete(NULL);
}

static void ota_restart_task(void *ctx)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(OTA_RESTART_DELAY_MS));
    esp_restart();
}

static esp_err_t ota_begin_host_update(esp_ota_handle_t *host_ota_handle, const esp_partition_t **update_partition)
{
    *update_partition = esp_ota_get_next_update_partition(NULL);
    ESP_RETURN_ON_FALSE(*update_partition != NULL, ESP_ERR_NOT_FOUND, WEB_TAG, "Failed to find ota partition");

    return esp_ota_begin(*update_partition, OTA_WITH_SEQUENTIAL_WRITES, host_ota_handle);
}

static esp_err_t ota_receive_uploaded_image(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    esp_rcp_ota_handle_t rcp_ota_handle = 0;
    esp_ota_handle_t host_ota_handle = 0;
    const esp_partition_t *update_partition = NULL;
    uint8_t *buffer = NULL;
    size_t remaining_len = req->content_len;
    uint32_t host_fw_downloaded = 0;
    uint32_t host_fw_size = 0;
    bool host_update_started = false;

    ESP_RETURN_ON_FALSE(remaining_len > 0, ESP_ERR_INVALID_ARG, WEB_TAG, "OTA upload body is empty");

    buffer = malloc(FILE_CHUNK_SIZE);
    ESP_RETURN_ON_FALSE(buffer, ESP_ERR_NO_MEM, WEB_TAG, "Failed to allocate OTA upload buffer");
    ESP_GOTO_ON_ERROR(esp_rcp_ota_begin(&rcp_ota_handle), exit, WEB_TAG, "Failed to begin RCP OTA");

    while (remaining_len > 0) {
        size_t chunk_size = remaining_len < FILE_CHUNK_SIZE ? remaining_len : FILE_CHUNK_SIZE;
        int recv_len = httpd_req_recv(req, (char *)buffer, chunk_size);
        size_t host_data_offset = 0;
        size_t host_data_len = 0;

        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        ESP_GOTO_ON_FALSE(recv_len > 0, ESP_FAIL, exit, WEB_TAG, "Failed to receive OTA upload body");
        remaining_len -= recv_len;

        if (esp_rcp_ota_get_state(rcp_ota_handle) != ESP_RCP_OTA_STATE_FINISHED) {
            size_t rcp_received_len = 0;
            ESP_GOTO_ON_ERROR(esp_rcp_ota_receive(rcp_ota_handle, buffer, recv_len, &rcp_received_len), exit, WEB_TAG,
                              "Failed to receive uploaded RCP OTA data");
            host_data_offset = rcp_received_len;

            if (esp_rcp_ota_get_state(rcp_ota_handle) == ESP_RCP_OTA_STATE_FINISHED) {
                host_fw_size = esp_rcp_ota_get_subfile_size(rcp_ota_handle, FILETAG_HOST_FIRMWARE);
                ESP_GOTO_ON_FALSE(host_fw_size > 0, ESP_ERR_INVALID_ARG, exit, WEB_TAG,
                                  "Uploaded OTA image does not contain host firmware");
                ESP_GOTO_ON_ERROR(ota_begin_host_update(&host_ota_handle, &update_partition), exit, WEB_TAG,
                                  "Failed to begin host OTA");
                host_update_started = true;
                ESP_LOGI(WEB_TAG, "Start writing uploaded border router firmware");
            }
        }

        if (esp_rcp_ota_get_state(rcp_ota_handle) == ESP_RCP_OTA_STATE_FINISHED) {
            host_data_len = recv_len - host_data_offset;
            if (host_data_len > 0) {
                ESP_GOTO_ON_FALSE(host_update_started, ESP_ERR_INVALID_STATE, exit, WEB_TAG,
                                  "Host OTA started before host partition was prepared");
                ESP_GOTO_ON_FALSE(host_fw_downloaded + host_data_len <= host_fw_size, ESP_ERR_INVALID_SIZE, exit,
                                  WEB_TAG, "Uploaded OTA image exceeds expected host firmware size");
                ESP_GOTO_ON_ERROR(esp_ota_write(host_ota_handle, buffer + host_data_offset, host_data_len), exit,
                                  WEB_TAG, "Failed to write uploaded host OTA data");
                host_fw_downloaded += host_data_len;
            }
        }
    }

    ESP_GOTO_ON_FALSE(esp_rcp_ota_get_state(rcp_ota_handle) == ESP_RCP_OTA_STATE_FINISHED, ESP_ERR_INVALID_ARG, exit,
                      WEB_TAG, "Uploaded OTA image ended before RCP firmware completed");
    ESP_GOTO_ON_FALSE(host_update_started, ESP_ERR_INVALID_ARG, exit, WEB_TAG,
                      "Uploaded OTA image never reached the host firmware payload");
    ESP_GOTO_ON_FALSE(host_fw_downloaded == host_fw_size, ESP_ERR_INVALID_SIZE, exit, WEB_TAG,
                      "Uploaded OTA image ended before host firmware completed");

    ret = esp_ota_end(host_ota_handle);
    host_ota_handle = 0;
    ESP_GOTO_ON_ERROR(ret, exit, WEB_TAG, "Failed to finalize uploaded host OTA");
    ESP_GOTO_ON_ERROR(esp_ota_set_boot_partition(update_partition), exit, WEB_TAG,
                      "Failed to set uploaded OTA boot partition");

    ret = esp_rcp_ota_end(rcp_ota_handle);
    if (ret != ESP_OK) {
        esp_ota_set_boot_partition(esp_ota_get_next_update_partition(NULL));
    }
    rcp_ota_handle = 0;

exit:
    free(buffer);
    if (ret != ESP_OK && host_ota_handle) {
        esp_ota_abort(host_ota_handle);
    }
    if (ret != ESP_OK && rcp_ota_handle) {
        esp_rcp_ota_abort(rcp_ota_handle);
    }
    return ret;
}

static esp_err_t ota_receive_app_image(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    esp_ota_handle_t host_ota_handle = 0;
    const esp_partition_t *update_partition = NULL;
    uint8_t *buffer = NULL;
    size_t remaining_len = req->content_len;

    ESP_RETURN_ON_FALSE(remaining_len > 0, ESP_ERR_INVALID_ARG, WEB_TAG, "App OTA upload body is empty");

    buffer = malloc(FILE_CHUNK_SIZE);
    ESP_RETURN_ON_FALSE(buffer, ESP_ERR_NO_MEM, WEB_TAG, "Failed to allocate app OTA upload buffer");
    ESP_GOTO_ON_ERROR(ota_begin_host_update(&host_ota_handle, &update_partition), exit, WEB_TAG,
                      "Failed to begin app OTA");

    while (remaining_len > 0) {
        size_t chunk_size = remaining_len < FILE_CHUNK_SIZE ? remaining_len : FILE_CHUNK_SIZE;
        int recv_len = httpd_req_recv(req, (char *)buffer, chunk_size);

        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        ESP_GOTO_ON_FALSE(recv_len > 0, ESP_FAIL, exit, WEB_TAG, "Failed to receive app OTA upload body");
        remaining_len -= recv_len;

        ESP_GOTO_ON_ERROR(esp_ota_write(host_ota_handle, buffer, recv_len), exit, WEB_TAG,
                          "Failed to write app OTA data");
    }

    ret = esp_ota_end(host_ota_handle);
    host_ota_handle = 0;
    ESP_GOTO_ON_ERROR(ret, exit, WEB_TAG, "Failed to finalize app OTA");
    ESP_GOTO_ON_ERROR(esp_ota_set_boot_partition(update_partition), exit, WEB_TAG,
                      "Failed to set app OTA boot partition");

exit:
    free(buffer);
    if (ret != ESP_OK && host_ota_handle) {
        esp_ota_abort(host_ota_handle);
    }
    return ret;
}

static esp_err_t ota_receive_rcp_image(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    esp_rcp_ota_handle_t rcp_ota_handle = 0;
    uint8_t *buffer = NULL;
    size_t remaining_len = req->content_len;

    ESP_RETURN_ON_FALSE(remaining_len > 0, ESP_ERR_INVALID_ARG, WEB_TAG, "RCP OTA upload body is empty");

    buffer = malloc(FILE_CHUNK_SIZE);
    ESP_RETURN_ON_FALSE(buffer, ESP_ERR_NO_MEM, WEB_TAG, "Failed to allocate RCP OTA upload buffer");
    ESP_GOTO_ON_ERROR(esp_rcp_ota_begin(&rcp_ota_handle), exit, WEB_TAG, "Failed to begin RCP OTA");

    while (remaining_len > 0) {
        size_t chunk_size = remaining_len < FILE_CHUNK_SIZE ? remaining_len : FILE_CHUNK_SIZE;
        int recv_len = httpd_req_recv(req, (char *)buffer, chunk_size);

        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        ESP_GOTO_ON_FALSE(recv_len > 0, ESP_FAIL, exit, WEB_TAG, "Failed to receive RCP OTA upload body");
        remaining_len -= recv_len;

        if (esp_rcp_ota_get_state(rcp_ota_handle) != ESP_RCP_OTA_STATE_FINISHED) {
            size_t rcp_received_len = 0;
            ESP_GOTO_ON_ERROR(esp_rcp_ota_receive(rcp_ota_handle, buffer, recv_len, &rcp_received_len), exit, WEB_TAG,
                              "Failed to receive uploaded RCP OTA data");
        }
    }

    ESP_GOTO_ON_FALSE(esp_rcp_ota_get_state(rcp_ota_handle) == ESP_RCP_OTA_STATE_FINISHED, ESP_ERR_INVALID_ARG, exit,
                      WEB_TAG, "Uploaded RCP OTA image ended before RCP firmware completed");

    ret = esp_rcp_ota_end(rcp_ota_handle);
    rcp_ota_handle = 0;
    ESP_GOTO_ON_ERROR(ret, exit, WEB_TAG, "Failed to finalize RCP OTA");

exit:
    free(buffer);
    if (ret != ESP_OK && rcp_ota_handle) {
        esp_rcp_ota_abort(rcp_ota_handle);
    }
    return ret;
}

static esp_err_t ota_receive_web_image(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    uint8_t *buffer = NULL;
    size_t remaining_len = req->content_len;
    size_t written = 0;

    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "web_storage");
    ESP_RETURN_ON_FALSE(partition != NULL, ESP_ERR_NOT_FOUND, WEB_TAG, "Failed to find web_storage partition");
    ESP_RETURN_ON_FALSE(remaining_len == (size_t)partition->size, ESP_ERR_INVALID_SIZE, WEB_TAG,
                        "Uploaded web image size (%zu) does not match partition size (%u)", remaining_len,
                        (unsigned)partition->size);

    buffer = malloc(FILE_CHUNK_SIZE);
    ESP_RETURN_ON_FALSE(buffer, ESP_ERR_NO_MEM, WEB_TAG, "Failed to allocate web OTA upload buffer");

    esp_err_t unregister_ret = esp_vfs_spiffs_unregister("web_storage");
    if (unregister_ret != ESP_OK) {
        ESP_LOGW(WEB_TAG, "Could not unmount web_storage before write: %s", esp_err_to_name(unregister_ret));
    }

    ESP_GOTO_ON_ERROR(esp_partition_erase_range(partition, 0, partition->size), exit, WEB_TAG,
                      "Failed to erase web_storage partition");

    while (remaining_len > 0) {
        size_t chunk_size = remaining_len < FILE_CHUNK_SIZE ? remaining_len : FILE_CHUNK_SIZE;
        int recv_len = httpd_req_recv(req, (char *)buffer, chunk_size);

        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        ESP_GOTO_ON_FALSE(recv_len > 0, ESP_FAIL, exit, WEB_TAG, "Failed to receive web OTA upload body");
        remaining_len -= recv_len;

        ESP_GOTO_ON_ERROR(esp_partition_write(partition, written, buffer, recv_len), exit, WEB_TAG,
                          "Failed to write web OTA data");
        written += recv_len;
    }

exit:
    free(buffer);
    return ret;
}

/*-----------------------------------------------------
 Note：Openthread resource API implement
-----------------------------------------------------*/
/**
 * @brief These APIs would collect corresponding information from Thread network and send to @param req.
 *
 * @param[in] req The request from http client.
 * @return
 *      -   ESP_OK   : On success
 *      -   ESP_FAIL : Failed to handle @param req
 *      -   ESP_ERR_INVALID_ARG         : Null request pointer
 *      -   ESP_ERR_HTTPD_RESP_HDR      : Essential headers are too large for internal buffer
 *      -   ESP_ERR_HTTPD_RESP_SEND     : Error in raw send
 *      -   ESP_ERR_HTTPD_INVALID_REQ   : Invalid request
 */

static esp_err_t esp_otbr_network_diagnostics_get_handler(httpd_req_t *req)
{
    ESP_RETURN_ON_FALSE(req, ESP_FAIL, WEB_TAG, "Failed to parse the diagnostics of http request");
    esp_err_t ret = ESP_OK;
    cJSON *response = handle_ot_resource_network_diagnostics_request();
    ESP_RETURN_ON_FALSE(response, ESP_FAIL, WEB_TAG, "Failed to handle openthread diagnostics request");

    /* Stream the JSON array in chunks to avoid allocating the entire
       serialized string in RAM at once (can be 30-50KB for large networks). */
    ESP_GOTO_ON_ERROR(httpd_resp_set_type(req, "application/json"), exit, WEB_TAG, "Failed to set content type");

    int array_size = cJSON_GetArraySize(response);
    ESP_GOTO_ON_ERROR(httpd_resp_sendstr_chunk(req, "["), exit, WEB_TAG, "Failed to send chunk");

    for (int i = 0; i < array_size; i++) {
        cJSON *detached = cJSON_DetachItemFromArray(response, 0);
        char *chunk = cJSON_PrintUnformatted(detached);
        cJSON_Delete(detached);
        if (chunk) {
            if (i > 0) {
                ESP_GOTO_ON_ERROR(httpd_resp_sendstr_chunk(req, ","), exit, WEB_TAG, "Failed to send chunk");
            }
            esp_err_t send_err = httpd_resp_sendstr_chunk(req, chunk);
            cJSON_free(chunk);
            ESP_GOTO_ON_ERROR(send_err, exit, WEB_TAG, "Failed to send chunk");
        }
    }

    ESP_GOTO_ON_ERROR(httpd_resp_sendstr_chunk(req, "]"), exit, WEB_TAG, "Failed to send chunk");
    /* Signal end of chunked response */
    httpd_resp_sendstr_chunk(req, NULL);

exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_network_node_get_handler(httpd_req_t *req)
{
    ESP_RETURN_ON_FALSE(req, ESP_FAIL, WEB_TAG, "Failed to parse the node information of http request");
    esp_err_t ret = ESP_OK;
    cJSON *response = handle_ot_resource_node_information_request();
    ESP_RETURN_ON_FALSE(response, ESP_FAIL, WEB_TAG, "Failed to handle openthread diagnostics request");
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_network_node_delete_handler(httpd_req_t *req)
{
    ESP_RETURN_ON_FALSE(req, ESP_FAIL, WEB_TAG, "Failed to parse the node information of http request");
    esp_err_t ret = ESP_OK;
    otError error = handle_ot_resource_node_delete_information_request();
    if (error == OT_ERROR_NONE) {
        httpd_resp_set_status(req, HTTPD_200);
    } else if (error == OT_ERROR_INVALID_STATE) {
        httpd_resp_set_status(req, HTTPD_409);
    } else {
        httpd_resp_set_status(req, HTTPD_500);
    }
    ESP_GOTO_ON_ERROR(httpd_resp_send(req, NULL, 0), exit, WEB_TAG, "Failed to response %s", req->uri);
exit:
    return ret;
}

static esp_err_t esp_otbr_network_node_rloc_get_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *response = handle_ot_resource_node_rloc_request();
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_network_node_rloc16_get_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *response = handle_ot_resource_node_rloc16_request();
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_network_node_state_get_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *response = handle_ot_resource_node_state_request();
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_network_node_state_put_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    otError err = OT_ERROR_NONE;
    cJSON *state = httpd_request_convert2_json(req, cJSON_Object);
    if (cJSON_IsString(state)) {
        err = handle_ot_resource_node_state_put_request(state);
    } else {
        ESP_LOGE(WEB_TAG, "Invalid args");
        err = OT_ERROR_INVALID_ARGS;
    }

    char http_return_status[64];
    if (convert_ot_err_to_response_code(err, http_return_status) != ESP_OK) {
        strcpy(http_return_status, HTTPD_500);
    }
    httpd_resp_set_status(req, http_return_status);
    ESP_GOTO_ON_ERROR(httpd_resp_send(req, NULL, 0), exit, WEB_TAG, "Failed to response %s", req->uri);
exit:
    cJSON_Delete(state);
    return ret;
}

static esp_err_t esp_otbr_network_node_extaddress_get_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *response = handle_ot_resource_node_extaddress_request();
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_network_node_network_name_get_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *response = handle_ot_resource_node_network_name_request();
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_network_node_leader_data_get_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *response = handle_ot_resource_node_leader_data_request();
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_network_node_number_of_router_get_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *response = handle_ot_resource_node_numofrouter_request();
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_network_node_extpanid_get_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *response = handle_ot_resource_node_extpanid_request();
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_network_node_baid_get_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *response = handle_ot_resource_node_baid_request();
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_network_node_dataset_active_handler(httpd_req_t *req)
{
    return esp_otbr_network_node_dataset_handler(req, ESP_OT_DATASET_TYPE_ACTIVE);
}

static esp_err_t esp_otbr_network_node_dataset_pending_handler(httpd_req_t *req)
{
    return esp_otbr_network_node_dataset_handler(req, ESP_OT_DATASET_TYPE_PENDING);
}

static esp_err_t esp_otbr_network_node_dataset_handler(httpd_req_t *req, const char *dataset_type)
{
    esp_err_t ret = ESP_OK;
    cJSON *request = cJSON_CreateObject();
    cJSON *response = NULL;
    cJSON *log = cJSON_CreateObject();
    cJSON_AddItemToObject(request, ESP_OT_REST_DATASET_TYPE, cJSON_CreateString(dataset_type));
    char format[256];
    uint16_t errcode = 0;

    if (req->method == HTTP_GET) {
        if (httpd_req_get_hdr_value_str(req, ESP_OT_REST_ACCEPT_HEADER, format, sizeof(format)) == ESP_OK &&
            strcmp(format, ESP_OT_REST_CONTENT_TYPE_PLAIN) == 0) {
            cJSON_AddItemToObject(request, ESP_OT_REST_ACCEPT_HEADER,
                                  cJSON_CreateString(ESP_OT_REST_CONTENT_TYPE_PLAIN));
        } else {
            cJSON_AddItemToObject(request, ESP_OT_REST_ACCEPT_HEADER,
                                  cJSON_CreateString(ESP_OT_REST_CONTENT_TYPE_JSON));
        }
        response = handle_ot_resource_node_get_dataset_request(request, log);
    } else if (req->method == HTTP_PUT) {
        cJSON *value = NULL;
        if (httpd_req_get_hdr_value_str(req, ESP_OT_REST_CONTENT_TYPE_HEADER, format, sizeof(format)) == ESP_OK &&
            strcmp(format, ESP_OT_REST_CONTENT_TYPE_PLAIN) == 0) {
            cJSON_AddItemToObject(request, ESP_OT_REST_CONTENT_TYPE_HEADER,
                                  cJSON_CreateString(ESP_OT_REST_CONTENT_TYPE_PLAIN));
            value = httpd_request_convert2_json(req, cJSON_String);
            if (!cJSON_IsString(value)) {
                errcode = 400;
            }
        } else {
            cJSON_AddItemToObject(request, ESP_OT_REST_CONTENT_TYPE_HEADER,
                                  cJSON_CreateString(ESP_OT_REST_CONTENT_TYPE_JSON));
            value = httpd_request_convert2_json(req, cJSON_Object);
            if (!cJSON_IsObject(value)) {
                errcode = 400;
            }
        }

        if (errcode == 0) {
            cJSON_AddItemToObject(request, "DatasetData", value);
            handle_ot_resource_node_set_dataset_request(request, log);
        } else if (errcode == 400) {
            ESP_LOGE(WEB_TAG, "Invalid args");
        }
    }

    cJSON *value = cJSON_GetObjectItemCaseSensitive(log, "ErrorCode");
    if (cJSON_IsNumber(value)) {
        errcode = (uint16_t)cJSON_GetNumberValue(value);
    }

    char http_return_status[64];
    ot_br_web_response_code_get(errcode, http_return_status);
    httpd_resp_set_status(req, http_return_status);
    if (response) {
        if (cJSON_IsString(response)) {
            ESP_GOTO_ON_ERROR(httpd_send_plain_text(req, cJSON_GetStringValue(response)), exit, WEB_TAG,
                              "Failed to response %s", req->uri);
        } else {
            ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
        }
    } else {
        ESP_GOTO_ON_ERROR(httpd_resp_send(req, NULL, 0), exit, WEB_TAG, "Failed to response %s", req->uri);
    }

exit:
    cJSON_Delete(request);
    cJSON_Delete(response);
    cJSON_Delete(log);
    return ret;
}

/*-----------------------------------------------------
 Note：Openthread WEB GUI API implement
-----------------------------------------------------*/
/**
 * @brief The API would collect the properties of Thread network and send to @param req.
 *
 * @param[in] req The request from http client.
 * @return
 *      -   ESP_OK                      : On success
 *      -   ESP_ERR_INVALID_ARG         : Null request pointer
 *      -   ESP_ERR_HTTPD_RESP_HDR      : Essential headers are too large for internal buffer
 *      -   ESP_ERR_HTTPD_RESP_SEND     : Error in raw send
 *      -   ESP_ERR_HTTPD_INVALID_REQ   : Invalid request
 */
static esp_err_t esp_otbr_network_properties_get_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *result = handle_openthread_network_properties_request(); /* encode json package */
    cJSON *error = result ? cJSON_CreateNumber((double)OT_ERROR_NONE) : cJSON_CreateNumber((double)OT_ERROR_FAILED);
    cJSON *message = result ? cJSON_CreateString("Properties: Success") : cJSON_CreateString("Properties: Failure");
    cJSON *response = pack_response(error, result, message);
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
    ESP_GOTO_ON_FALSE(result, ESP_FAIL, exit, WEB_TAG, "Failed to Get Thread network properties");
    // ESP_LOGI(WEB_TAG, "OpenThread properties collection complete");

exit:
    cJSON_Delete(response);
    return ret;
}

/**
 * @brief The API would discover the available thread network, packs and sends it to @param req.
 *
 * @param[in] req The request for http_client.
 * @return
 *      -   ESP_OK                      : On success
 *      -   ESP_ERR_HTTPD_RESP_HDR      : Essential headers are too large for internal buffer
 *      -   ESP_ERR_HTTPD_RESP_SEND     : Error in raw send
 *      -   ESP_ERR_HTTPD_INVALID_REQ   : Invalid request
 *      -   ESP_FAILED                  : Null request pointer.
 */
static esp_err_t esp_otbr_available_networks_get_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *result = handle_openthread_available_network_request();
    cJSON *error = result ? cJSON_CreateNumber((double)OT_ERROR_NONE) : cJSON_CreateNumber((double)OT_ERROR_FAILED);
    cJSON *message = result ? cJSON_CreateString("Networks: Success") : cJSON_CreateString("Networks: Failure");
    cJSON *response = pack_response(error, result, message);
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
    ESP_GOTO_ON_FALSE(result, ESP_FAIL, exit, WEB_TAG, "Failed to Discover Thread available networks");
    ESP_LOGI(WEB_TAG, "<================== Available Network =====================>");
    ESP_LOGI(WEB_TAG, "Discover Completed !");
    ESP_LOGI(WEB_TAG, "<==========================================================>");
exit:
    cJSON_Delete(response);
    return ret;
}

/**
 * @brief The API provides an entry for you to join the Thread network by the @param req.
 *
 * @param[in] req The request for http_client.
 * @return
 *      -   ESP_OK                      : On success
 *      -   ESP_ERR_HTTPD_RESP_HDR      : Essential headers are too large for internal buffer
 *      -   ESP_ERR_HTTPD_RESP_SEND     : Error in raw send
 *      -   ESP_ERR_HTTPD_INVALID_REQ   : Invalid request
 *      -   ESP_FAILED                  : Null request pointer.
 */
static esp_err_t esp_otbr_network_join_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *request = httpd_request_convert2_json(req, cJSON_Object);
    ESP_RETURN_ON_FALSE(request, ESP_FAIL, WEB_TAG, "Failed to parse the JOIN package");

    cJSON *join_log = cJSON_CreateString("Known");
    otError err = handle_openthread_join_network_request(request, join_log);
    cJSON *error = cJSON_CreateNumber((double)err);
    cJSON *result = err ? cJSON_CreateString("failed") : cJSON_CreateString("successful");
    cJSON *message = join_log;
    cJSON *response = pack_response(error, result, message);
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
    ESP_GOTO_ON_FALSE(!err, ESP_FAIL, exit, WEB_TAG, "Failed to JOIN openthread network");
    ESP_LOGI(WEB_TAG, "<================== Join Thread Network ====================>");
    ESP_LOGI(WEB_TAG, "Join request submitted, attaching...");
    ESP_LOGI(WEB_TAG, "<==========================================================>");
exit:
    cJSON_Delete(request);
    cJSON_Delete(response);
    return ret;
}

/**
 * @brief The API provides an entry to form a Thread network by the @param req.
 *
 * @param[in] req The request from http_client.
 * @return
 *      -   ESP_OK                      : On success
 *      -   ESP_ERR_HTTPD_RESP_HDR      : Essential headers are too large for internal buffer
 *      -   ESP_ERR_HTTPD_RESP_SEND     : Error in raw send
 *      -   ESP_ERR_HTTPD_INVALID_REQ   : Invalid request
 *      -   ESP_FAILED                  : Null request pointer
 */
static esp_err_t esp_otbr_network_form_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *request = httpd_request_convert2_json(req, cJSON_Object);
    ESP_RETURN_ON_FALSE(request, ESP_FAIL, WEB_TAG, "Failed to parse the FORM package");

    cJSON *form_log = cJSON_CreateString("Known");
    otError err = handle_openthread_form_network_request(request, form_log);
    cJSON *error = cJSON_CreateNumber((double)err);
    cJSON *result = err ? cJSON_CreateString("failed") : cJSON_CreateString("successful");
    cJSON *message = form_log;
    cJSON *response = pack_response(error, result, message);
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
    ESP_GOTO_ON_FALSE(!err, ESP_FAIL, exit, WEB_TAG, "Failed to form Thread network");
    ESP_LOGI(WEB_TAG, "<================== Form Thread Network ===================>");
    ESP_LOGI(WEB_TAG, "Form network successfully"); /* form successfully */
    ESP_LOGI(WEB_TAG, "<==========================================================>");
exit:
    cJSON_Delete(request);
    cJSON_Delete(response);
    return ret;
}

/**
 * @brief The API provides an entry to add network prefix to Thread node by the @param req.
 *
 * @param[in] req The request from http_client.
 * @return
 *      -   ESP_OK                      : On success
 *      -   ESP_ERR_HTTPD_RESP_HDR      : Essential headers are too large for internal buffer
 *      -   ESP_ERR_HTTPD_RESP_SEND     : Error in raw send
 *      -   ESP_ERR_HTTPD_INVALID_REQ   : Invalid request
 *      -   ESP_FAILED                  : Null request pointer
 */
static esp_err_t esp_otbr_add_network_prefix_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *request = httpd_request_convert2_json(req, cJSON_Object);
    ESP_RETURN_ON_FALSE(request, ESP_FAIL, WEB_TAG, "Failed to parse the add prefix package");
    otError err = handle_openthread_add_network_prefix_request(request);
    cJSON *error = cJSON_CreateNumber((double)err);
    cJSON *result = err ? cJSON_CreateString("failed") : cJSON_CreateString("successful");
    cJSON *message = err ? cJSON_CreateString("Add Prefix: Failure") : cJSON_CreateString("Add Prefix: Success");
    cJSON *response = pack_response(error, result, message);
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
    ESP_GOTO_ON_FALSE(!err, ESP_FAIL, exit, WEB_TAG, "Failed to ADD Thread network prefix");
    ESP_LOGI(WEB_TAG, "<================== Add Network Prefix ====================>");
    ESP_LOGI(WEB_TAG, "On mesh prefix: %s", cJSON_GetStringValue(cJSON_GetObjectItem(request, "prefix")));
    ESP_LOGI(WEB_TAG, "Add mesh prefix successfully"); /* successfully */
    ESP_LOGI(WEB_TAG, "<==========================================================>");
exit:
    cJSON_Delete(request);
    cJSON_Delete(response);
    return ret;
}

/**
 * @brief The API provides an entry to delete network prefix from Thread node by the @param req.
 *
 * @param[in] req The request from http_client.
 * @return
 *      -   ESP_OK                      : On success
 *      -   ESP_ERR_HTTPD_RESP_HDR      : Essential headers are too large for internal buffer
 *      -   ESP_ERR_HTTPD_RESP_SEND     : Error in raw send
 *      -   ESP_ERR_HTTPD_INVALID_REQ   : Invalid request
 *      -   ESP_FAILED                  : Null request pointer
 */
static esp_err_t esp_otbr_delete_network_prefix_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *request = httpd_request_convert2_json(req, cJSON_Object);
    ESP_RETURN_ON_FALSE(request, ESP_FAIL, WEB_TAG, "Failed to parse the delete prefix package");

    otError err = handle_openthread_delete_network_prefix_request(request);
    cJSON *error = cJSON_CreateNumber((double)err);
    cJSON *result = err ? cJSON_CreateString("failed") : cJSON_CreateString("successful");
    cJSON *message = err ? cJSON_CreateString("Delete Prefix: Failure") : cJSON_CreateString("Delete Prefix: Success");
    cJSON *response = pack_response(error, result, message);
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
    ESP_GOTO_ON_FALSE(!err, ESP_FAIL, exit, WEB_TAG, "Failed to DELETE Thread network prefix");
    ESP_LOGI(WEB_TAG, "<================= Delete Network Prefix ===================>");
    ESP_LOGI(WEB_TAG, "On mesh prefix: %s", cJSON_GetStringValue(cJSON_GetObjectItem(request, "prefix")));
    ESP_LOGI(WEB_TAG, "Delete mesh prefix successfully"); /* successfully */
    ESP_LOGI(WEB_TAG, "<==========================================================>");
exit:
    cJSON_Delete(request);
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_network_commission_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *request = httpd_request_convert2_json(req, cJSON_Object);
    ESP_RETURN_ON_FALSE(request, ESP_FAIL, WEB_TAG, "Failed to parse the add prefix package");

    otError err = handle_openthread_network_commission_request(request);
    cJSON *error = err ? cJSON_CreateNumber((double)err) : cJSON_CreateNumber((double)err);
    cJSON *result = err ? cJSON_CreateString("failed") : cJSON_CreateString("successful");
    cJSON *message = err ? cJSON_CreateString("Commissioner: Failure") : cJSON_CreateString("Commissioner: Process");
    cJSON *response = pack_response(error, result, message);
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
    ESP_GOTO_ON_FALSE(!err, ESP_FAIL, exit, WEB_TAG, "Failed to commsission thread network");
    ESP_LOGI(WEB_TAG, "<================== Thread Commission =====================>");
    ESP_LOGI(WEB_TAG, "Thread commission successfully"); /* successfully */
    ESP_LOGI(WEB_TAG, "<==========================================================>");
exit:
    cJSON_Delete(request);
    cJSON_Delete(response);
    return ret;
}

/**
 * @brief The API handles a ping request to an IPv6 address.
 *
 * @param[in] req The request from http_client (JSON with "address", optional "count" and "size").
 * @return
 *      -   ESP_OK                      : On success
 *      -   ESP_FAIL                    : On failure
 */
static esp_err_t esp_otbr_ping_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *request = httpd_request_convert2_json(req, cJSON_Object);
    ESP_RETURN_ON_FALSE(request, ESP_FAIL, WEB_TAG, "Failed to parse the ping request");

    cJSON *result = handle_openthread_ping_request(request);
    cJSON *error = result ? cJSON_CreateNumber((double)OT_ERROR_NONE) : cJSON_CreateNumber((double)OT_ERROR_FAILED);
    cJSON *message = result ? cJSON_CreateString("Ping: Success") : cJSON_CreateString("Ping: Failure");
    cJSON *response = pack_response(error, result, message);
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
    ESP_GOTO_ON_FALSE(result, ESP_FAIL, exit, WEB_TAG, "Failed to execute ping");
    ESP_LOGI(WEB_TAG, "<====================== Ping ==============================>");
    ESP_LOGI(WEB_TAG, "Ping completed");
    ESP_LOGI(WEB_TAG, "<==========================================================>");
exit:
    cJSON_Delete(request);
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_logs_stream_get_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    bool client_closed = false;
    uint64_t cursor = parse_log_cursor(req);
    TickType_t last_keepalive = xTaskGetTickCount();

    ESP_GOTO_ON_ERROR(httpd_resp_set_type(req, "text/event-stream"), exit, WEB_TAG, "Failed to set SSE content type");
    ESP_GOTO_ON_ERROR(httpd_resp_set_hdr(req, "Cache-Control", "no-store"), exit, WEB_TAG,
                      "Failed to set cache header");
    ESP_GOTO_ON_ERROR(httpd_resp_set_hdr(req, "Connection", "keep-alive"), exit, WEB_TAG,
                      "Failed to set SSE connection header");
    ESP_GOTO_ON_ERROR(httpd_resp_set_hdr(req, "X-Accel-Buffering", "no"), exit, WEB_TAG, "Failed to disable buffering");
    ESP_GOTO_ON_ERROR(httpd_resp_sendstr_chunk(req, "retry: 1000\n\n"), exit, WEB_TAG,
                      "Failed to initialize SSE stream");

    while (1) {
        char *log_text = NULL;
        size_t log_len = 0;
        uint64_t next_cursor = cursor;
        bool truncated = false;

        ret = copy_log_buffer_since(cursor, &log_text, &log_len, &next_cursor, &truncated);
        if (ret != ESP_OK) {
            client_closed = httpd_sse_stream_closed(ret);
            goto exit;
        }

        if (truncated) {
            ret = httpd_send_sse_event(req, "reset", next_cursor, "log buffer wrapped; showing newest retained output");
            if (ret != ESP_OK) {
                free(log_text);
                client_closed = httpd_sse_stream_closed(ret);
                goto exit;
            }
        }

        if (log_len > 0) {
            ret = httpd_send_sse_event(req, "log", next_cursor, log_text);
            free(log_text);
            if (ret != ESP_OK) {
                client_closed = httpd_sse_stream_closed(ret);
                goto exit;
            }
            cursor = next_cursor;
            last_keepalive = xTaskGetTickCount();
            continue;
        }

        free(log_text);

        if ((xTaskGetTickCount() - last_keepalive) >= pdMS_TO_TICKS(LOG_SSE_KEEPALIVE_MS)) {
            ret = httpd_resp_sendstr_chunk(req, ": keepalive\n\n");
            if (ret != ESP_OK) {
                client_closed = httpd_sse_stream_closed(ret);
                goto exit;
            }
            last_keepalive = xTaskGetTickCount();
        }

        vTaskDelay(pdMS_TO_TICKS(LOG_SSE_POLL_MS));
    }

exit:
    if (!client_closed) {
        httpd_resp_sendstr_chunk(req, NULL);
        return ret;
    }

    ESP_LOGD(WEB_TAG, "SSE client disconnected");
    return ESP_OK;
}

static esp_err_t esp_otbr_ota_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    char *http_status = "202 Accepted";
    cJSON *request = httpd_request_convert2_json(req, cJSON_Object);
    cJSON *url = NULL;
    cJSON *error = NULL;
    cJSON *result = NULL;
    cJSON *message = NULL;
    cJSON *response = NULL;
    ota_request_context_t *ota_request = NULL;

    ESP_RETURN_ON_FALSE(request, ESP_FAIL, WEB_TAG, "Failed to parse OTA request");

    url = cJSON_GetObjectItemCaseSensitive(request, "url");
    if (!cJSON_IsString(url) || !ota_url_is_valid(url->valuestring)) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_ARG);
        result = create_ota_result("invalid_url", NULL);
        message = cJSON_CreateString("Provide a valid http:// or https:// OTA URL.");
        goto respond;
    }

    if (!ota_try_acquire_slot()) {
        http_status = HTTPD_409;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_STATE);
        result = create_ota_result("busy", url->valuestring);
        message = cJSON_CreateString("An OTA update is already in progress.");
        goto respond;
    }

    ota_request = calloc(1, sizeof(*ota_request));
    if (!ota_request) {
        ota_release_slot();
        http_status = HTTPD_500;
        error = cJSON_CreateNumber((double)ESP_ERR_NO_MEM);
        result = create_ota_result("allocation_failed", url->valuestring);
        message = cJSON_CreateString("Failed to allocate the OTA request context.");
        goto respond;
    }

    ota_request->url = strdup(url->valuestring);
    if (!ota_request->url) {
        ota_release_slot();
        http_status = HTTPD_500;
        error = cJSON_CreateNumber((double)ESP_ERR_NO_MEM);
        result = create_ota_result("allocation_failed", url->valuestring);
        message = cJSON_CreateString("Failed to copy the OTA URL.");
        goto respond;
    }

    if (xTaskCreate(ota_update_task, "br_ota_web", OTA_TASK_STACK_SIZE, ota_request, OTA_TASK_PRIORITY, NULL) !=
        pdPASS) {
        ota_release_slot();
        http_status = HTTPD_500;
        error = cJSON_CreateNumber((double)ESP_FAIL);
        result = create_ota_result("task_start_failed", url->valuestring);
        message = cJSON_CreateString("Failed to start the OTA worker task.");
        goto respond;
    }

    error = cJSON_CreateNumber((double)ESP_OK);
    result = create_ota_result("accepted", url->valuestring);
    message = cJSON_CreateString("OTA accepted. The device will reboot automatically after a successful update.");
    ota_request = NULL;

respond:
    httpd_resp_set_status(req, http_status);
    response = pack_response(error, result, message);
    ESP_GOTO_ON_FALSE(response, ESP_FAIL, exit, WEB_TAG, "Failed to build OTA response");
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to respond %s", req->uri);

exit:
    if (ota_request) {
        free(ota_request->url);
        free(ota_request);
    }
    cJSON_Delete(request);
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_ota_upload_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    char *http_status = "200 OK";
    bool restart_required = false;
    cJSON *error = NULL;
    cJSON *result = NULL;
    cJSON *message = NULL;
    cJSON *response = NULL;

    if (!ota_upload_content_type_is_valid(req)) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_ARG);
        result = create_ota_result("invalid_upload", NULL);
        message = cJSON_CreateString("Upload a combined host+RCP OTA image as application/octet-stream.");
        goto respond;
    }

    if (req->content_len <= 0) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_ARG);
        result = create_ota_result("invalid_upload", NULL);
        message = cJSON_CreateString("Select a non-empty combined host+RCP OTA image to upload.");
        goto respond;
    }

    if (!ota_try_acquire_slot()) {
        http_status = HTTPD_409;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_STATE);
        result = create_ota_result("busy", NULL);
        message = cJSON_CreateString("An OTA update is already in progress.");
        goto respond;
    }

    ret = ota_receive_uploaded_image(req);
    if (ret != ESP_OK) {
        ota_release_slot();
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ret);
        result = create_ota_result("upload_failed", NULL);
        message = cJSON_CreateString("Failed to process the uploaded combined host+RCP OTA image.");
        goto respond;
    }

    error = cJSON_CreateNumber((double)ESP_OK);
    result = create_ota_result("accepted", NULL);
    message = cJSON_CreateString("Upload completed. The device will reboot automatically after a successful update.");
    restart_required = true;

respond:
    httpd_resp_set_status(req, http_status);
    response = pack_response(error, result, message);
    ESP_GOTO_ON_FALSE(response, ESP_FAIL, exit, WEB_TAG, "Failed to build OTA upload response");
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to respond %s", req->uri);

exit:
    cJSON_Delete(response);

    if (restart_required) {
        if (xTaskCreate(ota_restart_task, "br_ota_restart", OTA_RESTART_TASK_STACK_SIZE, NULL, OTA_TASK_PRIORITY,
                        NULL) != pdPASS) {
            esp_restart();
        }
    }

    return ret;
}

static esp_err_t esp_otbr_ota_upload_app_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    char *http_status = "200 OK";
    cJSON *error = NULL;
    cJSON *result = NULL;
    cJSON *message = NULL;
    cJSON *response = NULL;

    if (!ota_upload_content_type_is_valid(req)) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_ARG);
        result = create_ota_result("invalid_upload", NULL);
        message = cJSON_CreateString("Upload the app firmware binary as application/octet-stream.");
        goto respond;
    }

    if (req->content_len <= 0) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_ARG);
        result = create_ota_result("invalid_upload", NULL);
        message = cJSON_CreateString("Select a non-empty app firmware binary to upload.");
        goto respond;
    }

    if (!ota_try_acquire_slot()) {
        http_status = HTTPD_409;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_STATE);
        result = create_ota_result("busy", NULL);
        message = cJSON_CreateString("An OTA update is already in progress.");
        goto respond;
    }

    ret = ota_receive_app_image(req);
    ota_release_slot();
    if (ret != ESP_OK) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ret);
        result = create_ota_result("upload_failed", NULL);
        message = cJSON_CreateString("Failed to process the uploaded app firmware binary.");
        goto respond;
    }

    error = cJSON_CreateNumber((double)ESP_OK);
    result = create_ota_result("accepted", NULL);
    message = cJSON_CreateString("App firmware uploaded successfully.");

respond:
    httpd_resp_set_status(req, http_status);
    response = pack_response(error, result, message);
    ESP_GOTO_ON_FALSE(response, ESP_FAIL, exit, WEB_TAG, "Failed to build app OTA upload response");
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to respond %s", req->uri);

exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_ota_upload_rcp_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    char *http_status = "200 OK";
    cJSON *error = NULL;
    cJSON *result = NULL;
    cJSON *message = NULL;
    cJSON *response = NULL;

    if (!ota_upload_content_type_is_valid(req)) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_ARG);
        result = create_ota_result("invalid_upload", NULL);
        message = cJSON_CreateString("Upload the RCP OTA image as application/octet-stream.");
        goto respond;
    }

    if (req->content_len <= 0) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_ARG);
        result = create_ota_result("invalid_upload", NULL);
        message = cJSON_CreateString("Select a non-empty RCP OTA image to upload.");
        goto respond;
    }

    if (!ota_try_acquire_slot()) {
        http_status = HTTPD_409;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_STATE);
        result = create_ota_result("busy", NULL);
        message = cJSON_CreateString("An OTA update is already in progress.");
        goto respond;
    }

    ret = ota_receive_rcp_image(req);
    ota_release_slot();
    if (ret != ESP_OK) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ret);
        result = create_ota_result("upload_failed", NULL);
        message = cJSON_CreateString("Failed to process the uploaded RCP OTA image.");
        goto respond;
    }

    error = cJSON_CreateNumber((double)ESP_OK);
    result = create_ota_result("accepted", NULL);
    message = cJSON_CreateString("RCP firmware uploaded successfully.");

respond:
    httpd_resp_set_status(req, http_status);
    response = pack_response(error, result, message);
    ESP_GOTO_ON_FALSE(response, ESP_FAIL, exit, WEB_TAG, "Failed to build RCP OTA upload response");
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to respond %s", req->uri);

exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_ota_upload_web_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    char *http_status = "200 OK";
    cJSON *error = NULL;
    cJSON *result = NULL;
    cJSON *message = NULL;
    cJSON *response = NULL;

    if (!ota_upload_content_type_is_valid(req)) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_ARG);
        result = create_ota_result("invalid_upload", NULL);
        message = cJSON_CreateString("Upload the web SPIFFS image as application/octet-stream.");
        goto respond;
    }

    if (req->content_len <= 0) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_ARG);
        result = create_ota_result("invalid_upload", NULL);
        message = cJSON_CreateString("Select a non-empty web SPIFFS image to upload.");
        goto respond;
    }

    if (!ota_try_acquire_slot()) {
        http_status = HTTPD_409;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_STATE);
        result = create_ota_result("busy", NULL);
        message = cJSON_CreateString("An OTA update is already in progress.");
        goto respond;
    }

    ret = ota_receive_web_image(req);
    ota_release_slot();
    if (ret != ESP_OK) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ret);
        result = create_ota_result("upload_failed", NULL);
        message = cJSON_CreateString("Failed to process the uploaded web SPIFFS image. Ensure it is exactly 200 KB.");
        goto respond;
    }

    error = cJSON_CreateNumber((double)ESP_OK);
    result = create_ota_result("accepted", NULL);
    message = cJSON_CreateString("Web frontend uploaded successfully.");

respond:
    httpd_resp_set_status(req, http_status);
    response = pack_response(error, result, message);
    ESP_GOTO_ON_FALSE(response, ESP_FAIL, exit, WEB_TAG, "Failed to build web OTA upload response");
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to respond %s", req->uri);

exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_ota_restart_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *error = cJSON_CreateNumber((double)ESP_OK);
    cJSON *result = create_ota_result("restarting", NULL);
    cJSON *message = cJSON_CreateString("The device will restart now.");
    cJSON *response = pack_response(error, result, message);
    ESP_GOTO_ON_FALSE(response, ESP_FAIL, exit, WEB_TAG, "Failed to build OTA restart response");
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to respond %s", req->uri);

    if (xTaskCreate(ota_restart_task, "br_ota_restart", OTA_RESTART_TASK_STACK_SIZE, NULL, OTA_TASK_PRIORITY, NULL) !=
        pdPASS) {
        esp_restart();
    }

exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_ipaddr_get_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *result = handle_openthread_ipaddr_list_request();
    cJSON *error = result ? cJSON_CreateNumber((double)OT_ERROR_NONE) : cJSON_CreateNumber((double)OT_ERROR_FAILED);
    cJSON *message = result ? cJSON_CreateString("OK") : cJSON_CreateString("Failed to list addresses");
    cJSON *response = pack_response(error, result, message);
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to respond %s", req->uri);
exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_add_ipaddr_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *request = httpd_request_convert2_json(req, cJSON_Object);
    ESP_RETURN_ON_FALSE(request, ESP_FAIL, WEB_TAG, "Failed to parse ipaddr add request");

    cJSON *result = handle_openthread_add_ipaddr_request(request);
    cJSON *status = cJSON_GetObjectItem(result, "status");
    bool ok = status && status->valuestring && strcmp(status->valuestring, "ok") == 0;
    cJSON *error = cJSON_CreateNumber(ok ? (double)OT_ERROR_NONE : (double)OT_ERROR_FAILED);
    cJSON *message = cJSON_CreateString(ok ? "Address added" : "Failed to add address");
    cJSON *response = pack_response(error, result, message);
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to respond %s", req->uri);
exit:
    cJSON_Delete(request);
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_delete_ipaddr_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *request = httpd_request_convert2_json(req, cJSON_Object);
    ESP_RETURN_ON_FALSE(request, ESP_FAIL, WEB_TAG, "Failed to parse ipaddr delete request");

    cJSON *result = handle_openthread_delete_ipaddr_request(request);
    cJSON *status = cJSON_GetObjectItem(result, "status");
    bool ok = status && status->valuestring && strcmp(status->valuestring, "ok") == 0;
    cJSON *error = cJSON_CreateNumber(ok ? (double)OT_ERROR_NONE : (double)OT_ERROR_FAILED);
    cJSON *message = cJSON_CreateString(ok ? "Address removed" : "Failed to remove address");
    cJSON *response = pack_response(error, result, message);
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to respond %s", req->uri);
exit:
    cJSON_Delete(request);
    cJSON_Delete(response);
    return ret;
}

/*-----------------------------------------------------
 Note: Advanced management API handlers
-----------------------------------------------------*/

static esp_err_t esp_otbr_leader_weight_get_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    uint8_t weight = handle_ot_leader_weight_get_request();
    cJSON *error = cJSON_CreateNumber((double)OT_ERROR_NONE);
    cJSON *result = cJSON_CreateNumber((double)weight);
    cJSON *message = cJSON_CreateString("ok");
    cJSON *response = pack_response(error, result, message);
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to respond %s", req->uri);
exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_leader_weight_put_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *request = NULL;
    cJSON *response = NULL;
    cJSON *error = NULL;
    cJSON *result = NULL;
    cJSON *message = NULL;

    request = httpd_request_convert2_json(req, cJSON_Object);
    if (!request) {
        ESP_LOGE(WEB_TAG, "Failed to parse leader weight request");
        return ESP_FAIL;
    }

    cJSON *weight_json = cJSON_GetObjectItem(request, "weight");
    if (!cJSON_IsNumber(weight_json) || weight_json->valueint < 0 || weight_json->valueint > 255) {
        error = cJSON_CreateNumber((double)OT_ERROR_INVALID_ARGS);
        result = cJSON_CreateString("failed");
        message = cJSON_CreateString("Missing or invalid 'weight' field (0-255 required)");
    } else {
        handle_ot_leader_weight_put_request((uint8_t)weight_json->valueint);
        error = cJSON_CreateNumber((double)OT_ERROR_NONE);
        result = cJSON_CreateString("ok");
        message = cJSON_CreateString("Leader weight updated");
    }

    response = pack_response(error, result, message);
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to respond %s", req->uri);
exit:
    cJSON_Delete(request);
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_become_leader_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    otError err = handle_ot_become_leader_request();
    cJSON *error = cJSON_CreateNumber((double)err);
    cJSON *result = err ? cJSON_CreateString("failed") : cJSON_CreateString("ok");
    cJSON *message =
        err ? cJSON_CreateString(otThreadErrorToString(err)) : cJSON_CreateString("Become-leader request submitted");
    cJSON *response = pack_response(error, result, message);
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to respond %s", req->uri);
exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_restart_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *error = cJSON_CreateNumber((double)ESP_OK);
    cJSON *result = cJSON_CreateString("accepted");
    cJSON *message = cJSON_CreateString("The device will restart shortly.");
    cJSON *response = pack_response(error, result, message);
    ESP_GOTO_ON_FALSE(response, ESP_FAIL, exit, WEB_TAG, "Failed to build restart response");
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to respond %s", req->uri);

    if (xTaskCreate(ota_restart_task, "br_restart", OTA_RESTART_TASK_STACK_SIZE, NULL, OTA_TASK_PRIORITY, NULL) !=
        pdPASS) {
        esp_restart();
    }

exit:
    cJSON_Delete(response);
    return ret;
}

static esp_err_t esp_otbr_factory_reset_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *request = NULL;
    cJSON *error = NULL;
    cJSON *result = NULL;
    cJSON *message = NULL;
    cJSON *response = NULL;
    char *http_status = "200 OK";

    request = httpd_request_convert2_json(req, cJSON_Object);
    if (!request) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_ARG);
        result = cJSON_CreateString("invalid_request");
        message = cJSON_CreateString("Could not parse request body.");
        goto respond;
    }

    cJSON *confirm = cJSON_GetObjectItemCaseSensitive(request, "confirm");
    if (!cJSON_IsString(confirm) || strcmp(confirm->valuestring, "FACTORY_RESET") != 0) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_ARG);
        result = cJSON_CreateString("invalid_confirmation");
        message = cJSON_CreateString("Send {\"confirm\":\"FACTORY_RESET\"} to proceed.");
        goto respond;
    }

    esp_err_t erase_ret = nvs_flash_erase();
    if (erase_ret != ESP_OK) {
        http_status = HTTPD_500;
        error = cJSON_CreateNumber((double)erase_ret);
        result = cJSON_CreateString("erase_failed");
        message = cJSON_CreateString("Failed to erase NVS partition.");
        goto respond;
    }

    error = cJSON_CreateNumber((double)ESP_OK);
    result = cJSON_CreateString("accepted");
    message = cJSON_CreateString("NVS erased. The device will restart shortly.");

respond:
    httpd_resp_set_status(req, http_status);
    response = pack_response(error, result, message);
    ESP_GOTO_ON_FALSE(response, ESP_FAIL, exit, WEB_TAG, "Failed to build factory reset response");
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to respond %s", req->uri);

    if (strcmp(http_status, "200 OK") == 0) {
        if (xTaskCreate(ota_restart_task, "br_factory_rst", OTA_RESTART_TASK_STACK_SIZE, NULL, OTA_TASK_PRIORITY,
                        NULL) != pdPASS) {
            esp_restart();
        }
    }

exit:
    cJSON_Delete(request);
    cJSON_Delete(response);
    return ret;
}

/* Maximum size of a single base64-encoded NVS blob (64 KB blob → ~88 KB b64) */
#define NVS_BACKUP_B64_MAX (88 * 1024)
/* Maximum raw blob size we are willing to encode */
#define NVS_BACKUP_BLOB_MAX (64 * 1024)

static esp_err_t esp_otbr_nvs_backup_get_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    nvs_iterator_t it = NULL;
    nvs_handle_t ns_handle = 0;
    bool ns_open = false;
    uint8_t *blob_buf = NULL;
    unsigned char *b64_buf = NULL;
    cJSON *root = NULL;
    cJSON *entries = NULL;

    root = cJSON_CreateObject();
    ESP_GOTO_ON_FALSE(root, ESP_ERR_NO_MEM, exit_no_root, WEB_TAG, "OOM building NVS backup root");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "partition", "nvs");
    entries = cJSON_AddObjectToObject(root, "entries");
    ESP_GOTO_ON_FALSE(entries, ESP_ERR_NO_MEM, exit, WEB_TAG, "OOM building NVS backup entries");

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

        /* Get or create namespace object in JSON */
        cJSON *ns_obj = cJSON_GetObjectItemCaseSensitive(entries, info.namespace_name);
        if (!ns_obj) {
            ns_obj = cJSON_AddObjectToObject(entries, info.namespace_name);
        }

        /* Open namespace handle if not already open for this namespace */
        if (!ns_open ||
            (ns_open && ns_handle &&
             strcmp(cJSON_GetObjectItemCaseSensitive(root, "_cur_ns")
                        ? cJSON_GetObjectItemCaseSensitive(root, "_cur_ns")->valuestring
                        : "",
                    info.namespace_name) != 0)) {
            if (ns_open) {
                nvs_close(ns_handle);
                ns_open = false;
            }
            if (nvs_open(info.namespace_name, NVS_READONLY, &ns_handle) == ESP_OK) {
                ns_open = true;
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
                    /* JSON numbers lose precision for large u64 values; store as string */
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

    {
        char *json_str = cJSON_PrintUnformatted(root);
        ESP_GOTO_ON_FALSE(json_str, ESP_ERR_NO_MEM, exit, WEB_TAG, "OOM serialising NVS backup");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"nvs_backup.json\"");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        esp_err_t send_ret = httpd_resp_sendstr(req, json_str);
        cJSON_free(json_str);
        ESP_GOTO_ON_ERROR(send_ret, exit, WEB_TAG, "Failed to send NVS backup response");
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

static esp_err_t esp_otbr_nvs_restore_post_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    char *body = NULL;
    cJSON *root = NULL;
    cJSON *entries = NULL;
    char *http_status = "200 OK";
    cJSON *error = NULL;
    cJSON *result = NULL;
    cJSON *message = NULL;
    cJSON *response = NULL;

    if (req->content_len == 0 || req->content_len > (512 * 1024)) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_ARG);
        result = cJSON_CreateString("invalid_request");
        message = cJSON_CreateString("Request body is empty or too large.");
        goto respond;
    }

    body = malloc(req->content_len + 1);
    if (!body) {
        http_status = HTTPD_500;
        error = cJSON_CreateNumber((double)ESP_ERR_NO_MEM);
        result = cJSON_CreateString("oom");
        message = cJSON_CreateString("Not enough memory to read backup.");
        goto respond;
    }

    {
        int received = 0;
        int total = 0;
        while (total < (int)req->content_len) {
            received = httpd_req_recv(req, body + total, req->content_len - total);
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            if (received <= 0) {
                http_status = HTTPD_500;
                error = cJSON_CreateNumber((double)ESP_FAIL);
                result = cJSON_CreateString("receive_error");
                message = cJSON_CreateString("Failed to receive request body.");
                goto respond;
            }
            total += received;
        }
        body[total] = '\0';
    }

    root = cJSON_Parse(body);
    if (!root) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_ARG);
        result = cJSON_CreateString("parse_error");
        message = cJSON_CreateString("Could not parse backup JSON.");
        goto respond;
    }

    entries = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (!cJSON_IsObject(entries)) {
        http_status = HTTPD_400;
        error = cJSON_CreateNumber((double)ESP_ERR_INVALID_ARG);
        result = cJSON_CreateString("invalid_format");
        message = cJSON_CreateString("Backup JSON is missing \"entries\" object.");
        goto respond;
    }

    /* Iterate namespaces */
    cJSON *ns_item = NULL;
    cJSON_ArrayForEach(ns_item, entries)
    {
        const char *ns_name = ns_item->string;
        if (!ns_name || !cJSON_IsObject(ns_item)) {
            continue;
        }

        nvs_handle_t ns_handle = 0;
        if (nvs_open(ns_name, NVS_READWRITE, &ns_handle) != ESP_OK) {
            ESP_LOGW(WEB_TAG, "NVS restore: could not open namespace '%s', skipping", ns_name);
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
                uint64_t v = (uint64_t)strtoull(value_j->valuestring, NULL, 10);
                set_ret = nvs_set_u64(ns_handle, key, v);
            } else if (strcmp(type, "i64") == 0 && cJSON_IsString(value_j)) {
                int64_t v = (int64_t)strtoll(value_j->valuestring, NULL, 10);
                set_ret = nvs_set_i64(ns_handle, key, v);
            } else if (strcmp(type, "str") == 0 && cJSON_IsString(value_j)) {
                set_ret = nvs_set_str(ns_handle, key, value_j->valuestring);
            } else if (strcmp(type, "blob") == 0 && cJSON_IsString(value_j)) {
                const char *b64 = value_j->valuestring;
                size_t b64_len = strlen(b64);
                size_t decoded_len = 0;
                /* mbedtls_base64_decode with NULL output gives required size */
                int b64_ret = mbedtls_base64_decode(NULL, 0, &decoded_len, (const unsigned char *)b64, b64_len);
                /* b64_ret == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL is expected when dest is NULL */
                if (decoded_len > 0 && decoded_len <= NVS_BACKUP_BLOB_MAX) {
                    uint8_t *blob = malloc(decoded_len);
                    if (blob) {
                        b64_ret =
                            mbedtls_base64_decode(blob, decoded_len, &decoded_len, (const unsigned char *)b64, b64_len);
                        if (b64_ret == 0) {
                            set_ret = nvs_set_blob(ns_handle, key, blob, decoded_len);
                        }
                        free(blob);
                    }
                }
            }

            if (set_ret != ESP_OK) {
                ESP_LOGW(WEB_TAG, "NVS restore: failed to set %s/%s: %s", ns_name, key, esp_err_to_name(set_ret));
            }
        }

        nvs_commit(ns_handle);
        nvs_close(ns_handle);
    }

    error = cJSON_CreateNumber((double)ESP_OK);
    result = cJSON_CreateString("restored");
    message = cJSON_CreateString("NVS settings restored successfully. Restart the device for changes to take effect.");

respond:
    httpd_resp_set_status(req, http_status);
    response = pack_response(error, result, message);
    ESP_GOTO_ON_FALSE(response, ESP_FAIL, exit, WEB_TAG, "Failed to build NVS restore response");
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to respond %s", req->uri);

exit:
    free(body);
    cJSON_Delete(root);
    cJSON_Delete(response);
    return ret;
}

/**
 * @brief The API provides an entry to collect the topology of Thread node, packs and sends it to @param req.
 *
 * @param[in] req The request from http_client.
 * @return
 *      -   ESP_OK                      : On success
 *      -   ESP_ERR_HTTPD_RESP_HDR      : Essential headers are too large for internal buffer
 *      -   ESP_ERR_HTTPD_RESP_SEND     : Error in raw send
 *      -   ESP_ERR_HTTPD_INVALID_REQ   : Invalid request
 *      -   ESP_FAILED                  : Null request pointer
 */
static esp_err_t esp_otbr_network_topology_get_handler(httpd_req_t *req)
{
    ESP_RETURN_ON_FALSE(req, ESP_FAIL, WEB_TAG, "Failed to parse the diagnostics of http request");
    esp_err_t ret = ESP_OK;
    cJSON *result = handle_ot_resource_network_diagnostics_request();
    ESP_RETURN_ON_FALSE(result, ESP_FAIL, WEB_TAG, "Failed to get Thread Network Topology");

    /* Stream the wrapped JSON response in chunks to avoid allocating the
       entire serialized string in RAM at once (can be 30-50 KB for large networks).
       Format: {"error":0,"result":[<item>,<item>,...],"message":"Topology: Success"} */
    ESP_GOTO_ON_ERROR(httpd_resp_set_type(req, "application/json"), exit, WEB_TAG, "Failed to set content type");
    ESP_GOTO_ON_ERROR(httpd_resp_sendstr_chunk(req, "{\"error\":0,\"result\":["), exit, WEB_TAG,
                      "Failed to send chunk");

    int array_size = cJSON_GetArraySize(result);
    for (int i = 0; i < array_size; i++) {
        cJSON *detached = cJSON_DetachItemFromArray(result, 0);
        char *chunk = cJSON_PrintUnformatted(detached);
        cJSON_Delete(detached);
        if (chunk) {
            if (i > 0) {
                ESP_GOTO_ON_ERROR(httpd_resp_sendstr_chunk(req, ","), exit, WEB_TAG, "Failed to send chunk");
            }
            esp_err_t send_err = httpd_resp_sendstr_chunk(req, chunk);
            cJSON_free(chunk);
            ESP_GOTO_ON_ERROR(send_err, exit, WEB_TAG, "Failed to send chunk");
        }
    }

    ESP_GOTO_ON_ERROR(httpd_resp_sendstr_chunk(req, "],\"message\":\"Topology: Success\"}"), exit, WEB_TAG,
                      "Failed to send chunk");
    httpd_resp_sendstr_chunk(req, NULL); /* end chunked response */

    ESP_LOGI(WEB_TAG, "<==================== Thread Topology =====================>");
    ESP_LOGI(WEB_TAG, "Thread diagnostic Tlv Complete.");
    ESP_LOGI(WEB_TAG, "<==========================================================>");
exit:
    cJSON_Delete(result);
    return ret;
}

/**
 * @brief The API provides an entry to collect the information of Thread node, packs and sends it to @param req.
 *
 * @param[in] req The request from http_client.
 * @return
 *      -   ESP_OK                      : On success
 *      -   ESP_ERR_HTTPD_RESP_HDR      : Essential headers are too large for internal buffer
 *      -   ESP_ERR_HTTPD_RESP_SEND     : Error in raw send
 *      -   ESP_ERR_HTTPD_INVALID_REQ   : Invalid request
 *      -   ESP_FAILED                  : Null request pointer
 */
static esp_err_t esp_otbr_current_node_get_handler(httpd_req_t *req)
{
    ESP_RETURN_ON_FALSE(req, ESP_FAIL, WEB_TAG, "Failed to parse the node information of http request");
    esp_err_t ret = ESP_OK;
    cJSON *result = handle_ot_resource_node_information_request();
    cJSON *error = result ? cJSON_CreateNumber((double)OT_ERROR_NONE) : cJSON_CreateNumber((double)OT_ERROR_FAILED);
    cJSON *message = result ? cJSON_CreateString("Get Node: Success") : cJSON_CreateString("Get Node: Failure");
    cJSON *response = pack_response(error, result, message);
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);
    ESP_GOTO_ON_FALSE(result, ESP_FAIL, exit, WEB_TAG, "Failed to get current thread node information");
    ESP_LOGI(WEB_TAG, "<=================== Node Information =====================>");
    ESP_LOGI(WEB_TAG, "Extraction Complete");
    ESP_LOGI(WEB_TAG, "<==========================================================>");
exit:
    cJSON_Delete(response);
    return ret;
}

/*-----------------------------------------------------
 Note：Handling for Client request
-----------------------------------------------------*/
static esp_err_t NOT_FOUND_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    cJSON *response = resource_status("404", "404 Not Found");
    ESP_GOTO_ON_ERROR(httpd_send_packet(req, response), exit, WEB_TAG, "Failed to response %s", req->uri);

exit:
    cJSON_Delete(response);
    return ret;
}

/**
 * @brief Provide the favicon for GUI.
 *
 * @param[in] req The request from client's browser.
 * @return
 *      -   ESP_OK                      : On success
 *      -   ESP_ERR_INVALID_ARG         : Null request pointer
 *      -   ESP_ERR_HTTPD_RESP_HDR      : Essential headers are too large for internal buffer
 *      -   ESP_ERR_HTTPD_RESP_SEND     : Error in raw send
 *      -   ESP_ERR_HTTPD_INVALID_REQ   : Invalid request
 */
static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char favicon_ico_end[] asm("_binary_favicon_ico_end");
    const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);

    ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, "image/x-icon"), WEB_TAG, "Failed to set http respond type");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
    ESP_RETURN_ON_ERROR(httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_size), WEB_TAG,
                        "Failed to send http respond");
    return ESP_OK;
}

/**
 * @brief Provide a method to send message to @param req client accord to @param path.
 *
 * @param[in] req  The request from the client
 * @param[in] path The path of file which will be sent.
 * @return
 *      -   ESP_OK: on success
 *      -   ESP_FAIL: on failure
 */
static esp_err_t httpd_resp_send_spiffs_file(httpd_req_t *req, char *path)
{
    // ESP_LOGI(WEB_TAG, "Reading %s", path);

    FILE *fp = fopen(path, "r");
    ESP_RETURN_ON_FALSE(fp, ESP_FAIL, WEB_TAG, "Failed to open %s file", path);

    char buf[FILE_CHUNK_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (httpd_resp_send_chunk(req, buf, bytes_read) != ESP_OK) {
            fclose(fp);
            /* Abort chunked transfer on send error */
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    return fclose(fp) == 0 ? ESP_OK : ESP_FAIL;
}

/**
 * @brief Provide the index.html for GUI,when the client login the web.
 *
 * @param[in] req is the request from client's browser.
 * @param[in] path points to the index.hrml path.
 * @return
 *      -   ESP_OK : On success
 *      -   ESP_ERR_INVALID_ARG : Null request pointer
 *      -   ESP_ERR_HTTPD_RESP_HDR    : Essential headers are too large for internal buffer
 *      -   ESP_ERR_HTTPD_RESP_SEND   : Error in raw send
 *      -   ESP_ERR_HTTPD_INVALID_REQ : Invalid request
 */
static esp_err_t index_html_get_handler(httpd_req_t *req, char *path)
{
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=600");
    ESP_RETURN_ON_ERROR(httpd_resp_send_spiffs_file(req, path), WEB_TAG, "Failed to send index html file");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, NULL), WEB_TAG, "Failed to send http string chunk");
    return ESP_OK;
}

static esp_err_t style_css_get_handler(httpd_req_t *req, char *path)
{
    ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, "text/css"), WEB_TAG, "Failed to set http text/css type");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=600");
    ESP_RETURN_ON_ERROR(httpd_resp_send_spiffs_file(req, path), WEB_TAG, "Failed to send css file");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, NULL), WEB_TAG, "Failed to send http string chunk");
    return ESP_OK;
}

static esp_err_t script_js_get_handler(httpd_req_t *req, char *path)
{
    ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, "application/javascript"), WEB_TAG,
                        "Failed to set http application/javascript type");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=600");
    ESP_RETURN_ON_ERROR(httpd_resp_send_spiffs_file(req, path), WEB_TAG, "Failed to send js file");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, NULL), WEB_TAG, "Failed to send http string chunk");
    return ESP_OK;
}

/**
 * @brief Parse @param parse_url to obtain valid information for returning
 *
 * @param[in] uri       A pointer points the request uri from client
 * @param[in] parse_url A pointer points to the result structure for @function "http_parser_parse_url()".
 * @param[in] base_path A pointer points the base files path.
 * @return the valid information from @param parse_url
 */
static request_url_t parse_request_url_information(const char *uri, const struct http_parser_url *parse_url,
                                                   const char *base_path)
{
    request_url_t ret = {
        .protocol = "http",
        .port = 80,
        .file_name = "",
        .file_path = "",
    };
    ret.port = parse_url->port;
    if ((parse_url->field_set & (1 << UF_SCHEMA)) != 0 && PROTLOCOL_MAX_SIZE > parse_url->field_data[UF_SCHEMA].len) {
        memcpy(ret.protocol, uri + parse_url->field_data[UF_SCHEMA].off, parse_url->field_data[UF_SCHEMA].len);
        ret.protocol[parse_url->field_data[UF_SCHEMA].len] = '\0';
    }

    if ((parse_url->field_set & (1 << UF_PATH)) != 0 && FILENAME_MAX_SIZE > parse_url->field_data[UF_PATH].len) {
        memcpy(ret.file_name, uri + parse_url->field_data[UF_PATH].off, parse_url->field_data[UF_PATH].len);
        ret.file_name[parse_url->field_data[UF_PATH].len] = '\0';
        memcpy(ret.file_path, base_path, strlen(base_path));
        strcat(ret.file_path, ret.file_name);
    }
    return ret;
}

/**
 * @brief Check if a string ends with the given suffix.
 */
static bool str_ends_with(const char *str, const char *suffix)
{
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > str_len) {
        return false;
    }
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

/**
 * @brief Verify and handle the client's default request, return corresponding file to client.
 *        Routes files by extension: .html, .css, .js are served from SPIFFS.
 *
 * @param[in] req The request of http client.
 * @return
 *      -   ESP_OK: on success
 *      -   ESP_FAIL: on failure.
 */
static esp_err_t default_urls_get_handler(httpd_req_t *req)
{
#if CONFIG_OPENTHREAD_BR_SOFTAP_SETUP
    // Check if this is a WiFi config request (when WiFi config mode is active)
    if (esp_br_wifi_config_is_active()) {
        // Let WiFi config server handle it
        return ESP_OK;
    }
#endif

    struct http_parser_url url;
    ESP_RETURN_ON_ERROR(http_parser_parse_url(req->uri, strlen(req->uri), 0, &url), WEB_TAG, "Failed to parse url");
    request_url_t info =
        parse_request_url_information(req->uri, &url, ((http_server_data_t *)req->user_ctx)->base_path);

    // ESP_LOGI(WEB_TAG, "Requested %s", info.file_name);
    if (!strcmp(info.file_name, "")) // check the filename.
    {
        ESP_LOGE(WEB_TAG, "Filename is too long or url error"); /* Respond with 500 Internal Server Error */
        ESP_RETURN_ON_ERROR(
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename is too long or url error"), WEB_TAG,
            "Failed to send error code");
        return ESP_FAIL;
    }

    /* Root path: serve index.html */
    if (strcmp(info.file_name, "/") == 0) {
        char index_path[FILEPATH_MAX_SIZE];
        strcpy(index_path, ((http_server_data_t *)req->user_ctx)->base_path);
        strcat(index_path, "/index.html");
        return index_html_get_handler(req, index_path);
    }

    /* Favicon: served from embedded binary */
    if (strcmp(info.file_name, "/favicon.ico") == 0) {
        return favicon_get_handler(req);
    }

    /* Extension-based routing for SPIFFS files */
    if (str_ends_with(info.file_name, ".html")) {
        return index_html_get_handler(req, info.file_path);
    } else if (str_ends_with(info.file_name, ".css")) {
        return style_css_get_handler(req, info.file_path);
    } else if (str_ends_with(info.file_name, ".js")) {
        return script_js_get_handler(req, info.file_path);
    }

    ESP_LOGE(WEB_TAG, "Failed to stat file : %s", info.file_path); /* Respond with 404 Not Found */
    return NOT_FOUND_handler(req);
}

#if CONFIG_SPIRAM
static void *ot_web_json_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void ot_web_json_free(void *ptr)
{
    heap_caps_free(ptr);
}
#endif

/*-----------------------------------------------------
 Note：Server Start
-----------------------------------------------------*/
/**
 * @brief Create an HTTP server and register an accessible URI
 *
 * @param[in] base_path A string represents the basic path of http_server files.
 * @param[in] host_ip   A IPvr4 address provided by the connected wifi, recorded by s_server.ip
 * @return
 *      -   ESP_OK: on success
 *      -   ESP_FAIL: on failure
 */
static httpd_handle_t *start_esp_br_http_server(const char *base_path, const char *host_ip)
{
    ESP_RETURN_ON_FALSE(base_path, NULL, WEB_TAG, "Invalid http server path");

    ensure_log_hook_installed();
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);

    if (!s_ota_mutex) {
        s_ota_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_ota_mutex, NULL, WEB_TAG, "Failed to create OTA mutex");
    }

#if CONFIG_SPIRAM
    cJSON_Hooks hooks;
    hooks.malloc_fn = ot_web_json_malloc;
    hooks.free_fn = ot_web_json_free;
    cJSON_InitHooks(&hooks);
#endif

    strcpy(s_server.ip, host_ip);
    strlcpy(s_server.data.base_path, base_path, ESP_VFS_PATH_MAX + 1);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = (sizeof(s_resource_handlers) + sizeof(s_web_gui_handlers)) / sizeof(httpd_uri_t) + 2;
    config.max_resp_headers = (sizeof(s_resource_handlers) + sizeof(s_web_gui_handlers)) / sizeof(httpd_uri_t) + 2;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8 * 1024;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    s_server.port = config.server_port;

    esp_br_web_api_init();

    // start http_server
    ESP_RETURN_ON_FALSE(!httpd_start(&s_server.handle, &config), NULL, WEB_TAG, "Failed to start web server");

    httpd_uri_t default_uris_get = {.uri = "/*", // Match all URIs of type /path/to/file
                                    .method = HTTP_GET,
                                    .handler = default_urls_get_handler,
                                    .user_ctx = &s_server.data};

    httpd_server_register_http_uri(&s_server, s_resource_handlers, sizeof(s_resource_handlers) / sizeof(httpd_uri_t));
    httpd_server_register_http_uri(&s_server, s_web_gui_handlers, sizeof(s_web_gui_handlers) / sizeof(httpd_uri_t));
    httpd_register_uri_handler(s_server.handle, &default_uris_get);

    // Show the login address in the console
    ESP_LOGI(WEB_TAG, "%s\r\n", "<========server start========>");
    ESP_LOGI(WEB_TAG, "http://%s\r\n", s_server.ip);
    ESP_LOGI(WEB_TAG, "%s\r\n", "<============================>");

    return s_server.handle;
}

void connect_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data, const char *base_path)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    ESP_RETURN_ON_FALSE(server, , WEB_TAG, "Http server is invalid, failed to start it");
    ESP_LOGI(WEB_TAG, "Start the web server for Openthread Border Router");
    *server = (httpd_handle_t *)start_esp_br_http_server(base_path, s_server.ip);
}

/*-----------------------------------------------------
 Note：Server Stop
-----------------------------------------------------*/
void stop_httpserver(httpd_handle_t server)
{
    httpd_stop(server); // Stop the httpd server
}

void disconnect_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    http_server_data_t *data = (http_server_data_t *)event_data;
    if (data) {
        free(data);
        data = NULL;
    }
    httpd_handle_t *server = (httpd_handle_t *)arg;
    ESP_RETURN_ON_FALSE(server, , WEB_TAG, "Web server is valid, failed to stop it");
    ESP_LOGI(WEB_TAG, "Stop the web server for Openthread Border Router");
    stop_httpserver(*server);
    *server = NULL;
}

static bool is_br_web_server_started = false;
static void handler_got_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
#if CONFIG_OPENTHREAD_BR_SOFTAP_SETUP
    // Don't start Thread BR web server if WiFi config mode is active
    if (esp_br_wifi_config_is_active()) {
        ESP_LOGI(WEB_TAG, "WiFi config mode is active, skipping Thread BR web server");
        return;
    }
#endif

    if (!is_br_web_server_started) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ipv4_address[SERVER_IPV4_LEN];
        sprintf((char *)ipv4_address, IPSTR, IP2STR(&event->ip_info.ip));
        if (start_esp_br_http_server((const char *)arg, (char *)ipv4_address) != NULL) {
            is_br_web_server_started = true;
        } else {
            ESP_LOGE(WEB_TAG, "Fail to start web server");
        }
    } else {
        ESP_LOGW(WEB_TAG, "Web server had already been started");
    }
}

void esp_br_web_start(char *base_path)
{
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &handler_got_ip_event, base_path));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &handler_got_ip_event, base_path));
}
