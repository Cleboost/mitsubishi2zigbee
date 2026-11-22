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
/** ZCL cooling occupied setpoint on device (dummy high; satisfies dead-band vs heating) */
const ZCL_COOL_SETPOINT_DUMMY = 3200;

const SYSTEM_MODE_FROM_ZCL = {
    0: 'off',
    1: 'auto',
    3: 'cool',
    4: 'heat',
    7: 'fan_only',
    8: 'dry',
};

function zclSystemModeToString(mode) {
    if (typeof mode === 'number') return SYSTEM_MODE_FROM_ZCL[mode];
    return mode;
}

/** Herdsman may use name, hex string, or numeric attr id in msg.data. */
function mitsuAttrU8(msg, name, attrId) {
    const id = attrId;
    return msg.data[name] ?? msg.data[id] ?? msg.data[`0x${id.toString(16).padStart(4, '0')}`];
}

function swBuildIdToFileVersion(swBuildId) {
    if (!swBuildId || typeof swBuildId !== 'string') return null;
    const clean = swBuildId.trim().replace(/^[^\d]+/, '');
    const parts = clean.split('.');
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

const fzThermostat = {
    cluster: 'hvacThermostat',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        const result = {};
        if (msg.data.localTemp !== undefined) {
            result.local_temperature = Math.round(Number(msg.data.localTemp) / 10) / 10;
        }
        if (msg.data.systemMode !== undefined) {
            result.system_mode = zclSystemModeToString(msg.data.systemMode);
        }
        return Object.keys(result).length ? result : undefined;
    },
};

const fzFirmware = {
    cluster: 'genBasic',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        if (msg.data.swBuildId === undefined) return;
        const cleanVer = String(msg.data.swBuildId).trim().replace(/^[^\d]+/, '');
        const fileVersion = swBuildIdToFileVersion(cleanVer);
        const result = {software_build_id: cleanVer || msg.data.swBuildId};
        if (fileVersion != null) {
            const prevUpdate = meta?.state?.update;
            const latest =
                prevUpdate?.latest_version != null && prevUpdate.latest_version > fileVersion
                    ? prevUpdate.latest_version
                    : fileVersion;
            result.update = {
                ...prevUpdate,
                installed_version: fileVersion,
                latest_version: latest,
                state: latest > fileVersion ? 'available' : (prevUpdate?.state ?? 'idle'),
            };
        }
        return result;
    },
};

const fzVane = {
    cluster: MITSU_EXT_CLUSTER,
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        const idx = mitsuAttrU8(msg, 'vanePosition', 0x0000);
        if (idx === undefined) return;
        return {swing_mode: VANE_OPTIONS[idx] ?? 'auto'};
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
        const heatSp = rounded * 100;
        await entity.write('hvacThermostat', {
            occupiedHeatingSetpoint: heatSp,
            occupiedCoolingSetpoint: ZCL_COOL_SETPOINT_DUMMY,
        });
        return {state: setpointState(rounded)};
    },
    convertGet: async (entity, key, meta) => {
        await entity.read('hvacThermostat', ['occupiedHeatingSetpoint']);
    },
};

const tzVane = {
    key: ['swing_mode'],
    convertSet: async (entity, key, value, meta) => {
        const idx = VANE_OPTIONS.indexOf(value);
        if (idx < 0) return;
        await entity.write(MITSU_EXT_CLUSTER, {vanePosition: idx});
        return {state: {swing_mode: value}};
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

async function readFullDeviceState(ep) {
    await ep.read('hvacThermostat', [
        'localTemp',
        'occupiedHeatingSetpoint',
        'systemMode',
    ]);
    await ep.read('hvacFanCtrl', ['fanMode']);
    try {
        await ep.read(MITSU_EXT_CLUSTER, ['vanePosition', 'mitsuDebugLed']);
    } catch {}
}

module.exports = {
    zigbeeModel: ['Mitsubishi2Zigbee', 'MZ-Zigbee-C6'],
    model: 'Mitsubishi2Zigbee',
    vendor: 'Cleboost',
    description: 'Cleboost ESP32-C6 Zigbee gateway for Mitsubishi CN105',
    icon: 'https://raw.githubusercontent.com/Cleboost/mitsubishi2zigbee/main/assets/mitsubishi2zigbee.png',
    ota: true,
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
        fzThermostat,
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
            .withFanMode(['auto', 'low', 'medium', 'high', 'smart'])
            .withSwingMode(VANE_OPTIONS),
        e
            .binary('debug', ea.ALL, true, false)
            .withDescription(
                'Status LED: true (default) = normal blue/green pairing patterns; false = off',
            ),
    ],
    meta: {
        disableDefaultResponse: false,
    },
    onEvent: async (type, data, device, options, state, logger) => {
        if (type !== 'deviceAnnounce' && type !== 'start') return;
        const ep = device.getEndpoint(1);
        if (!ep) return;
        try {
            await readFullDeviceState(ep);
            logger.info('Mitsubishi2Zigbee: full state read after ' + type);
        } catch (err) {
            logger.warning(`Mitsubishi2Zigbee: read after ${type} failed: ${err}`);
        }
    },
    configure: async (device, coordinatorEndpoint, logger) => {
        const ep = device.getEndpoint(1);
        for (const cluster of HVAC_CLUSTERS) {
            try {
                await ep.bind(cluster, coordinatorEndpoint);
            } catch (e) {
                logger?.debug?.(`bind ${cluster}: ${e}`);
            }
        }
        const tryReporting = async (label, fn) => {
            try {
                await fn();
            } catch (e) {
                logger?.warning?.(`reporting (${label}): ${e}`);
            }
        };
        await tryReporting('localTemp', () => reporting.thermostatTemperature(ep));
        await tryReporting('occupiedHeatingSetpoint', () =>
            reporting.thermostatOccupiedHeatingSetpoint(ep),
        );
        await tryReporting('fanMode', () => reporting.fanMode(ep));
        await tryReporting('systemMode', () =>
            ep.configureReporting('hvacThermostat', [
                {
                    attribute: 'systemMode',
                    minimumReportInterval: 1,
                    maximumReportInterval: 300,
                    reportableChange: 1,
                },
            ]),
        );
        await tryReporting('vanePosition', () =>
            ep.configureReporting(MITSU_EXT_CLUSTER, [
                {
                    attribute: 'vanePosition',
                    minimumReportInterval: 1,
                    maximumReportInterval: 600,
                    reportableChange: 1,
                },
            ]),
        );
        await readFullDeviceState(ep);
        await ep.read('genBasic', ['swBuildId']);
    },
};