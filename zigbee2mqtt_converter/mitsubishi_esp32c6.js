const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const tz = require('zigbee-herdsman-converters/converters/toZigbee');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');
const e = exposes.presets;
const ea = exposes.access;

const VANE_OPTIONS = ['auto', '1', '2', '3', '4', '5', 'swing'];

const OPERATING_ATTR = '0x0401';
const DEBUG_LED_ATTR = '0x0402';

/** ZCL systemMode → string (thermostat cluster). */
const SYSTEM_MODE_FROM_ZCL = {
  0: 'off',
  1: 'auto',
  3: 'cool',
  4: 'heat',
  7: 'fan_only',
  8: 'dry',
};

/**
 * Same rules as gysmo38/mitsubishi2MQTT hpGetAction() — maps to Z2M running_state.
 * @see https://github.com/gysmo38/mitsubishi2MQTT/blob/master/src/mitsubishi2mqtt/mitsubishi2mqtt.ino
 */
function runningStateFromMode(systemMode, operating) {
  const mode =
    typeof systemMode === 'number'
      ? SYSTEM_MODE_FROM_ZCL[systemMode]
      : systemMode;
  if (mode === 'off') return 'idle';
  if (mode === 'fan_only') return 'fan_only';
  if (!operating) return 'idle';
  if (mode === 'auto') return 'idle';
  if (mode === 'cool') return 'cool';
  if (mode === 'heat') return 'heat';
  if (mode === 'dry') return 'dry';
  return 'idle';
}

function updateRunningStateMeta(meta, msg) {
  if (!meta.mitsuHp) meta.mitsuHp = {};
  if (msg.data.systemMode !== undefined) {
    meta.mitsuHp.systemMode = msg.data.systemMode;
  }
  if (msg.data[OPERATING_ATTR] !== undefined) {
    meta.mitsuHp.operating = msg.data[OPERATING_ATTR] === 1;
  }
}

function publishRunningState(meta) {
  const {systemMode, operating} = meta.mitsuHp ?? {};
  if (systemMode === undefined || operating === undefined) return null;
  return {running_state: runningStateFromMode(systemMode, operating)};
}

const fzRunningState = {
  cluster: 'hvacThermostat',
  type: ['attributeReport', 'readResponse'],
  convert: (model, msg, publish, options, meta) => {
    updateRunningStateMeta(meta, msg);
    return publishRunningState(meta);
  },
};

function swBuildIdToFileVersion(swBuildId) {
    if (!swBuildId || typeof swBuildId !== 'string') return null;
    const parts = swBuildId.split('.');
    if (parts.length !== 3) return null;
    const nums = parts.map((p) => parseInt(p, 10));
    if (nums.some((n) => Number.isNaN(n))) return null;
    return (nums[0] << 16) | (nums[1] << 8) | nums[2];
}

async function tryBind(endpoint, coordinatorEndpoint, cluster) {
    try {
        await endpoint.bind(cluster, coordinatorEndpoint);
    } catch {}
}

const fzFirmware = {
    cluster: 'genBasic',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        if (msg.data.swBuildId === undefined) return;
        const fileVersion = swBuildIdToFileVersion(msg.data.swBuildId);
        const result = {software_build_id: msg.data.swBuildId};
        if (fileVersion != null) {
            result.update = {
                installed_version: fileVersion,
                latest_version: fileVersion,
                state: 'idle',
            };
        }
        return result;
    },
};

const fzVane = {
  cluster: "hvacThermostat",
  type: ["attributeReport", "readResponse"],
  convert: (model, msg, publish, options, meta) => {
    if (msg.data["0x0400"] !== undefined) {
      const idx = msg.data["0x0400"];
      return { vane_position: VANE_OPTIONS[idx] ?? "auto" };
    }
  },
};

const fzDebug = {
  cluster: "hvacThermostat",
  type: ["attributeReport", "readResponse"],
  convert: (model, msg) => {
    if (msg.data[DEBUG_LED_ATTR] === undefined) return;
    return {debug: msg.data[DEBUG_LED_ATTR] === 1};
  },
};

