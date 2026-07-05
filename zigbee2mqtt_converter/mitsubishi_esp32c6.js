const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const tz = require('zigbee-herdsman-converters/converters/toZigbee');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');
const e = exposes.presets;
const ea = exposes.access;

const VANE_OPTIONS = ['auto', '1', '2', '3', '4', '5', 'swing'];

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
  zigbeeModel: ["MZ-Zigbee-C6"],
  model: "MZ-Zigbee-C6",
  vendor: "Mitsubishi",
  description: "Mitsubishi HVAC — passerelle ESP32-C6 Zigbee",
  ota: {
    manufacturerCode: 0x1337,
    imageType: 0x0001,
  },
  endpoint: (device) => ({default: 1, ota: 2}),
  fromZigbee: [fzFirmware, fz.thermostat, fz.fan, fzVane],
  toZigbee: [
    tz.thermostat_system_mode,
    tz.thermostat_occupied_heating_setpoint,
    tz.thermostat_occupied_cooling_setpoint,
    tz.fan_mode,
    tzVane,
  ],
  exposes: [
    e
      .climate()
      .withSystemMode(["off", "heat", "cool", "auto", "fan_only", "dry"])
      .withSetpoint("occupied_heating_setpoint", 16, 31, 0.5)
      .withLocalTemperature()
      .withFanMode(["auto", "low", "medium", "high", "smart"]),
    e
      .enum("vane_position", ea.ALL, VANE_OPTIONS)
      .withDescription("Inclinaison des pales (AUTO, 1-5, SWING)"),
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
    ]);
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
