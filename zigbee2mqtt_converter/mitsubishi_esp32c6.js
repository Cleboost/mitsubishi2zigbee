const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const tz = require('zigbee-herdsman-converters/converters/toZigbee');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');
const m = require('zigbee-herdsman-converters/lib/modernExtend');
const e = exposes.presets;
const ea = exposes.access;

const VANE_OPTIONS = ['auto', '1', '2', '3', '4', '5', 'swing'];
const MITSU_EXT_CLUSTER = 'manuSpecificMitsubishiHvac';
const HVAC_CLUSTERS = ['hvacThermostat', 'hvacFanCtrl', MITSU_EXT_CLUSTER];

/** Herdsman may use name, hex string, or numeric attr id in msg.data. */
function mitsuAttrU8(msg, name, attrId) {
    const id = attrId;
    return msg.data[name] ?? msg.data[id] ?? msg.data[`0x${id.toString(16).padStart(4, '0')}`];
}

function swBuildIdToFileVersion(swBuildId) {
    if (!swBuildId || typeof swBuildId !== 'string') return null;
    const parts = swBuildId.split('.');
    if (parts.length !== 3) return null;
    const nums = parts.map((p) => parseInt(p, 10));
    if (nums.some((n) => Number.isNaN(n))) return null;
    return (nums[0] << 16) | (nums[1] << 8) | nums[2];
}

function roundDeg(value) {
    return Math.round(Number(value));
}

function setpointState(value) {
    return {
        occupied_heating_setpoint: value,
        current_heating_setpoint: value,
    };
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
    cluster: MITSU_EXT_CLUSTER,
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        if (msg.data.vanePosition === undefined) return;
        const idx = msg.data.vanePosition;
        return {vane_position: VANE_OPTIONS[idx] ?? 'auto'};
    },
};

const fzSetpoint = {
    cluster: 'hvacThermostat',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        if (msg.data.occupiedHeatingSetpoint === undefined) return;
        const value = roundDeg(Number(msg.data.occupiedHeatingSetpoint) / 100);
        return setpointState(value);
    },
};

const fzDebug = {
    cluster: MITSU_EXT_CLUSTER,
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        const v = mitsuAttrU8(msg, 'mitsuDebugLed', 0x0002);
        if (v === undefined) return;
        return {debug: v === 1};
    },
};

const tzSetpoint = {
    key: ['occupied_heating_setpoint'],
    convertSet: async (entity, key, value, meta) => {
        const rounded = roundDeg(value);
        await entity.write('hvacThermostat', {occupiedHeatingSetpoint: rounded * 100});
        return {state: setpointState(rounded)};
    },
    convertGet: async (entity, key, meta) => {
        await entity.read('hvacThermostat', ['occupiedHeatingSetpoint']);
    },
};

const tzVane = {
    key: ['vane_position'],
    convertSet: async (entity, key, value, meta) => {
        const idx = VANE_OPTIONS.indexOf(value);
        if (idx < 0) return;
        await entity.write(MITSU_EXT_CLUSTER, {vanePosition: idx});
        return {state: {vane_position: value}};
    },
    convertGet: async (entity, key, meta) => {
        await entity.read(MITSU_EXT_CLUSTER, ['vanePosition']);
    },
};

const tzDebug = {
    key: ['debug'],
    convertSet: async (entity, key, value, meta) => {
        const on = value === true || value === 'ON' || value === 1;
        await entity.write(MITSU_EXT_CLUSTER, {mitsuDebugLed: on ? 1 : 0});
        meta.state.debug = on;
        return {state: {debug: on}};
    },
    convertGet: async (entity, key, meta) => {
        await entity.read(MITSU_EXT_CLUSTER, ['mitsuDebugLed']);
    },
};

module.exports = {
    zigbeeModel: ['Mitsubishi2Zigbee', 'MZ-Zigbee-C6'],
    model: 'Mitsubishi2Zigbee',
    vendor: 'Cleboost',
    description: 'Cleboost ESP32-C6 Zigbee gateway for Mitsubishi CN105',
    icon: 'https://raw.githubusercontent.com/Cleboost/mitsubishi2zigbee/main/assets/mitsubishi2zigbee.png',
    ota: {
        manufacturerCode: 0x1337,
        imageType: 0x0001,
    },
    endpoint: (device) => ({default: 1, ota: 2}),
    extend: [
        m.deviceAddCustomCluster(MITSU_EXT_CLUSTER, {
            ID: 0xff00,
            attributes: {
                vanePosition: {ID: 0x0000, type: 0x20, write: true},
                mitsuDebugLed: {ID: 0x0002, type: 0x20, write: true},
            },
            commands: {},
            commandsResponse: {},
        }),
    ],
    fromZigbee: [
        fzFirmware,
        fz.thermostat,
        fzSetpoint,
        fz.fan,
        fzVane,
        fzDebug,
    ],
    toZigbee: [
        tz.thermostat_system_mode,
        tzSetpoint,
        tz.fan_mode,
        tzVane,
        tzDebug,
    ],
    exposes: [
        e
            .climate()
            .withSystemMode(['off', 'heat', 'cool', 'auto', 'fan_only', 'dry'])
            .withSetpoint('occupied_heating_setpoint', 16, 31, 1)
            .withLocalTemperature()
            .withFanMode(['auto', 'low', 'medium', 'high', 'smart']),
        e
            .enum('vane_position', ea.ALL, VANE_OPTIONS)
            .withDescription('Inclinaison des pales (AUTO, 1-5, SWING)'),
        e
            .binary('debug', ea.ALL, true, false)
            .withDescription(
                'Status LED: true (default) = normal blue/green pairing patterns; false = off',
            ),
    ],
    configure: async (device, coordinatorEndpoint) => {
        const ep = device.getEndpoint(1);
        for (const cluster of HVAC_CLUSTERS) {
            try {
                await ep.bind(cluster, coordinatorEndpoint);
            } catch {}
        }
        try {
            await reporting.thermostatTemperature(ep);
            await reporting.thermostatOccupiedHeatingSetpoint(ep);
            await reporting.fanMode(ep);
        } catch {}
        await ep.read('genBasic', ['swBuildId']);
        await ep.read('hvacThermostat', ['localTemp', 'occupiedHeatingSetpoint', 'systemMode']);
        try {
            await ep.read(MITSU_EXT_CLUSTER, ['vanePosition', 'mitsuDebugLed']);
        } catch {}
        await ep.read('hvacFanCtrl', ['fanMode']);
        const otaEp = device.getEndpoint(2);
        if (otaEp) {
            try {
                await otaEp.read('genOta', ['currentFileVersion']);
            } catch {}
        }
    },
};