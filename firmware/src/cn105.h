// ─────────────────────────────────────────────────────────────────────────────
// cn105.h — Pilote du protocole Mitsubishi CN105 (port ESP-IDF natif)
//
// Réimplémentation en C++/ESP-IDF du protocole série utilisé par la librairie
// Arduino SwiCago/HeatPump. Communique avec une unité Mitsubishi via le
// connecteur CN105 (UART 2400 bauds, 8E1).
//
// Câblage CN105 (5 broches) :
//   1: 12V   2: GND   3: 5V   4: TX (unité→ESP)   5: RX (ESP→unité)
//   - Alimenter l'ESP via la broche 5V (PAS la 12V).
//   - Les lignes TX/RX du CN105 sont en logique 5V : un adaptateur de niveau
//     3.3V↔5V est recommandé sur la ligne RX de l'ESP (broche 4 du CN105).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <stdbool.h>
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

// Broches UART côté ESP32-C6 (modifiables selon le câblage)
#define CN105_UART_PORT   UART_NUM_1
#define CN105_TX_PIN      4    // ESP TX  → CN105 broche 5 (RX unité)
#define CN105_RX_PIN      5    // ESP RX  ← CN105 broche 4 (TX unité)

// État courant de l'unité Mitsubishi, mis à jour au fil des trames reçues.
typedef struct {
    bool  connected;       // handshake CN105 effectué
    bool  power_on;        // alimentation ON/OFF
    char  mode[8];         // "HEAT","DRY","COOL","FAN","AUTO"
    float target_temp;     // consigne en °C (16.0 – 31.0, pas de 0.5)
    char  fan[8];          // "AUTO","QUIET","1","2","3","4"
    char  vane[8];         // "AUTO","1".."5","SWING"
    float room_temp;       // température mesurée par l'unité en °C
    bool  operating;       // compresseur actif (chauffe/refroidit réellement)
} hp_state_t;

// Initialise l'UART. À appeler une fois au démarrage.
void cn105_init(void);

// Envoie la trame de handshake CONNECT. Renvoie true si l'octet a été émis.
void cn105_connect(void);

// Demande à l'unité d'envoyer ses réglages / température / statut.
void cn105_request_settings(void);
void cn105_request_room_temp(void);
void cn105_request_status(void);

// Lit les octets disponibles sur l'UART, décode les trames complètes et met à
// jour `out`. Renvoie true si au moins un champ d'état a changé.
bool cn105_poll(hp_state_t *out);

// Commandes (construisent et émettent une trame SET ne modifiant que ce champ).
void cn105_set_power(bool on);
void cn105_set_mode(const char *mode);    // "HEAT","DRY","COOL","FAN","AUTO"
void cn105_set_temp(float temp_c);        // arrondi au pas de 0.5, borné 16–31
void cn105_set_fan(const char *fan);      // "AUTO","QUIET","1".."4"
void cn105_set_vane(const char *vane);    // "AUTO","1".."5","SWING"

#ifdef __cplusplus
}
#endif
