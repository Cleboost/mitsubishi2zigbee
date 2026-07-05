// ─────────────────────────────────────────────────────────────────────────────
// cn105.cpp — Implémentation du protocole Mitsubishi CN105 (ESP-IDF)
// Constantes du protocole reprises de SwiCago/HeatPump (licence LGPL).
// ─────────────────────────────────────────────────────────────────────────────
#include "cn105.h"

#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "CN105";

// ─── Constantes du protocole ────────────────────────────────────────────────

#define PACKET_LEN  22
#define RX_BUF_SIZE 256

// Trames d'en-tête
static const uint8_t CONNECT[]    = {0xfc, 0x5a, 0x01, 0x30, 0x02, 0xca, 0x01, 0xa8};
static const uint8_t INFOHEADER[] = {0xfc, 0x42, 0x01, 0x30, 0x10};
static const uint8_t SETHEADER[]  = {0xfc, 0x41, 0x01, 0x30, 0x10};

// Types de trame reçue (octet 1)
#define TYPE_INFO_RESP    0x62
#define TYPE_SET_ACK      0x61
#define TYPE_CONNECT_ACK  0x7a

// Modes d'information (data[0] d'une requête/réponse 0x62)
#define INFO_SETTINGS     0x02
#define INFO_ROOM_TEMP    0x03
#define INFO_STATUS       0x06

// Bits de contrôle SET : quels champs sont modifiés
#define CTRL_POWER  0x01
#define CTRL_MODE   0x02
#define CTRL_TEMP   0x04
#define CTRL_FAN    0x08
#define CTRL_VANE   0x10

// Tables de correspondance octet ↔ chaîne
static const uint8_t     POWER_B[]  = {0x00, 0x01};
static const char *const POWER_S[]  = {"OFF", "ON"};
static const uint8_t     MODE_B[]   = {0x01, 0x02, 0x03, 0x07, 0x08};
static const char *const MODE_S[]   = {"HEAT", "DRY", "COOL", "FAN", "AUTO"};
static const uint8_t     FAN_B[]    = {0x00, 0x01, 0x02, 0x03, 0x05, 0x06};
static const char *const FAN_S[]    = {"AUTO", "QUIET", "1", "2", "3", "4"};
static const uint8_t     VANE_B[]   = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07};
static const char *const VANE_S[]   = {"AUTO", "1", "2", "3", "4", "5", "SWING"};

// ─── Helpers de correspondance ──────────────────────────────────────────────

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

// ─── Émission ───────────────────────────────────────────────────────────────

static void write_packet(uint8_t *packet)
{
    packet[PACKET_LEN - 1] = checksum(packet, PACKET_LEN - 1);
    uart_write_bytes(CN105_UART_PORT, (const char *)packet, PACKET_LEN);
}

