// Convertisseur Zigbee2MQTT pour mitsubishi2zigbee (ESP32-C6)
// Fichier à placer dans /config/zigbee2mqtt/mitsubishi_esp32c6.js
// Puis ajouter dans configuration.yaml :
//   external_converters:
//     - mitsubishi_esp32c6.js

const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const tz = require('zigbee-herdsman-converters/converters/toZigbee');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const e = exposes.presets;
const ea = exposes.access;

// Noms des positions de pales (index 0-6 = attribut 0x0400 du cluster thermostat)
const VANE_OPTIONS = ['auto', '1', '2', '3', '4', '5', 'swing'];

const fzVane = {
    cluster: 'hvacThermostat',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        if (msg.data['0x0400'] !== undefined) {
            const idx = msg.data['0x0400'];
            return {vane_position: VANE_OPTIONS[idx] ?? 'auto'};
        }
    },
};

const tzVane = {
    key: ['vane_position'],
    convertSet: async (entity, key, value, meta) => {
        const idx = VANE_OPTIONS.indexOf(value);
        if (idx < 0) return;
        await entity.write('hvacThermostat', {'0x0400': {value: idx, type: 0x20}});
        return {state: {vane_position: value}};
    },
    convertGet: async (entity, key, meta) => {
        await entity.read('hvacThermostat', ['0x0400']);
    },
};

const definition = {
    zigbeeModel: ['MZ-Zigbee-C6'],
    model: 'MZ-Zigbee-C6',
    vendor: 'Mitsubishi',
    description: 'Mitsubishi HVAC — passerelle ESP32-C6 Zigbee',
    fromZigbee: [fz.thermostat, fz.fan, fzVane],
    toZigbee: [
        tz.thermostat_system_mode,
        tz.thermostat_occupied_heating_setpoint,
        tz.thermostat_occupied_cooling_setpoint,
        tz.fan_mode,
        tzVane,
    ],
    exposes: [
        e.climate()
            .withSystemMode(['off', 'heat', 'cool', 'auto', 'fan_only', 'dry'])
            .withSetpoint('occupied_heating_setpoint', 16, 31, 0.5)
            .withLocalTemperature()
            .withFanMode(['auto', 'low', 'medium', 'high', 'smart']),
        e.enum('vane_position', ea.ALL, VANE_OPTIONS)
            .withDescription('Inclinaison des pales (AUTO, 1-5, SWING)'),
    ],
    configure: async (device, coordinatorEndpoint) => {
        const ep = device.getEndpoint(1);
        await ep.bind('hvacThermostat', coordinatorEndpoint);
        await ep.bind('hvacFanCtrl', coordinatorEndpoint);
        await ep.read('hvacThermostat', [
            'localTemp', 'occupiedHeatingSetpoint', 'systemMode',
        ]);
        await ep.read('hvacFanCtrl', ['fanMode']);
    },
};

module.exports = definition;
