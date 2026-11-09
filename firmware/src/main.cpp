// mitsubishi2zigbee - Zigbee gateway for Mitsubishi heat pumps (CN105)
//
// Based on gysmo38/mitsubishi2MQTT; exposes Thermostat + Fan Control clusters
// on ESP32-C6 (ESP-IDF) instead of MQTT.
//
//   Home Assistant ──Zigbee──► ESP32-C6 ──UART CN105──► Mitsubishi unit
//
// Status LED (WS2812, GPIO8): fast blue = scanning | slow blue = found, not paired | green = paired
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "led_strip.h"
#include "esp_timer.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_attribute.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "bdb/esp_zigbee_bdb_commissioning.h"
#include "zcl/esp_zigbee_zcl_command.h"

#include "cn105.h"
#include "device_manifest.h"
#include "ota_zigbee.h"
#include <string.h>

static const char *TAG = "MITSU_ZB";

#define LED_PIN     8
#define LED_BRIGHT  20

typedef enum {
    LED_SEARCHING,
    LED_FOUND,
    LED_CONNECTED,
} led_state_t;

static volatile led_state_t g_led_state = LED_SEARCHING;
static led_strip_handle_t   g_led;

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(g_led, 0, r, g, b);
    led_strip_refresh(g_led);
}
static void led_off(void) { led_strip_clear(g_led); }

static volatile bool g_cn105_linked = false;

static void led_task(void *arg)
{
    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = LED_PIN;
    strip_cfg.max_leds = 1;
    strip_cfg.led_model = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB;

    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &g_led));

    led_set(LED_BRIGHT, LED_BRIGHT, LED_BRIGHT);
    vTaskDelay(pdMS_TO_TICKS(300));
    led_off();

    while (true) {
        switch (g_led_state) {
        case LED_SEARCHING:
            led_set(0, 0, LED_BRIGHT); vTaskDelay(pdMS_TO_TICKS(150));
            led_off();                 vTaskDelay(pdMS_TO_TICKS(150));
            break;
        case LED_FOUND:
            led_set(0, 0, LED_BRIGHT); vTaskDelay(pdMS_TO_TICKS(800));
            led_off();                 vTaskDelay(pdMS_TO_TICKS(800));
            break;
        case LED_CONNECTED:
            if (g_cn105_linked) {
                led_set(0, LED_BRIGHT, 0);
                vTaskDelay(pdMS_TO_TICKS(900));
                led_off();
                vTaskDelay(pdMS_TO_TICKS(100));
            } else {
                /* Zigbee OK, CN105 muet → jaune */
                led_set(LED_BRIGHT, LED_BRIGHT, 0);
                vTaskDelay(pdMS_TO_TICKS(200));
                led_off();
                vTaskDelay(pdMS_TO_TICKS(800));
            }
            break;
        }
    }
}

#define CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

static hp_state_t g_hp = {};
static uint8_t    g_vane_attr = 0;
static volatile bool g_syncing_zigbee = false;

typedef enum {
    HP_CMD_SYSTEM_MODE,
    HP_CMD_SETPOINT,
    HP_CMD_FAN,
    HP_CMD_VANE,
} hp_cmd_kind_t;

typedef struct {
    hp_cmd_kind_t kind;
    uint8_t         u8;
    int16_t         i16;
} hp_cmd_t;

static QueueHandle_t g_hp_cmd_q;

typedef struct {
    uint8_t     zcl;
    bool        power;
    const char *mode;
} zcl_mode_map_t;

static const zcl_mode_map_t ZCL_MODES[] = {
    {ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF,      false, "HEAT"},
    {ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_HEAT,     true,  "HEAT"},
    {ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_COOL,     true,  "COOL"},
    {ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_AUTO,     true,  "AUTO"},
    {ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_FAN_ONLY, true,  "FAN"},
    {ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_DRY,      true,  "DRY"},
};

typedef struct {
    uint8_t     zcl;
    const char *fan;
} zcl_fan_map_t;

static const zcl_fan_map_t ZCL_FANS[] = {
    {ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_LOW,    "1"},
    {ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_MEDIUM, "3"},
    {ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_HIGH,   "4"},
    {ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_ON,     "4"},
    {ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_SMART,  "QUIET"},
    {ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_AUTO,   "AUTO"},
};

static void apply_zcl_mode(uint8_t zcl)
{
    for (size_t i = 0; i < sizeof(ZCL_MODES) / sizeof(ZCL_MODES[0]); i++) {
        if (ZCL_MODES[i].zcl != zcl) continue;
        if (!ZCL_MODES[i].power) cn105_set_power(false);
        else cn105_set_system_mode(true, ZCL_MODES[i].mode);
        return;
    }
    ESP_LOGW(TAG, "unsupported system_mode 0x%02x", zcl);
}

