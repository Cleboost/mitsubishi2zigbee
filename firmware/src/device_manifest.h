#pragma once

#define ZB_MANUFACTURER_NAME "\x0A""Cleboost"
#define ZB_MODEL_ID          "\x11""Mitsubishi2Zigbee"
#define HP_ENDPOINT_ID       1
#define MITSU_EXT_CLUSTER_ID 0xFF00
#define VANE_ATTR_ID         0x0000
/** CN105 compressor active (INFO 0x06 data[4]); 0=idle, 1=running */
#define OPERATING_ATTR_ID    0x0001
/** 0 = status LED off; 1 = normal blue/green patterns (default) */
#define DEBUG_LED_ATTR_ID    0x0002

#define HP_SETPOINT_DEFAULT  2100
#define HP_SETPOINT_MIN      1600
#define HP_SETPOINT_MAX      3100

#define NVS_NS               "mitsu_zb"
#define NVS_KEY_DEBUG_LED    "debug_led"