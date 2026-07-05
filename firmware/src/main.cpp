// ─────────────────────────────────────────────────────────────────────────────
// mitsubishi2zigbee — Passerelle Zigbee pour climatiseur Mitsubishi (CN105)
//
// Inspiré de gysmo38/mitsubishi2MQTT, mais expose l'unité en Zigbee (cluster
// Thermostat + Fan Control) au lieu de MQTT. Tourne sur ESP32-C6 en ESP-IDF.
//
//   Home Assistant  ──Zigbee──►  ESP32-C6  ──UART CN105──►  Unité Mitsubishi
//
// LED de statut (WS2812, GPIO8) :
//   bleu rapide = recherche réseau | bleu lent = trouvé, pas appairé
//   vert        = appairé au réseau Zigbee
// ─────────────────────────────────────────────────────────────────────────────
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "led_strip.h"
#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"

#include "cn105.h"

static const char *TAG = "MITSU_ZB";

// ─── LED de statut (WS2812, ordre RGB sur le Waveshare C6-Zero) ──────────────

#define LED_PIN     8
#define LED_BRIGHT  20

typedef enum {
    LED_SEARCHING,  // bleu rapide : scan réseau
    LED_FOUND,      // bleu lent   : réseau trouvé, pas encore appairé
    LED_CONNECTED,  // vert        : appairé
} led_state_t;

static volatile led_state_t g_led_state = LED_SEARCHING;
static led_strip_handle_t   g_led;

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(g_led, 0, r, g, b);
    led_strip_refresh(g_led);
}
static void led_off(void) { led_strip_clear(g_led); }

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

    led_set(LED_BRIGHT, LED_BRIGHT, LED_BRIGHT);   // test blanc 1s
    vTaskDelay(pdMS_TO_TICKS(1000));
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
            led_set(0, LED_BRIGHT, 0); vTaskDelay(pdMS_TO_TICKS(900));
            led_off();                 vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
    }
}

// ─── Définitions Zigbee ─────────────────────────────────────────────────────

#define ENDPOINT_ID       1
#define CHANNEL_MASK      ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

#define MANUFACTURER_NAME "\x0A""Mitsubishi"    // 10 chars
#define MODEL_ID          "\x0C""MZ-Zigbee-C6"  // 12 chars

// Consignes : int16 en centi-degrés (21°C = 2100)
#define SETPOINT_DEFAULT  2100
#define SETPOINT_MIN      1600   // 16°C
#define SETPOINT_MAX      3100   // 31°C

// Attribut personnalisé pour la position des pales (dans le cluster thermostat)
// 0=AUTO  1=pos1  2=pos2  3=pos3  4=pos4  5=pos5  6=SWING
#define VANE_ATTR_ID  0x0400

// État partagé de l'unité (mis à jour par cn105_poll)
static hp_state_t g_hp = {};
static uint8_t    g_vane_attr = 0;  // valeur persistante pour l'attribut ZCL

// ─── Conversion Zigbee ↔ Mitsubishi ─────────────────────────────────────────

// system_mode Zigbee → action sur l'unité
static void apply_system_mode(uint8_t sys_mode)
{
    switch (sys_mode) {
    case ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF:
        cn105_set_power(false);
        break;
    case ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_HEAT:
        cn105_set_power(true); cn105_set_mode("HEAT");
        break;
    case ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_COOL:
        cn105_set_power(true); cn105_set_mode("COOL");
        break;
    case ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_AUTO:
        cn105_set_power(true); cn105_set_mode("AUTO");
        break;
    case ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_FAN_ONLY:
        cn105_set_power(true); cn105_set_mode("FAN");
        break;
    case ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_DRY:
        cn105_set_power(true); cn105_set_mode("DRY");
        break;
    default:
        ESP_LOGW(TAG, "system_mode 0x%02x non géré", sys_mode);
        break;
    }
}

// vane Zigbee (index 0-6) → inclinaison Mitsubishi
static const char *const VANE_NAMES[] = {"AUTO","1","2","3","4","5","SWING"};
#define VANE_COUNT 7

static void apply_vane(uint8_t idx)
{
    if (idx < VANE_COUNT) cn105_set_vane(VANE_NAMES[idx]);
}

static uint8_t hp_to_vane_idx(const hp_state_t *st)
{
    for (int i = 0; i < VANE_COUNT; i++)
        if (strcmp(st->vane, VANE_NAMES[i]) == 0) return (uint8_t)i;
    return 0;
}

// fan_mode Zigbee → vitesse Mitsubishi
static void apply_fan_mode(uint8_t fan_mode)
{
    switch (fan_mode) {
    case ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_LOW:    cn105_set_fan("1");     break;
    case ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_MEDIUM: cn105_set_fan("2");     break;
    case ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_HIGH:   cn105_set_fan("4");     break;
    case ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_ON:     cn105_set_fan("4");     break;
    case ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_SMART:  cn105_set_fan("QUIET"); break;
    case ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_AUTO:
    default:                                     cn105_set_fan("AUTO");  break;
    }
}