static void apply_zcl_fan(uint8_t zcl)
{
    for (size_t i = 0; i < sizeof(ZCL_FANS) / sizeof(ZCL_FANS[0]); i++) {
        if (ZCL_FANS[i].zcl == zcl) {
            cn105_set_fan(ZCL_FANS[i].fan);
            return;
        }
    }
    cn105_set_fan("AUTO");
}

static uint8_t zcl_mode_from_hp(const hp_state_t *st)
{
    if (!st->power_on) return ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF;
    for (size_t i = 1; i < sizeof(ZCL_MODES) / sizeof(ZCL_MODES[0]); i++) {
        if (strcmp(st->mode, ZCL_MODES[i].mode) == 0) return ZCL_MODES[i].zcl;
    }
    return ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_AUTO;
}

static uint8_t zcl_fan_from_hp(const hp_state_t *st)
{
    if (strcmp(st->fan, "2") == 0) return ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_LOW;
    for (size_t i = 0; i < sizeof(ZCL_FANS) / sizeof(ZCL_FANS[0]); i++) {
        if (strcmp(st->fan, ZCL_FANS[i].fan) == 0) return ZCL_FANS[i].zcl;
    }
    return ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_AUTO;
}

static void hp_queue_cmd(hp_cmd_kind_t kind, uint8_t u8, int16_t i16)
{
    if (!g_hp_cmd_q) return;
    hp_cmd_t cmd = { .kind = kind, .u8 = u8, .i16 = i16 };
    if (xQueueSend(g_hp_cmd_q, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "hp cmd queue pleine - commande perdue kind=%d", (int)kind);
    }
}

static void push_state_to_zigbee(const hp_state_t *st)
{
    int16_t local_temp = (int16_t)(st->room_temp * 100.0f);
    int16_t setpoint   = (int16_t)(st->target_temp * 100.0f);
    uint8_t sys_mode   = zcl_mode_from_hp(st);
    uint8_t fan_mode   = zcl_fan_from_hp(st);
    const bool cmd_pending = cn105_cmd_pending();
    const hp_state_t *w = cn105_wanted_state();
    const bool vane_cn105_synced = strcmp(w->vane, st->vane) == 0;

    g_syncing_zigbee = true;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(HP_ENDPOINT_ID,
        ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_ID, &local_temp, false);
    if (!cmd_pending) {
        esp_zb_zcl_set_attribute_val(HP_ENDPOINT_ID,
            ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID, &setpoint, false);
        esp_zb_zcl_set_attribute_val(HP_ENDPOINT_ID,
            ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID, &setpoint, false);
        esp_zb_zcl_set_attribute_val(HP_ENDPOINT_ID,
            ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID, &sys_mode, false);
        esp_zb_zcl_set_attribute_val(HP_ENDPOINT_ID,
            ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID, &fan_mode, false);
        /* Ne pas écraser vane ZCL tant que CN105 n'a pas appliqué (sinon write HA perdu) */
        if (vane_cn105_synced) {
            g_vane_attr = cn105_vane_index(st->vane);
            esp_zb_zcl_set_attribute_val(HP_ENDPOINT_ID,
                MITSU_EXT_CLUSTER_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                VANE_ATTR_ID, &g_vane_attr, false);
        }
    }
    esp_zb_lock_release();
    g_syncing_zigbee = false;
}

static void hp_drain_cmd_queue(void)
{
    hp_cmd_t mode = {}, setpt = {}, fan = {}, vane = {};
    bool have_mode = false, have_setpt = false, have_fan = false, have_vane = false;
    hp_cmd_t cmd;

    while (g_hp_cmd_q && xQueueReceive(g_hp_cmd_q, &cmd, 0) == pdTRUE) {
        switch (cmd.kind) {
        case HP_CMD_SYSTEM_MODE: mode = cmd; have_mode = true; break;
        case HP_CMD_SETPOINT:    setpt = cmd; have_setpt = true; break;
        case HP_CMD_FAN:         fan = cmd; have_fan = true; break;
        case HP_CMD_VANE:        vane = cmd; have_vane = true; break;
        }
    }
    if (have_mode) apply_zcl_mode(mode.u8);
    if (have_setpt) cn105_set_temp(setpt.i16 / 100.0f);
    if (have_fan) apply_zcl_fan(fan.u8);
    if (have_vane && vane.u8 < CN105_VANE_COUNT) {
        g_vane_attr = vane.u8;
        cn105_set_vane(cn105_vane_name(vane.u8));
    }
}

