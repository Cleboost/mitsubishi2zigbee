// cn105.h — Mitsubishi CN105 serial protocol driver (native ESP-IDF port)
//
// ESP-IDF reimplementation of the SwiCago/HeatPump Arduino library protocol.
// UART 2400 baud, 8E1, via the CN105 connector.
//
// CN105 wiring (5 pins):
//   1: 12V   2: GND   3: 5V   4: TX (unit→ESP)   5: RX (ESP→unit)
//   - Power the ESP from pin 3 (5V), NOT pin 1 (12V).
//   - CN105 TX/RX lines are 5V logic; use a 3.3V↔5V level shifter on the
//     ESP RX line (CN105 pin 4).
#pragma once

#include <stdbool.h>
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CN105_UART_PORT   UART_NUM_1
#define CN105_TX_PIN      4    // ESP TX → CN105 pin 5 (unit RX)
#define CN105_RX_PIN      5    // ESP RX ← CN105 pin 4 (unit TX)

typedef struct {
    bool  connected;
    bool  power_on;
    char  mode[8];         // "HEAT","DRY","COOL","FAN","AUTO"
    float target_temp;     // setpoint °C, 16.0–31.0, 0.5 step
    char  fan[8];          // "AUTO","QUIET","1","2","3","4"
    char  vane[8];         // "AUTO","1".."5","SWING"
    float room_temp;
    bool  operating;       // compressor running
} hp_state_t;

void cn105_init(void);
void cn105_connect(void);
void cn105_request_settings(void);
void cn105_request_room_temp(void);
void cn105_request_status(void);
bool cn105_poll(hp_state_t *out);

void cn105_set_power(bool on);
void cn105_set_mode(const char *mode);
void cn105_set_temp(float temp_c);
void cn105_set_fan(const char *fan);
void cn105_set_vane(const char *vane);

#ifdef __cplusplus
}
#endif
