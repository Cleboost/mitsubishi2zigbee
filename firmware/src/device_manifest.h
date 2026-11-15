#pragma once

#define ZB_MANUFACTURER_NAME "\x0A""Cleboost"
#define ZB_MODEL_ID          "\x11""Mitsubishi2Zigbee"
#define HP_ENDPOINT_ID       1
#define MITSU_EXT_CLUSTER_ID 0xFF00
#define VANE_ATTR_ID         0x0000
/** 0 = status LED off; 1 = normal blue/green patterns (default) */
#define DEBUG_LED_ATTR_ID    0x0002

#define HP_SETPOINT_DEFAULT  2100
#define HP_SETPOINT_MIN      1600
#define HP_SETPOINT_MAX      3100
/** Min heat/cool gap for ZCL Thermostat (0.1 °C steps); CN105 still single setpoint */
#define HP_SETPOINT_DEAD_BAND_TENTHS 10
/** Cooling occupied setpoint kept high so heating writes pass ZCL dead-band checks */
#define HP_ZCL_COOL_SETPOINT_DUMMY   3200

#define NVS_NS               "mitsu_zb"
#define NVS_KEY_DEBUG_LED    "debug_led"