static signed int mitsu_ext_check_value(uint16_t attr_id, uint8_t endpoint, uint8_t *value)
{
    (void)endpoint;
    if (attr_id == VANE_ATTR_ID && value && *value >= CN105_VANE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void mitsu_ext_write_attr(uint8_t endpoint, uint16_t attr_id, uint8_t *new_value, uint16_t manuf_code)
{
    (void)endpoint;
    (void)manuf_code;
    if (g_syncing_zigbee || !new_value) return;
    if (attr_id == VANE_ATTR_ID && *new_value < CN105_VANE_COUNT) {
        g_vane_attr = *new_value;
        hp_queue_cmd(HP_CMD_VANE, *new_value, 0);
    }
}

static void register_mitsu_ext_handlers(void)
{
    esp_zb_zcl_custom_cluster_handlers_t h = {
        .cluster_id = MITSU_EXT_CLUSTER_ID,
        .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        .check_value_cb = mitsu_ext_check_value,
        .write_attr_cb = mitsu_ext_write_attr,
    };
    ESP_ERROR_CHECK(esp_zb_zcl_custom_cluster_handlers_update(h));
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                   const void *message)
{
    if (callback_id == ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID ||
        callback_id == ESP_ZB_CORE_OTA_UPGRADE_QUERY_IMAGE_RESP_CB_ID) {
        return ota_zigbee_action_handler(callback_id, message);
    }
    if (callback_id != ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) return ESP_OK;

    const esp_zb_zcl_set_attr_value_message_t *m =
        (const esp_zb_zcl_set_attr_value_message_t *)message;
    if (m->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) return ESP_OK;
    if (m->info.dst_endpoint != HP_ENDPOINT_ID) return ESP_OK;

    uint16_t cluster = m->info.cluster;
    uint16_t attr    = m->attribute.id;
    void    *val     = m->attribute.data.value;

    if (cluster != ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT &&
        cluster != ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL) {
        return ESP_OK;
    }

    if (g_syncing_zigbee) {
        const hp_state_t *w = cn105_wanted_state();
        if (cluster == ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL &&
            attr == ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID) {
            if (*(uint8_t *)val == zcl_fan_from_hp(w)) return ESP_OK;
        } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT &&
                   attr == ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID) {
            if (*(uint8_t *)val == zcl_mode_from_hp(w)) return ESP_OK;
        } else {
            return ESP_OK;
        }
    }

    if (cluster == ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT) {
        if (attr == ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID) {
            hp_queue_cmd(HP_CMD_SYSTEM_MODE, *(uint8_t *)val, 0);
        } else if (attr == ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID ||
                   attr == ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID) {
            hp_queue_cmd(HP_CMD_SETPOINT, 0, *(int16_t *)val);
        }
    } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL) {
        if (attr == ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID) {
            hp_queue_cmd(HP_CMD_FAN, *(uint8_t *)val, 0);
        }
    }
    return ESP_OK;
}

static void bdb_start_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(
        esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK,
        , TAG, "commissioning start failed");
}

extern "C" void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p     = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = (esp_zb_app_signal_type_t)*p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            g_led_state = LED_SEARCHING;
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGW(TAG, "stack init failed: %s", esp_err_to_name(err_status));
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING: {
        uint16_t addr = esp_zb_get_short_address();
        bool in_network = (addr != 0xFFFE && addr != 0xFFFF);
        if (in_network) {
            g_led_state = LED_CONNECTED;
            esp_zb_bdb_finding_binding_start_target(HP_ENDPOINT_ID, 300);
        } else {
            g_led_state = LED_FOUND;
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    }

    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        g_led_state = LED_CONNECTED;
        break;

    default:
        break;
    }
}