// État Mitsubishi → system_mode Zigbee (pour le retour d'information)
static uint8_t hp_to_system_mode(const hp_state_t *st)
{
    if (!st->power_on)               return ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF;
    if (strcmp(st->mode, "HEAT")==0) return ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_HEAT;
    if (strcmp(st->mode, "COOL")==0) return ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_COOL;
    if (strcmp(st->mode, "DRY")==0)  return ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_DRY;
    if (strcmp(st->mode, "FAN")==0)  return ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_FAN_ONLY;
    return ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_AUTO;
}

static uint8_t hp_to_fan_mode(const hp_state_t *st)
{
    if (strcmp(st->fan, "QUIET")==0) return ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_SMART;
    if (strcmp(st->fan, "1")==0)     return ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_LOW;
    if (strcmp(st->fan, "2")==0)     return ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_MEDIUM;
    if (strcmp(st->fan, "3")==0)     return ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_HIGH;
    if (strcmp(st->fan, "4")==0)     return ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_HIGH;
    return ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_AUTO;
}

// Pushes current state of the unit to Zigbee attributes.
static void push_state_to_zigbee(const hp_state_t *st)
{
    int16_t local_temp = (int16_t)(st->room_temp * 100.0f);
    int16_t setpoint   = (int16_t)(st->target_temp * 100.0f);
    uint8_t sys_mode   = hp_to_system_mode(st);
    uint8_t fan_mode   = hp_to_fan_mode(st);

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(ENDPOINT_ID,
        ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_ID, &local_temp, false);
    esp_zb_zcl_set_attribute_val(ENDPOINT_ID,
        ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID, &setpoint, false);
    esp_zb_zcl_set_attribute_val(ENDPOINT_ID,
        ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID, &setpoint, false);
    esp_zb_zcl_set_attribute_val(ENDPOINT_ID,
        ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID, &sys_mode, false);
    esp_zb_zcl_set_attribute_val(ENDPOINT_ID,
        ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID, &fan_mode, false);
    g_vane_attr = hp_to_vane_idx(st);
    esp_zb_zcl_set_attribute_val(ENDPOINT_ID,
        ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        VANE_ATTR_ID, &g_vane_attr, false);
    esp_zb_lock_release();
}

// ─── Réception des commandes Home Assistant ─────────────────────────────────

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                   const void *message)
{
    if (callback_id != ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) return ESP_OK;

    const esp_zb_zcl_set_attr_value_message_t *m =
        (const esp_zb_zcl_set_attr_value_message_t *)message;
    if (m->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) return ESP_OK;

    uint16_t cluster = m->info.cluster;
    uint16_t attr    = m->attribute.id;
    void    *val     = m->attribute.data.value;

    if (cluster == ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT) {
        if (attr == ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID) {
            apply_system_mode(*(uint8_t *)val);
        } else if (attr == ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID ||
                   attr == ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID) {
            int16_t centi = *(int16_t *)val;
            cn105_set_temp(centi / 100.0f);
        } else if (attr == VANE_ATTR_ID) {
            apply_vane(*(uint8_t *)val);
        }
    } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL) {
        if (attr == ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID) {
            apply_fan_mode(*(uint8_t *)val);
        }
    }
    return ESP_OK;
}

// ─── Commissioning Zigbee ───────────────────────────────────────────────────

