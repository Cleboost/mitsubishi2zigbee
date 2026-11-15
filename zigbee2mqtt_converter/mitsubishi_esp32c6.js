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

const OPERATING_ATTR = '0x0401';
const DEBUG_LED_ATTR = '0x0402';

const SYSTEM_MODE_FROM_ZCL = {
    0: 'off',
    1: 'auto',
    3: 'cool',
    4: 'heat',
    7: 'fan_only',
    8: 'dry',
};

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
    cluster: 'hvacThermostat',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        if (msg.data[DEBUG_LED_ATTR] === undefined) return;
        return {debug: msg.data[DEBUG_LED_ATTR] === 1};
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
        await entity.write('hvacThermostat', {
            [DEBUG_LED_ATTR]: {value: on ? 1 : 0, type: 0x20},
        });
        return {state: {debug: on}};
    },
    convertGet: async (entity, key, meta) => {
        await entity.read('hvacThermostat', [DEBUG_LED_ATTR]);
    },
};

module.exports = {
    zigbeeModel: ['Mitsubishi2Zigbee', 'MZ-Zigbee-C6'],
    model: 'Mitsubishi2Zigbee',
    vendor: 'Cleboost',
    description: 'Cleboost ESP32-C6 Zigbee gateway for Mitsubishi CN105',
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
            },
            commands: {},
            commandsResponse: {},
        }),
    ],
    fromZigbee: [fzFirmware, fzRunningState, fz.thermostat, fzSetpoint, fz.fan, fzVane, fzDebug],
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
            .withFanMode(['auto', 'low', 'medium', 'high', 'smart'])
            .withRunningState(['idle', 'heat', 'cool', 'dry', 'fan_only'], ea.STATE),
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
        await ep.read('hvacThermostat', [
            'localTemp',
            'occupiedHeatingSetpoint',
            'systemMode',
            OPERATING_ATTR,
            DEBUG_LED_ATTR,
        ]);
        try {
            await ep.configureReporting('hvacThermostat', [
                {
                    attribute: {ID: 0x0401, type: 0x20},
                    minimumReportInterval: 5,
                    maximumReportInterval: 60,
                    reportableChange: 1,
                },
            ]);
        } catch {}
        await ep.read('hvacFanCtrl', ['fanMode']);
        try {
            await ep.read(MITSU_EXT_CLUSTER, ['vanePosition']);
        } catch {}
        const otaEp = device.getEndpoint(2);
        if (otaEp) {
            try {
                await otaEp.read('genOta', ['currentFileVersion']);
            } catch {}
        }
    },
};