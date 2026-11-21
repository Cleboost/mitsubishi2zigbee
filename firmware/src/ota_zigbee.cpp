#include "device_manifest.h"
#include "ota_zigbee.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_zigbee_ota.h"

static const char *TAG = "OTA_ZB";

static const esp_partition_t *s_ota_partition = nullptr;
static esp_ota_handle_t s_ota_handle = 0;
static bool s_tag_received = false;
static uint16_t s_tag_id = 0;
static uint32_t s_total_size = 0;
static uint32_t s_offset = 0;

#define OTA_ELEMENT_HEADER_LEN 6

typedef enum {
    UPGRADE_IMAGE = 0x0000,
} ota_element_tag_id_t;

static void ota_reset_state(bool abort_handle)
{
    if (abort_handle && s_ota_handle != 0) {
        esp_ota_abort(s_ota_handle);
    }
    s_ota_handle = 0;
    s_ota_partition = nullptr;
    s_tag_received = false;
    s_tag_id = 0;
    s_total_size = 0;
    s_offset = 0;
}

static void ota_reboot_cb(uint8_t param)
{
    (void)param;
    ESP_LOGW(TAG, "OTA rebooting now");
    esp_restart();
}

static esp_err_t element_ota_data(uint32_t total_size, const void *payload,
                                  uint16_t payload_size, void **outbuf,
                                  uint16_t *outlen)
{
    const uint8_t *data = (const uint8_t *)payload;

    if (!s_tag_received) {
        if (!payload || payload_size <= OTA_ELEMENT_HEADER_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
        uint32_t length = 0;
        memcpy(&s_tag_id, data, sizeof(s_tag_id));
        memcpy(&length, data + sizeof(s_tag_id), sizeof(length));
        if (s_tag_id != UPGRADE_IMAGE) {
            ESP_LOGE(TAG, "unsupported OTA tag 0x%04x", s_tag_id);
            return ESP_ERR_INVALID_ARG;
        }
        if ((length + OTA_ELEMENT_HEADER_LEN) != total_size) {
            ESP_LOGE(TAG, "bad element length %lu (total %lu)",
                     (unsigned long)length, (unsigned long)total_size);
            return ESP_ERR_INVALID_ARG;
        }
        s_tag_received = true;
        *outbuf = (void *)(data + OTA_ELEMENT_HEADER_LEN);
        *outlen = payload_size - OTA_ELEMENT_HEADER_LEN;
        return ESP_OK;
    }

    if (s_tag_id != UPGRADE_IMAGE) {
        return ESP_ERR_INVALID_ARG;
    }
    *outbuf = (void *)data;
    *outlen = payload_size;
    return ESP_OK;
}

static esp_err_t ota_status_handler(esp_zb_zcl_ota_upgrade_value_message_t message)
{
    esp_err_t ret = ESP_OK;

    if (message.info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "OTA message bad status 0x%02x (upgrade_status=%d)",
                 message.info.status, (int)message.upgrade_status);
        ota_reset_state(true);
        return ESP_FAIL;
    }

    switch (message.upgrade_status) {
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
        ota_reset_state(true);
        s_ota_partition = esp_ota_get_next_update_partition(nullptr);
        if (!s_ota_partition) {
            ESP_LOGE(TAG, "no OTA partition");
            return ESP_FAIL;
        }
        ret = esp_ota_begin(s_ota_partition, OTA_SIZE_UNKNOWN, &s_ota_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
            ota_reset_state(false);
            return ret;
        }
        ESP_LOGI(TAG, "OTA start → partition %s", s_ota_partition->label);
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE:
        if (s_ota_handle == 0) {
            ESP_LOGE(TAG, "OTA receive without active handle");
            return ESP_ERR_INVALID_STATE;
        }
        s_total_size = message.ota_header.image_size;
        s_offset += message.payload_size;
        if (message.payload_size && message.payload) {
            void *payload = nullptr;
            uint16_t payload_size = 0;
            ret = element_ota_data(s_total_size, message.payload, message.payload_size,
                                   &payload, &payload_size);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "element parse failed: %s", esp_err_to_name(ret));
                ota_reset_state(true);
                return ret;
            }
            ret = esp_ota_write(s_ota_handle, payload, payload_size);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
                ota_reset_state(true);
                return ret;
            }
        }
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK:
        ret = (s_total_size > 0 && s_offset == s_total_size) ? ESP_OK : ESP_FAIL;
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "OTA size mismatch: %lu / %lu",
                     (unsigned long)s_offset, (unsigned long)s_total_size);
            ota_reset_state(true);
        }
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
        if (s_ota_handle == 0 || !s_ota_partition) {
            ESP_LOGE(TAG, "OTA finish without active session");
            ota_reset_state(true);
            return ESP_ERR_INVALID_STATE;
        }
        ret = esp_ota_end(s_ota_handle);
        s_ota_handle = 0;
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(ret));
            ota_reset_state(false);
            return ret;
        }
        ret = esp_ota_set_boot_partition(s_ota_partition);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(ret));
            ota_reset_state(false);
            return ret;
        }
        ota_reset_state(false);
        ESP_LOGW(TAG, "OTA complete — rebooting in 1s");
        esp_zb_scheduler_alarm((esp_zb_callback_t)ota_reboot_cb, 0, 1000);
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ABORT:
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_SERVER_NOT_FOUND:
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR:
        ESP_LOGW(TAG, "OTA aborted/error (status=%d)", (int)message.upgrade_status);
        ota_reset_state(true);
        break;

    default:
        break;
    }

    return ret;
}