static esp_zb_cluster_list_t *create_hvac_cluster_list(void)
{
    esp_zb_cluster_list_t *list = esp_zb_zcl_cluster_list_create();

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x01,
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)ZB_MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)ZB_MODEL_ID);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, (void *)FW_VERSION_STRING);
    esp_zb_cluster_list_add_basic_cluster(list, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_identify_cluster_cfg_t id_cfg = { .identify_time = 0 };
    esp_zb_cluster_list_add_identify_cluster(list,
        esp_zb_identify_cluster_create(&id_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_thermostat_cluster_cfg_t th_cfg = {
        .local_temperature             = HP_SETPOINT_DEFAULT,
        .occupied_cooling_setpoint     = HP_SETPOINT_DEFAULT,
        .occupied_heating_setpoint     = HP_SETPOINT_DEFAULT,
        .control_sequence_of_operation =
            ESP_ZB_ZCL_THERMOSTAT_CONTROL_SEQ_OF_OPERATION_COOLING_AND_HEATING_4_PIPES,
        .system_mode                   = ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF,
    };
    esp_zb_attribute_list_t *th = esp_zb_thermostat_cluster_create(&th_cfg);
    int16_t lim_min = HP_SETPOINT_MIN, lim_max = HP_SETPOINT_MAX;
    esp_zb_thermostat_cluster_add_attr(th,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_MIN_HEAT_SETPOINT_LIMIT_ID, &lim_min);
    esp_zb_thermostat_cluster_add_attr(th,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_MAX_HEAT_SETPOINT_LIMIT_ID, &lim_max);
    esp_zb_thermostat_cluster_add_attr(th,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_MIN_COOL_SETPOINT_LIMIT_ID, &lim_min);
    esp_zb_thermostat_cluster_add_attr(th,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_MAX_COOL_SETPOINT_LIMIT_ID, &lim_max);
    esp_zb_cluster_list_add_thermostat_cluster(list, th, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_attribute_list_t *mitsu_ext = esp_zb_zcl_attr_list_create(MITSU_EXT_CLUSTER_ID);
    ESP_ERROR_CHECK(esp_zb_custom_cluster_add_custom_attr(mitsu_ext, VANE_ATTR_ID,
        ESP_ZB_ZCL_ATTR_TYPE_U8, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &g_vane_attr));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(list, mitsu_ext, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    esp_zb_fan_control_cluster_cfg_t fan_cfg = {
        .fan_mode          = ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_AUTO,
        .fan_mode_sequence = ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_SEQUENCE_LOW_MED_HIGH_AUTO,
    };
    esp_zb_cluster_list_add_fan_control_cluster(list,
        esp_zb_fan_control_cluster_create(&fan_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    return list;
}

static void hp_sync_task(void *arg)
{
    cn105_init();
    int64_t last_connect_ms = -3000;
    int64_t last_warn_ms = 0;
    uint8_t cycle = 0;

    while (true) {
        int64_t now = esp_timer_get_time() / 1000;

        hp_drain_cmd_queue();

        bool changed = cn105_poll(&g_hp);
        while (cn105_poll(&g_hp)) changed = true;

        if (!g_hp.connected) {
            if (now - last_connect_ms >= 3000) {
                cn105_connect();
                cn105_rotate_baud_if_silent();
                last_connect_ms = now;
            }
            if (now - last_warn_ms >= 30000) {
                ESP_LOGW(TAG, "CN105 silent - check wiring / level shifter");
                last_warn_ms = now;
            }
        } else {
            if (!cn105_request_settings_if_needed() && !cn105_flush_pending(&g_hp)) {
                if (cycle % 2 == 0) cn105_request_settings();
                else                  cn105_request_room_temp();
                cycle++;
            }
        }

        g_cn105_linked = g_hp.connected;
        if (changed) push_state_to_zigbee(&g_hp);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void esp_zb_task(void *pvParameters)
{
    esp_zb_aps_src_binding_table_size_set(32);
    esp_zb_aps_dst_binding_table_size_set(32);

    esp_zb_cfg_t zb_nwk_cfg = {};
    zb_nwk_cfg.esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER;
    zb_nwk_cfg.install_code_policy = false;
    zb_nwk_cfg.nwk_cfg.zczr_cfg.max_children = 10;
    esp_zb_init(&zb_nwk_cfg);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint           = HP_ENDPOINT_ID,
        .app_profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id      = ESP_ZB_HA_THERMOSTAT_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, create_hvac_cluster_list(), ep_cfg);
    ESP_ERROR_CHECK(ota_zigbee_register_endpoint(ep_list));
    register_mitsu_ext_handlers();
    esp_zb_device_register(ep_list);

    esp_zb_core_action_handler_register(zb_action_handler);

    esp_zb_set_primary_network_channel_set(CHANNEL_MASK);
    esp_zb_secur_network_min_join_lqi_set(0);

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    g_hp_cmd_q = xQueueCreate(16, sizeof(hp_cmd_t));

    esp_zb_platform_config_t config = {};
    config.radio_config.radio_mode = ZB_RADIO_MODE_NATIVE;
    config.host_config.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE;
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    xTaskCreate(led_task,     "led",     4096, NULL, 3, NULL);
    xTaskCreate(hp_sync_task, "hp_sync", 4096, NULL, 4, NULL);
    xTaskCreate(esp_zb_task,  "esp_zb",  8192, NULL, 5, NULL);
}