const tzDebug = {
  key: ["debug"],
  convertSet: async (entity, key, value, meta) => {
    const on = value === true || value === "ON" || value === 1;
    await entity.write("hvacThermostat", {
      [DEBUG_LED_ATTR]: {value: on ? 1 : 0, type: 0x20},
    });
    return {state: {debug: on}};
  },
  convertGet: async (entity, key, meta) => {
    await entity.read("hvacThermostat", [DEBUG_LED_ATTR]);
  },
};

const tzVane = {
  key: ["vane_position"],
  convertSet: async (entity, key, value, meta) => {
    const idx = VANE_OPTIONS.indexOf(value);
    if (idx < 0) return;
    await entity.write("hvacThermostat", {
      "0x0400": { value: idx, type: 0x20 },
    });
    return { state: { vane_position: value } };
  },
  convertGet: async (entity, key, meta) => {
    await entity.read("hvacThermostat", ["0x0400"]);
  },
};

const definition = {
  zigbeeModel: ["Mitsubishi2Zigbee"],
  model: "Mitsubishi2Zigbee",
  vendor: "Cleboost",
  description: "Cleboost ESP32-C6 Zigbee gateway for Mitsubishi CN105",
  ota: {
    manufacturerCode: 0x1337,
    imageType: 0x0001,
  },
  endpoint: (device) => ({default: 1, ota: 2}),
  fromZigbee: [fzFirmware, fzRunningState, fz.thermostat, fz.fan, fzVane, fzDebug],
  toZigbee: [
    tz.thermostat_system_mode,
    tz.thermostat_occupied_heating_setpoint,
    tz.thermostat_occupied_cooling_setpoint,
    tz.fan_mode,
    tzVane,
    tzDebug,
  ],
  exposes: [
    e
      .climate()
      .withSystemMode(["off", "heat", "cool", "auto", "fan_only", "dry"])
      .withSetpoint("occupied_heating_setpoint", 16, 31, 1)
      .withLocalTemperature()
      .withFanMode(["auto", "low", "medium", "high", "smart"])
      .withRunningState(["idle", "heat", "cool", "dry", "fan_only"], ea.STATE),
    e
      .enum("vane_position", ea.ALL, VANE_OPTIONS)
      .withDescription("Inclinaison des pales (AUTO, 1-5, SWING)"),
    e
      .binary("debug", ea.ALL, true, false)
      .withDescription(
        "Status LED: false (default) = off; true = normal blue/green pairing patterns",
      ),
  ],
  configure: async (device, coordinatorEndpoint) => {
    const ep = device.getEndpoint(1);
    await tryBind(ep, coordinatorEndpoint, "hvacThermostat");
    await tryBind(ep, coordinatorEndpoint, "hvacFanCtrl");
    try {
      await reporting.thermostatTemperature(ep);
      await reporting.thermostatOccupiedHeatingSetpoint(ep);
      await reporting.fanMode(ep);
    } catch {}
    await ep.read("genBasic", ["swBuildId"]);
    await ep.read("hvacThermostat", [
      "localTemp",
      "occupiedHeatingSetpoint",
      "systemMode",
      OPERATING_ATTR,
      DEBUG_LED_ATTR,
    ]);
    try {
      await ep.configureReporting("hvacThermostat", [
        {
          attribute: {ID: 0x0401, type: 0x20},
          minimumReportInterval: 5,
          maximumReportInterval: 60,
          reportableChange: 1,
        },
      ]);
    } catch {}
    await ep.read("hvacFanCtrl", ["fanMode"]);
    const otaEp = device.getEndpoint(2);
    if (otaEp) {
      try {
        await otaEp.read("genOta", ["currentFileVersion"]);
      } catch {}
    }
  },
  onEvent: async (type, data, device) => {
    if (type !== "deviceInterview") return;
    const ep = device.getEndpoint(1);
    if (ep) {
      try {
        await ep.read("genBasic", ["swBuildId"]);
      } catch {}
    }
  },
};

module.exports = definition;
