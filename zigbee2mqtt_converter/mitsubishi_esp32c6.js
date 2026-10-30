const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const tz = require('zigbee-herdsman-converters/converters/toZigbee');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');
const m = require('zigbee-herdsman-converters/lib/modernExtend');
const e = exposes.presets;
const ea = exposes.access;

const VANE_OPTIONS = ['auto', '1', '2', '3', '4', '5', 'swing'];
const MITSU_EXT_CLUSTER = 'manuSpecificMitsubishiHvac';

function swBuildIdToFileVersion(swBuildId) {
    if (!swBuildId || typeof swBuildId !== 'string') return null;
    const parts = swBuildId.split('.');
    if (parts.length !== 3) return null;
    const nums = parts.map((p) => parseInt(p, 10));
    if (nums.some((n) => Number.isNaN(n))) return null;
    return (nums[0] << 16) | (nums[1] << 8) | nums[2];
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
        const value = Math.round(Number(msg.data.occupiedHeatingSetpoint)) / 100;
        return {
            occupied_heating_setpoint: value,
            current_heating_setpoint: value,
        };
    },
};

const tzSetpoint = {
    key: ['occupied_heating_setpoint'],
    convertSet: async (entity, key, value, meta) => {
        const rounded = Math.round(Number(value) * 2) / 2;
        const centi = Math.round(rounded * 100);
        await entity.write('hvacThermostat', {occupiedHeatingSetpoint: centi});
        return {
            state: {
                occupied_heating_setpoint: rounded,
                current_heating_setpoint: rounded,
            },
        };
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

module.exports = {
    zigbeeModel: ['MZ-Zigbee-C6'],
    model: 'MZ-Zigbee-C6',
    vendor: 'Mitsubishi',
    description: 'Mitsubishi HVAC - passerelle ESP32-C6 Zigbee',
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
    fromZigbee: [fzFirmware, fz.thermostat, fzSetpoint, fz.fan, fzVane],
    toZigbee: [tz.thermostat_system_mode, tzSetpoint, tz.fan_mode, tzVane],
    exposes: [
        e
            .climate()
            .withSystemMode(['off', 'heat', 'cool', 'auto', 'fan_only', 'dry'])
            .withSetpoint('occupied_heating_setpoint', 16, 31, 0.5)
            .withLocalTemperature()
            .withFanMode(['auto', 'low', 'medium', 'high', 'smart']),
        e
            .enum('vane_position', ea.ALL, VANE_OPTIONS)
            .withDescription('Inclinaison des pales (AUTO, 1-5, SWING)'),
    ],
    configure: async (device, coordinatorEndpoint) => {
        const ep = device.getEndpoint(1);
        for (const cluster of ['hvacThermostat', 'hvacFanCtrl', MITSU_EXT_CLUSTER]) {
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
