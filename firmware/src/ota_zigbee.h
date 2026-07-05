#pragma once

#include "esp_err.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_cluster.h"

#define OTA_ENDPOINT_ID          2
#define OTA_MANUFACTURER_CODE    0x1337
#define OTA_IMAGE_TYPE           0x0001
#define OTA_FILE_VERSION         0x00010000
#define FW_VERSION_STRING        "\x05""1.0.0"
#define OTA_HW_VERSION           0x0001
#define OTA_MAX_DATA_SIZE        64

esp_err_t ota_zigbee_register_endpoint(esp_zb_ep_list_t *ep_list);
esp_err_t ota_zigbee_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                    const void *message);