void cn105_init(void)
{
    uart_config_t cfg = {};
    cfg.baud_rate  = 2400;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_EVEN;          // 8E1
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(CN105_UART_PORT, RX_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CN105_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(CN105_UART_PORT, CN105_TX_PIN, CN105_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART%d initialisé (2400 8E1, TX=%d RX=%d)",
             CN105_UART_PORT, CN105_TX_PIN, CN105_RX_PIN);
}

void cn105_connect(void)
{
    uart_write_bytes(CN105_UART_PORT, (const char *)CONNECT, sizeof(CONNECT));
    ESP_LOGI(TAG, "→ CONNECT");
}

static void request_info(uint8_t info_mode)
{
    uint8_t packet[PACKET_LEN] = {};
    memcpy(packet, INFOHEADER, sizeof(INFOHEADER));
    packet[5] = info_mode;
    write_packet(packet);
}

void cn105_request_settings(void)  { request_info(INFO_SETTINGS); }
void cn105_request_room_temp(void) { request_info(INFO_ROOM_TEMP); }
void cn105_request_status(void)    { request_info(INFO_STATUS); }

// Construit une trame SET ne positionnant que les champs marqués dans `control`.
static void send_set(uint8_t control, uint8_t power, uint8_t mode,
                     float temp, uint8_t fan)
{
    uint8_t packet[PACKET_LEN] = {};
    memcpy(packet, SETHEADER, sizeof(SETHEADER));
    packet[5] = 0x01;       // commande SET
    packet[6] = control;    // octet de contrôle 1
    packet[7] = 0x00;       // octet de contrôle 2 (wideVane, inutilisé ici)

    if (control & CTRL_POWER) packet[8]  = power;
    if (control & CTRL_MODE)  packet[9]  = mode;
    if (control & CTRL_TEMP) {
        // Mode hérité : 31 - température entière, à l'index 10
        int t = (int)lroundf(temp);
        if (t < 16) t = 16;
        if (t > 31) t = 31;
        packet[10] = 31 - t;
        // Mode 0.5°C : (temp*2)+128 à l'index 19 (unités récentes)
        packet[19] = (uint8_t)((int)lroundf(temp * 2.0f) + 128);
    }
    if (control & CTRL_FAN)  packet[11] = fan;

    write_packet(packet);
}

void cn105_set_power(bool on)
{
    send_set(CTRL_POWER, on ? 0x01 : 0x00, 0, 0, 0);
    ESP_LOGI(TAG, "→ SET power=%s", on ? "ON" : "OFF");
}

void cn105_set_mode(const char *mode)
{
    uint8_t b = str_to_byte(mode, MODE_B, MODE_S, 5);
    send_set(CTRL_MODE, 0, b, 0, 0);
    ESP_LOGI(TAG, "→ SET mode=%s", mode);
}

void cn105_set_temp(float temp_c)
{
    if (temp_c < 16.0f) temp_c = 16.0f;
    if (temp_c > 31.0f) temp_c = 31.0f;
    temp_c = roundf(temp_c * 2.0f) / 2.0f;   // pas de 0.5
    send_set(CTRL_TEMP, 0, 0, temp_c, 0);
    ESP_LOGI(TAG, "→ SET temp=%.1f", temp_c);
}

void cn105_set_fan(const char *fan)
{
    uint8_t b = str_to_byte(fan, FAN_B, FAN_S, 6);
    send_set(CTRL_FAN, 0, 0, 0, b);
    ESP_LOGI(TAG, "→ SET fan=%s", fan);
}

void cn105_set_vane(const char *vane)
{
    uint8_t b = str_to_byte(vane, VANE_B, VANE_S, 7);
    // CTRL_VANE (0x10) : seul le champ vane est modifié
    uint8_t packet[PACKET_LEN] = {};
    memcpy(packet, SETHEADER, sizeof(SETHEADER));
    packet[5] = 0x01;
    packet[6] = CTRL_VANE;
    packet[12] = b;
    write_packet(packet);
    ESP_LOGI(TAG, "→ SET vane=%s", vane);
}

// ─── Réception / décodage ───────────────────────────────────────────────────

// Décode une trame 0x62 complète (data = octets après l'en-tête de 5 octets).
static bool parse_info(const uint8_t *data, int dlen, hp_state_t *st)
{
    bool changed = false;
    uint8_t info_mode = data[0];

    if (info_mode == INFO_SETTINGS && dlen >= 12) {
        bool power = (data[3] == 0x01);
        if (power != st->power_on) { st->power_on = power; changed = true; }

        const char *mode = byte_to_str(data[4] & 0x07, MODE_B, MODE_S, 5);
        if (strcmp(mode, st->mode) != 0) { strncpy(st->mode, mode, sizeof(st->mode)); changed = true; }

        float temp;
        if (data[11] != 0x00) temp = (data[11] - 128) / 2.0f;   // mode 0.5°C
        else                  temp = 31.0f - data[5];           // mode hérité
        if (fabsf(temp - st->target_temp) > 0.01f) { st->target_temp = temp; changed = true; }

        const char *fan = byte_to_str(data[6], FAN_B, FAN_S, 6);
        if (strcmp(fan, st->fan) != 0) { strncpy(st->fan, fan, sizeof(st->fan)); changed = true; }

        const char *vane = byte_to_str(data[7], VANE_B, VANE_S, 7);
        if (strcmp(vane, st->vane) != 0) { strncpy(st->vane, vane, sizeof(st->vane)); changed = true; }
    }
    else if (info_mode == INFO_ROOM_TEMP && dlen >= 7) {
        float temp;
        if (data[6] != 0x00) temp = (data[6] - 128) / 2.0f;     // mode 0.5°C
        else                 temp = 10.0f + data[3];            // mode hérité
        if (fabsf(temp - st->room_temp) > 0.01f) { st->room_temp = temp; changed = true; }
    }
    else if (info_mode == INFO_STATUS && dlen >= 5) {
        bool op = (data[4] != 0x00);
        if (op != st->operating) { st->operating = op; changed = true; }
    }
    return changed;
}

bool cn105_poll(hp_state_t *out)
{
    bool changed = false;
    uint8_t byte;

    // Lecture octet par octet : on resynchronise sur l'octet de départ 0xfc.
    while (uart_read_bytes(CN105_UART_PORT, &byte, 1, 0) == 1) {
        if (byte != 0xfc) continue;             // resynchronisation

        uint8_t hdr[4];
        if (uart_read_bytes(CN105_UART_PORT, hdr, 4, pdMS_TO_TICKS(50)) != 4) break;
        uint8_t type = hdr[0];
        uint8_t dlen = hdr[3];
        if (dlen > PACKET_LEN) continue;

        uint8_t data[PACKET_LEN] = {};
        if (dlen > 0 &&
            uart_read_bytes(CN105_UART_PORT, data, dlen, pdMS_TO_TICKS(50)) != dlen) break;

        uint8_t cks;
        if (uart_read_bytes(CN105_UART_PORT, &cks, 1, pdMS_TO_TICKS(50)) != 1) break;

        if (type == TYPE_CONNECT_ACK) {
            if (!out->connected) { out->connected = true; changed = true; }
            ESP_LOGI(TAG, "← CONNECT_ACK");
        } else if (type == TYPE_INFO_RESP) {
            if (parse_info(data, dlen, out)) changed = true;
        }
        // TYPE_SET_ACK (0x61) : accusé de réception, rien à décoder.
    }
    return changed;
}