static void bdb_start_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(
        esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK,
        , TAG, "Échec démarrage commissioning");
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
            ESP_LOGW(TAG, "Échec init stack: %s", esp_err_to_name(err_status));
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING: {
        uint16_t addr = esp_zb_get_short_address();
        bool in_network = (addr != 0xFFFE && addr != 0xFFFF);
        if (in_network) {
            g_led_state = LED_CONNECTED;
            ESP_LOGI(TAG, "✅ Appairé ! PAN=0x%04hx Canal=%d Adresse=0x%04hx",
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(), addr);
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
        ESP_LOGI(TAG, "Signal ZDO: %s (0x%x), status: %s",
                 esp_zb_zdo_signal_to_string(sig_type), sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}

// ─── Construction de l'endpoint thermostat + fan ────────────────────────────

static esp_zb_cluster_list_t *create_hvac_cluster_list(void)
{
    esp_zb_cluster_list_t *list = esp_zb_zcl_cluster_list_create();

    // Basic
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x01,   // secteur
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)MODEL_ID);
    esp_zb_cluster_list_add_basic_cluster(list, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Identify
    esp_zb_identify_cluster_cfg_t id_cfg = { .identify_time = 0 };
    esp_zb_cluster_list_add_identify_cluster(list,
        esp_zb_identify_cluster_create(&id_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Thermostat
    esp_zb_thermostat_cluster_cfg_t th_cfg = {
        .local_temperature             = 2100,
        .occupied_cooling_setpoint     = SETPOINT_DEFAULT,
        .occupied_heating_setpoint     = SETPOINT_DEFAULT,
        .control_sequence_of_operation =
            ESP_ZB_ZCL_THERMOSTAT_CONTROL_SEQ_OF_OPERATION_COOLING_AND_HEATING_4_PIPES,
        .system_mode                   = ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF,
    };
    esp_zb_attribute_list_t *th = esp_zb_thermostat_cluster_create(&th_cfg);
    // Bornes de consigne 16–31°C pour que HA affiche la bonne plage
    int16_t lim_min = SETPOINT_MIN, lim_max = SETPOINT_MAX;
    esp_zb_thermostat_cluster_add_attr(th,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_MIN_HEAT_SETPOINT_LIMIT_ID, &lim_min);
    esp_zb_thermostat_cluster_add_attr(th,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_MAX_HEAT_SETPOINT_LIMIT_ID, &lim_max);
    esp_zb_thermostat_cluster_add_attr(th,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_MIN_COOL_SETPOINT_LIMIT_ID, &lim_min);
    esp_zb_thermostat_cluster_add_attr(th,
        ESP_ZB_ZCL_ATTR_THERMOSTAT_MAX_COOL_SETPOINT_LIMIT_ID, &lim_max);
    // Attribut personnalisé : position des pales (uint8, lecture/écriture)
    esp_zb_thermostat_cluster_add_attr(th, VANE_ATTR_ID, &g_vane_attr);
    esp_zb_cluster_list_add_thermostat_cluster(list, th, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Fan Control
    esp_zb_fan_control_cluster_cfg_t fan_cfg = {
        .fan_mode          = ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_AUTO,
        .fan_mode_sequence = ESP_ZB_ZCL_FAN_CONTROL_FAN_MODE_SEQUENCE_LOW_MED_HIGH_AUTO,
    };
    esp_zb_cluster_list_add_fan_control_cluster(list,
        esp_zb_fan_control_cluster_create(&fan_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    return list;
}

// ─── Boucle de synchronisation avec l'unité Mitsubishi ──────────────────────

static void hp_sync_task(void *arg)
{
    cn105_init();

    // Handshake : on (re)tente jusqu'à recevoir le CONNECT_ACK
    while (!g_hp.connected) {
        cn105_connect();
        for (int i = 0; i < 10 && !g_hp.connected; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            cn105_poll(&g_hp);
        }
        if (!g_hp.connected) ESP_LOGW(TAG, "Pas de réponse de l'unité, nouvel essai...");
    }
    ESP_LOGI(TAG, "Unité Mitsubishi connectée");

    uint8_t cycle = 0;
    while (true) {
        // Interroge tour à tour réglages / température / statut
        switch (cycle % 3) {
        case 0: cn105_request_settings();  break;
        case 1: cn105_request_room_temp(); break;
        case 2: cn105_request_status();    break;
        }
        cycle++;

        // Laisse le temps à la réponse d'arriver, décode en continu
        bool changed = false;
        for (int i = 0; i < 10; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (cn105_poll(&g_hp)) changed = true;
        }

        if (changed) {
            push_state_to_zigbee(&g_hp);
            ESP_LOGI(TAG, "📡 %s %s | consigne %.1f°C | pièce %.1f°C | vent. %s | %s",
                     g_hp.power_on ? "ON" : "OFF", g_hp.mode,
                     g_hp.target_temp, g_hp.room_temp, g_hp.fan,
                     g_hp.operating ? "actif" : "repos");
        }
    }
}

// ─── Tâche Zigbee ───────────────────────────────────────────────────────────

static void esp_zb_task(void *pvParameters)
{
    esp_zb_cfg_t zb_nwk_cfg = {};
    zb_nwk_cfg.esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER;
    zb_nwk_cfg.install_code_policy = false;
    zb_nwk_cfg.nwk_cfg.zczr_cfg.max_children = 10;
    esp_zb_init(&zb_nwk_cfg);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint           = ENDPOINT_ID,
        .app_profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id      = ESP_ZB_HA_THERMOSTAT_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, create_hvac_cluster_list(), ep_cfg);
    esp_zb_device_register(ep_list);

    esp_zb_core_action_handler_register(zb_action_handler);

    esp_zb_set_primary_network_channel_set(CHANNEL_MASK);
    esp_zb_secur_network_min_join_lqi_set(0);

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

// ─── Point d'entrée ─────────────────────────────────────────────────────────

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    esp_zb_platform_config_t config = {};
    config.radio_config.radio_mode = ZB_RADIO_MODE_NATIVE;
    config.host_config.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE;
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    ESP_LOGI(TAG, "=== mitsubishi2zigbee — passerelle CN105 ↔ Zigbee ===");

    xTaskCreate(led_task,     "led",     4096, NULL, 3, NULL);
    xTaskCreate(hp_sync_task, "hp_sync", 4096, NULL, 4, NULL);
    xTaskCreate(esp_zb_task,  "esp_zb",  4096, NULL, 5, NULL);
}
