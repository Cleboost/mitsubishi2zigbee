// cn105.cpp - Mitsubishi CN105 protocol (ESP-IDF)
// Logic aligned with SwiCago/HeatPump (LGPL) + gysmo38/mitsubishi2MQTT usage.
#include "cn105.h"

#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "CN105";

#define PACKET_LEN  22
#define RX_BUF_SIZE 256
#define TX_INTERVAL_MS 1000
#define LINK_TIMEOUT_MS 10000

static const uint8_t CONNECT[]    = {0xfc, 0x5a, 0x01, 0x30, 0x02, 0xca, 0x01, 0xa8};
static const uint8_t INFOHEADER[] = {0xfc, 0x42, 0x01, 0x30, 0x10};
static const uint8_t SETHEADER[]  = {0xfc, 0x41, 0x01, 0x30, 0x10};

#define TYPE_INFO_RESP    0x62
#define TYPE_SET_ACK      0x61
#define TYPE_CONNECT_ACK  0x7a

#define INFO_SETTINGS     0x02
#define INFO_ROOM_TEMP    0x03

#define CTRL_POWER  0x01
#define CTRL_MODE   0x02
#define CTRL_TEMP   0x04
#define CTRL_FAN    0x08
#define CTRL_VANE   0x10

static const int TEMP_MAP[16] = {31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16};

static const uint8_t     MODE_B[]   = {0x01, 0x02, 0x03, 0x07, 0x08};
static const char *const MODE_S[]   = {"HEAT", "DRY", "COOL", "FAN", "AUTO"};
static const uint8_t     FAN_B[]    = {0x00, 0x01, 0x02, 0x03, 0x05, 0x06};
static const char *const FAN_S[]    = {"AUTO", "QUIET", "1", "2", "3", "4"};
static const uint8_t     VANE_B[]   = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07};
static const char *const VANE_S[]   = {"AUTO", "1", "2", "3", "4", "5", "SWING"};

static hp_state_t s_wanted = {};
static hp_state_t s_current = {};
static bool       s_state_valid = false;
static bool       s_link_up = false;
static bool       s_user_pending = false;
static bool       s_need_settings = true;
static int64_t    s_last_user_cmd_ms = 0;
#define USER_CMD_GRACE_MS 5000
static int64_t    s_last_tx_ms = 0;
static int64_t    s_last_rx_ms = 0;
static bool       s_half_degree = false;
static bool       s_half_degree_known = false;
static bool       s_unit_isee = false;
static bool       s_rx_seen = false;
static int        s_baud_rate = 2400;
static uint8_t       s_connect_fails = 0;

static const char *byte_to_str(uint8_t b, const uint8_t *bytes, const char *const *strs, int n)
{
    for (int i = 0; i < n; i++) if (bytes[i] == b) return strs[i];
    return strs[0];
}

static uint8_t str_to_byte(const char *s, const uint8_t *bytes, const char *const *strs, int n)
{
    for (int i = 0; i < n; i++) if (strcmp(strs[i], s) == 0) return bytes[i];
    return bytes[0];
}

static uint8_t checksum(const uint8_t *p, int len)
{
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) sum += p[i];
    return (0xfc - sum) & 0xff;
}