esp_err_t ota_zigbee_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                    const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID:
        return ota_status_handler(*(const esp_zb_zcl_ota_upgrade_value_message_t *)message);
    case ESP_ZB_CORE_OTA_UPGRADE_QUERY_IMAGE_RESP_CB_ID:
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t ota_zigbee_add_client_cluster(esp_zb_cluster_list_t *list)
{
    esp_zb_ota_cluster_cfg_t ota_cfg = {};
    ota_cfg.ota_upgrade_file_version = OTA_FILE_VERSION;
    ota_cfg.ota_upgrade_manufacturer = OTA_MANUFACTURER_CODE;
    ota_cfg.ota_upgrade_image_type = OTA_IMAGE_TYPE;
    ota_cfg.ota_upgrade_downloaded_file_ver = 0;

    esp_zb_zcl_ota_upgrade_client_variable_t client_var = {
        .timer_query = ESP_ZB_ZCL_OTA_UPGRADE_QUERY_TIMER_COUNT_DEF,
        .hw_version = OTA_HW_VERSION,
        .max_data_size = OTA_MAX_DATA_SIZE,
    };

    uint16_t server_addr = 0xffff;
    uint8_t server_ep = 0xff;

    esp_zb_attribute_list_t *ota = esp_zb_ota_cluster_create(&ota_cfg);
    ESP_RETURN_ON_FALSE(ota, ESP_ERR_NO_MEM, TAG, "OTA cluster create failed");

    ESP_RETURN_ON_ERROR(
        esp_zb_ota_cluster_add_attr(ota, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_CLIENT_DATA_ID,
                                    &client_var),
        TAG, "client data attr failed");
    ESP_RETURN_ON_ERROR(
        esp_zb_ota_cluster_add_attr(ota, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ADDR_ID,
                                    &server_addr),
        TAG, "server addr attr failed");
    ESP_RETURN_ON_ERROR(
        esp_zb_ota_cluster_add_attr(ota, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ENDPOINT_ID,
                                    &server_ep),
        TAG, "server ep attr failed");

    return esp_zb_cluster_list_add_ota_cluster(list, ota, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
}

static esp_zb_cluster_list_t *create_ota_cluster_list(void)
{
    static const char *manuf = ZB_MANUFACTURER_NAME;
    static const char *model = ZB_MODEL_ID;

    esp_zb_cluster_list_t *list = esp_zb_zcl_cluster_list_create();

    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(nullptr);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)manuf);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)model);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, (void *)FW_VERSION_STRING);
    esp_zb_cluster_list_add_basic_cluster(list, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    ESP_ERROR_CHECK(ota_zigbee_add_client_cluster(list));
    return list;
}

esp_err_t ota_zigbee_register_endpoint(esp_zb_ep_list_t *ep_list)
{
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint           = OTA_ENDPOINT_ID,
        .app_profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id      = ESP_ZB_HA_TEST_DEVICE_ID,
        .app_device_version = 0,
    };
    return esp_zb_ep_list_add_ep(ep_list, create_ota_cluster_list(), ep_cfg);
}