static bool verify_rx_checksum(const uint8_t *hdr, const uint8_t *data, uint8_t dlen, uint8_t cks)
{
    uint8_t sum = 0xfc + hdr[0] + hdr[1] + hdr[2] + hdr[3];
    for (int i = 0; i < dlen; i++) sum += data[i];
    return cks == ((uint8_t)((0xfc - sum) & 0xff));
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool tx_ready(void)
{
    return (now_ms() - s_last_tx_ms) >= TX_INTERVAL_MS;
}

static void note_tx(void)
{
    s_last_tx_ms = now_ms();
}

static void write_packet(uint8_t *packet)
{
    packet[PACKET_LEN - 1] = checksum(packet, PACKET_LEN - 1);
    uart_write_bytes(CN105_UART_PORT, (const char *)packet, PACKET_LEN);
    note_tx();
}

static float legacy_byte_to_temp(uint8_t b)
{
    for (int i = 0; i < 16; i++) {
        if ((uint8_t)i == b) return (float)TEMP_MAP[i];
    }
    return 21.0f;
}

static uint8_t temp_to_legacy_byte(float temp_c)
{
    int t = (int)lroundf(temp_c);
    if (t < 16) t = 16;
    if (t > 31) t = 31;
    for (int i = 0; i < 16; i++) {
        if (TEMP_MAP[i] == t) return (uint8_t)i;
    }
    return 0;
}

static void ensure_state_defaults(void)
{
    if (s_state_valid) return;
    s_wanted = {};
    s_current = {};
    s_wanted.power_on = false;
    s_current.power_on = false;
    strncpy(s_wanted.mode, "HEAT", sizeof(s_wanted.mode));
    strncpy(s_current.mode, "HEAT", sizeof(s_current.mode));
    s_wanted.target_temp = 21.0f;
    s_current.target_temp = 21.0f;
    strncpy(s_wanted.fan, "AUTO", sizeof(s_wanted.fan));
    strncpy(s_current.fan, "AUTO", sizeof(s_current.fan));
    strncpy(s_wanted.vane, "AUTO", sizeof(s_wanted.vane));
    strncpy(s_current.vane, "AUTO", sizeof(s_current.vane));
    s_state_valid = true;
}

static void note_temp_encoding_from_info(const uint8_t *data, int dlen)
{
    if (dlen < 12) return;
    s_half_degree = (data[11] != 0x00);
    s_half_degree_known = true;
}

static void copy_cmd_fields(const hp_state_t *src, hp_state_t *dst)
{
    dst->power_on = src->power_on;
    strncpy(dst->mode, src->mode, sizeof(dst->mode));
    dst->target_temp = src->target_temp;
    strncpy(dst->fan, src->fan, sizeof(dst->fan));
    strncpy(dst->vane, src->vane, sizeof(dst->vane));
}

static bool settings_differ(const hp_state_t *wanted, const hp_state_t *current);

static void apply_settings_from_info(const uint8_t *data, int dlen, hp_state_t *st)
{
    if (dlen < 12) return;
    note_temp_encoding_from_info(data, dlen);

    s_unit_isee = (data[4] > 0x08);

    hp_state_t unit = s_current;
    unit.power_on = (data[3] == 0x01);

    uint8_t mode_b = data[4];
    if (mode_b > 0x08) mode_b = (uint8_t)(mode_b - 0x08);
    strncpy(unit.mode, byte_to_str(mode_b, MODE_B, MODE_S, 5), sizeof(unit.mode));

    if (data[11] != 0x00) unit.target_temp = (data[11] - 128) / 2.0f;
    else                  unit.target_temp = legacy_byte_to_temp(data[5]);

    strncpy(unit.fan, byte_to_str(data[6], FAN_B, FAN_S, 6), sizeof(unit.fan));
    strncpy(unit.vane, byte_to_str(data[7], VANE_B, VANE_S, 7), sizeof(unit.vane));

    /* INFO settings ne contient pas temp pièce - ne pas écraser */
    unit.room_temp = st->room_temp;
    unit.connected = st->connected;

    const bool initial_sync = s_need_settings;
    s_current = unit;

    /* HeatPump: wanted ≠ current - sync murale mode/power/temp ; garder fan/vane utilisateur */
    if (initial_sync) {
        s_wanted = unit;
    } else if (!s_user_pending &&
               (now_ms() - s_last_user_cmd_ms) > USER_CMD_GRACE_MS) {
        s_wanted.power_on = unit.power_on;
        /* Ne pas écraser fan_only tant que l'unité n'a pas confirmé le mode FAN */
        if (strcmp(s_wanted.mode, "FAN") != 0 || strcmp(unit.mode, "FAN") == 0) {
            strncpy(s_wanted.mode, unit.mode, sizeof(s_wanted.mode));
        }
        s_wanted.target_temp = unit.target_temp;
        if (strcmp(s_wanted.fan, "AUTO") == 0) {
            strncpy(s_wanted.fan, unit.fan, sizeof(s_wanted.fan));
        }
        if (strcmp(s_wanted.vane, "AUTO") == 0) {
            strncpy(s_wanted.vane, unit.vane, sizeof(s_wanted.vane));
        }
    }

    if (s_user_pending || settings_differ(&s_wanted, &s_current)) {
        copy_cmd_fields(&s_wanted, st);
    } else {
        copy_cmd_fields(&s_current, st);
    }

    st->room_temp = unit.room_temp;
    st->connected = unit.connected;
    s_state_valid = true;
}

static void mark_rx(void)
{
    s_last_rx_ms = now_ms();
}

static void mark_link_up(hp_state_t *out)
{
    if (!s_link_up) {
        s_link_up = true;
        s_need_settings = true;
    }
    if (out && !out->connected) {
        out->connected = true;
    }
}

static void mark_link_down(hp_state_t *out)
{
    if (s_link_up) {
        ESP_LOGW(TAG, "CN105 link down");
    }
    s_link_up = false;
    s_need_settings = true;
    if (out) {
        out->connected = false;
    }
}

static void check_link_timeout(hp_state_t *out)
{
    if (!s_link_up || s_last_rx_ms == 0) return;
    if ((now_ms() - s_last_rx_ms) > LINK_TIMEOUT_MS) {
        mark_link_down(out);
    }
}

static bool settings_differ(const hp_state_t *wanted, const hp_state_t *current)
{
    if (wanted->power_on != current->power_on) return true;
    if (strcmp(wanted->mode, current->mode) != 0) return true;
    if (strcmp(wanted->mode, "FAN") != 0 &&
        fabsf(wanted->target_temp - current->target_temp) > 0.01f) return true;
    if (strcmp(wanted->fan, current->fan) != 0) return true;
    if (strcmp(wanted->vane, current->vane) != 0) return true;
    return false;
}

/* Position pale fixe 1-5 utilisable en mode FAN (SWING/AUTO = volets refermés MSZ) */
static bool vane_is_fixed(const char *v)
{
    return v && v[0] >= '1' && v[0] <= '5' && v[1] == '\0';
}

/* En mode FAN, AUTO ventilo/volets = unité se referme. Copier vitesse heat ou défaut. */
static void fan_only_prepare(hp_state_t *st)
{
    if (strcmp(st->fan, "AUTO") == 0) {
        if (strcmp(s_current.fan, "AUTO") != 0 && strcmp(s_current.fan, "QUIET") != 0)
            strncpy(st->fan, s_current.fan, sizeof(st->fan));
        else
            strncpy(st->fan, "3", sizeof(st->fan));
    }
    if (vane_is_fixed(st->vane)) {
        return;
    }
    if (vane_is_fixed(s_current.vane)) {
        strncpy(st->vane, s_current.vane, sizeof(st->vane));
    } else {
        strncpy(st->vane, "1", sizeof(st->vane));
    }
}

static uint8_t mode_to_byte(const char *mode)
{
    uint8_t b = str_to_byte(mode, MODE_B, MODE_S, 5);
    if (s_unit_isee) b = (uint8_t)(b + 0x08);
    return b;
}

/* HeatPump::createPacket - only changed fields, single ctrl byte */
static bool send_update(const hp_state_t *wanted, const hp_state_t *current)
{
    uint8_t packet[PACKET_LEN] = {};
    memcpy(packet, SETHEADER, sizeof(SETHEADER));
    packet[5] = 0x01;
    packet[6] = 0x00;
    packet[7] = 0x00;

    const bool entering_fan = wanted->power_on &&
        strcmp(wanted->mode, "FAN") == 0 &&
        (strcmp(current->mode, "FAN") != 0 || !current->power_on);

    if (entering_fan) {
        /* Entrée mode ventilateur : paquet complet (AUTO = muet sur la plupart des MSZ) */
        packet[8] = 0x01;
        packet[9] = mode_to_byte("FAN");
        packet[11] = str_to_byte(wanted->fan, FAN_B, FAN_S, 6);
        packet[12] = str_to_byte(wanted->vane, VANE_B, VANE_S, 7);
        packet[6] = (uint8_t)(CTRL_POWER | CTRL_MODE | CTRL_FAN | CTRL_VANE);
    } else {
        if (wanted->power_on != current->power_on) {
            packet[8] = wanted->power_on ? 0x01 : 0x00;
            packet[6] |= CTRL_POWER;
        }
        if (strcmp(wanted->mode, current->mode) != 0) {
            packet[9] = mode_to_byte(wanted->mode);
            packet[6] |= CTRL_MODE;
        }
        /* Pas de consigne temp en mode FAN (HeatPump tempMode) */
        if (strcmp(wanted->mode, "FAN") != 0 &&
            fabsf(wanted->target_temp - current->target_temp) > 0.01f) {
            if (s_half_degree_known && s_half_degree) {
                float t = roundf(wanted->target_temp * 2.0f) / 2.0f;
                packet[19] = (uint8_t)((int)lroundf(t * 2.0f) + 128);
            } else {
                packet[10] = temp_to_legacy_byte(wanted->target_temp);
            }
            packet[6] |= CTRL_TEMP;
        }
        if (strcmp(wanted->fan, current->fan) != 0) {
            packet[11] = str_to_byte(wanted->fan, FAN_B, FAN_S, 6);
            packet[6] |= CTRL_FAN;
        }
        if (strcmp(wanted->vane, current->vane) != 0) {
            packet[12] = str_to_byte(wanted->vane, VANE_B, VANE_S, 7);
            packet[6] |= CTRL_VANE;
        }
    }

    if (packet[6] == 0) return false;

    /* Certaines unités ignorent fan/vane sans bit POWER explicite */
    if (wanted->power_on && !(packet[6] & CTRL_POWER)) {
        packet[8] = 0x01;
        packet[6] |= CTRL_POWER;
    }

    write_packet(packet);
    return true;
}

void cn105_init(void)
{
    uart_config_t cfg = {};
    cfg.baud_rate  = 2400;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_EVEN;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(CN105_UART_PORT, RX_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CN105_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(CN105_UART_PORT, CN105_TX_PIN, CN105_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_set_pull_mode((gpio_num_t)CN105_RX_PIN, GPIO_PULLUP_ONLY);
    uart_flush_input(CN105_UART_PORT);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

void cn105_connect(void)
{
    if (!tx_ready()) return;
    uart_write_bytes(CN105_UART_PORT, (const char *)CONNECT, sizeof(CONNECT));
    note_tx();
}

void cn105_rotate_baud_if_silent(void)
{
    if (s_link_up || s_rx_seen) {
        s_connect_fails = 0;
        return;
    }
    if (++s_connect_fails < 5) return;
    s_connect_fails = 0;
    s_baud_rate = (s_baud_rate == 2400) ? 9600 : 2400;
    uart_set_baudrate(CN105_UART_PORT, s_baud_rate);
    ESP_LOGW(TAG, "CN105 silent - try %d baud", s_baud_rate);
}

static void request_info(uint8_t info_mode)
{
    if (!tx_ready()) return;

    uint8_t packet[PACKET_LEN] = {};
    memcpy(packet, INFOHEADER, sizeof(INFOHEADER));
    packet[5] = info_mode;
    write_packet(packet);
}

void cn105_request_settings(void)  { request_info(INFO_SETTINGS); }
void cn105_request_room_temp(void) { request_info(INFO_ROOM_TEMP); }

static void note_user_cmd(void)
{
    s_last_user_cmd_ms = now_ms();
    s_user_pending = true;
}

bool cn105_flush_pending(const hp_state_t *live)
{
    ensure_state_defaults();
    if (!s_link_up) return false;
    if (s_need_settings) {
        return false;
    }
    if (!settings_differ(&s_wanted, &s_current)) {
        s_user_pending = false;
        return false;
    }
    if (!tx_ready()) return false;

    /* HeatPump::update - vider RX avant UPDATE pour ne pas rater SET_ACK */
    if (live) {
        while (cn105_poll((hp_state_t *)live)) {}
    } else {
        hp_state_t sink = s_current;
        while (cn105_poll(&sink)) {}
    }

    if (send_update(&s_wanted, &s_current)) {
        return true;
    }
    return false;
}

bool cn105_cmd_pending(void)
{
    return s_user_pending;
}

bool cn105_request_settings_if_needed(void)
{
    if (!s_link_up || !s_need_settings || !tx_ready()) return false;
    request_info(INFO_SETTINGS);
    return true;
}

static void queue_change(void)
{
    cn105_flush_pending(nullptr);
}

static void retry_flush_if_stale(void)
{
    if (settings_differ(&s_wanted, &s_current))
        queue_change();
}

const hp_state_t *cn105_wanted_state(void)
{
    ensure_state_defaults();
    return &s_wanted;
}

void cn105_set_power(bool on)
{
    ensure_state_defaults();
    if (s_wanted.power_on == on) {
        retry_flush_if_stale();
        return;
    }
    note_user_cmd();
    s_wanted.power_on = on;
    queue_change();
}

void cn105_set_system_mode(bool power_on, const char *mode)
{
    ensure_state_defaults();
    hp_state_t next = s_wanted;
    bool changed = false;

    if (next.power_on != power_on) {
        next.power_on = power_on;
        changed = true;
    }
    if (mode && mode[0] && strcmp(next.mode, mode) != 0) {
        strncpy(next.mode, mode, sizeof(next.mode));
        if (strcmp(mode, "FAN") == 0) {
            fan_only_prepare(&next);
        }
        changed = true;
    }
    if (!changed) {
        retry_flush_if_stale();
        return;
    }
    if (!settings_differ(&next, &s_wanted)) {
        retry_flush_if_stale();
        return;
    }
    note_user_cmd();
    s_wanted = next;
    queue_change();
}

void cn105_enter_fan_only(void)
{
    cn105_set_system_mode(true, "FAN");
}

void cn105_set_temp(float temp_c)
{
    if (temp_c < 16.0f) temp_c = 16.0f;
    if (temp_c > 31.0f) temp_c = 31.0f;
    if (s_half_degree_known && s_half_degree) {
        temp_c = roundf(temp_c * 2.0f) / 2.0f;
    } else {
        temp_c = (float)(int)lroundf(temp_c);
    }
    ensure_state_defaults();
    if (fabsf(s_wanted.target_temp - temp_c) <= 0.01f) {
        retry_flush_if_stale();
        return;
    }
    note_user_cmd();
    s_wanted.target_temp = temp_c;
    queue_change();
}

void cn105_set_fan(const char *fan)
{
    ensure_state_defaults();
    /* Z2M renvoie souvent fan AUTO après fan_only → tue le mode ventilateur */
    if (strcmp(s_wanted.mode, "FAN") == 0 && strcmp(fan, "AUTO") == 0) return;
    if (strcmp(s_wanted.fan, fan) == 0) {
        retry_flush_if_stale();
        return;
    }
    note_user_cmd();
    strncpy(s_wanted.fan, fan, sizeof(s_wanted.fan));
    queue_change();
}

void cn105_set_vane(const char *vane)
{
    ensure_state_defaults();
    if (strcmp(s_wanted.vane, vane) == 0) {
        retry_flush_if_stale();
        return;
    }
    note_user_cmd();
    strncpy(s_wanted.vane, vane, sizeof(s_wanted.vane));
    queue_change();
}

static bool parse_info(const uint8_t *data, int dlen, hp_state_t *st)
{
    bool changed = false;
    uint8_t info_mode = data[0];

    if (info_mode == INFO_SETTINGS && dlen >= 12) {
        hp_state_t before = *st;
        apply_settings_from_info(data, dlen, st);
        s_need_settings = false;
        if (memcmp(&before, st, sizeof(*st)) != 0) changed = true;
    }
    else if (info_mode == INFO_ROOM_TEMP && dlen >= 7) {
        float temp;
        if (data[6] != 0x00) temp = (data[6] - 128) / 2.0f;
        else                 temp = 10.0f + (float)data[3];
        if (fabsf(temp - st->room_temp) > 0.01f) {
            st->room_temp = temp;
            s_current.room_temp = temp;
            changed = true;
        }
    }
    return changed;
}

bool cn105_poll(hp_state_t *out)
{
    if (!out) return false;

    bool changed = false;
    uint8_t byte;

    while (uart_read_bytes(CN105_UART_PORT, &byte, 1, 0) == 1) {
        s_rx_seen = true;
        if (byte != 0xfc) continue;

        uint8_t hdr[4];
        if (uart_read_bytes(CN105_UART_PORT, hdr, 4, pdMS_TO_TICKS(100)) != 4) break;

        if (hdr[1] != 0x01 || hdr[2] != 0x30) continue;

        uint8_t type = hdr[0];
        uint8_t dlen = hdr[3];
        if (dlen > PACKET_LEN) continue;

        uint8_t data[PACKET_LEN] = {};
        if (dlen > 0 &&
            uart_read_bytes(CN105_UART_PORT, data, dlen, pdMS_TO_TICKS(100)) != dlen) break;

        uint8_t cks;
        if (uart_read_bytes(CN105_UART_PORT, &cks, 1, pdMS_TO_TICKS(100)) != 1) break;

        if (!verify_rx_checksum(hdr, data, dlen, cks)) {
            continue;
        }
        mark_rx();

        if (type == TYPE_CONNECT_ACK) {
            mark_link_up(out);
            changed = true;
        } else if (type == TYPE_INFO_RESP) {
            mark_link_up(out);
            if (parse_info(data, dlen, out)) changed = true;
        } else if (type == TYPE_SET_ACK) {
            mark_link_up(out);
            s_current = s_wanted;
            s_user_pending = false;
            changed = true;
        }
    }

    check_link_timeout(out);
    return changed;
}
