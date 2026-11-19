#pragma once

// Default Home Assistant MQTT discovery, served as ha_mqtt_discovery.json.
// Entities reading node fields (node_*) go to the node's own HA device, named after the node;
// the others go to the bridge's device. Buttons, switch and the node position are built in.
// A file uploaded on the Settings page replaces it (stored in LittleFS, kept across OTA).
//
// Per entity:
//   key         unique id suffix (a-z, 0-9, _); keep it to keep the HA entity and its history
//   name        entity name in HA
//   field       JSON field it reads (defaults to key); see /api/mqtt/fields for the list
//   component   "sensor" (default) or "binary_sensor"
//   format      "duration" = seconds shown as 1d 02h 03m 04s
//   diagnostic  true = listed under Diagnostic in HA
// Any other key (unit_of_measurement, device_class, state_class, icon, ...) goes to HA unchanged.
static const char DISCOVERY_DEFAULT[] PROGMEM = R"JSON({
  "version": 1,
  "entities": [
    {"key": "ble_connected", "component": "binary_sensor", "name": "Bluetooth connected", "device_class": "connectivity"},
    {"key": "ble_state", "name": "Bluetooth state", "device_class": "enum",
     "options": ["ready", "connecting", "failed", "idle", "not_configured", "disabled"]},
    {"key": "ble_rssi", "name": "Bluetooth RSSI", "device_class": "signal_strength", "unit_of_measurement": "dBm", "state_class": "measurement"},
    {"key": "ble_uptime", "name": "Bluetooth connected for", "format": "duration", "icon": "mdi:timer-outline"},
    {"key": "ble_reconnects", "name": "Bluetooth reconnects", "state_class": "total_increasing", "icon": "mdi:bluetooth-off"},
    {"key": "tcp_clients", "name": "TCP clients", "state_class": "measurement", "icon": "mdi:lan-connect"},
    {"key": "node_name", "name": "Node Bluetooth name", "icon": "mdi:radio-tower", "diagnostic": true},
    {"key": "wifi_rssi", "name": "WiFi RSSI", "device_class": "signal_strength", "unit_of_measurement": "dBm", "state_class": "measurement", "diagnostic": true},
    {"key": "ip", "name": "IP address", "icon": "mdi:ip-network", "diagnostic": true},
    {"key": "uptime", "name": "Uptime", "format": "duration", "icon": "mdi:clock-outline", "diagnostic": true},
    {"key": "free_heap", "name": "Free memory", "device_class": "data_size", "unit_of_measurement": "kB", "state_class": "measurement", "diagnostic": true},
    {"key": "status", "component": "binary_sensor", "name": "Status", "device_class": "connectivity", "diagnostic": true},
    {"key": "wifi_ssid", "name": "WiFi SSID", "icon": "mdi:wifi", "diagnostic": true},
    {"key": "dns", "name": "DNS address", "icon": "mdi:dns", "diagnostic": true},
    {"key": "wifi_mac", "name": "WiFi MAC address", "icon": "mdi:network-outline", "diagnostic": true},
    {"key": "temperature", "name": "Internal temperature", "device_class": "temperature", "unit_of_measurement": "°C", "state_class": "measurement", "diagnostic": true},

    {"key": "node_battery", "name": "Battery", "device_class": "battery", "unit_of_measurement": "%", "state_class": "measurement"},
    {"key": "node_voltage", "name": "Voltage", "device_class": "voltage", "unit_of_measurement": "V", "state_class": "measurement", "suggested_display_precision": 2},
    {"key": "node_ch_util", "name": "Channel utilization", "unit_of_measurement": "%", "state_class": "measurement", "icon": "mdi:signal-distance-variant"},
    {"key": "node_air_util", "name": "Airtime TX", "unit_of_measurement": "%", "state_class": "measurement", "icon": "mdi:radio-tower"},
    {"key": "node_uptime", "name": "Uptime", "format": "duration", "icon": "mdi:clock-outline"},
    {"key": "node_online", "name": "Online nodes", "state_class": "measurement", "icon": "mdi:account-group"},
    {"key": "node_total", "name": "Known nodes", "state_class": "measurement", "icon": "mdi:account-multiple-outline"},
    {"key": "node_pkt_rx", "name": "Packets RX", "state_class": "total_increasing", "icon": "mdi:download-network"},
    {"key": "node_pkt_tx", "name": "Packets TX", "state_class": "total_increasing", "icon": "mdi:upload-network"},
    {"key": "node_pkt_rx_bad", "name": "Packets RX bad", "state_class": "total_increasing", "icon": "mdi:alert-circle-outline"},
    {"key": "node_noise_floor", "name": "Noise floor", "device_class": "signal_strength", "unit_of_measurement": "dBm", "state_class": "measurement"},
    {"key": "node_rx_dupe", "name": "Packets RX duplicate", "state_class": "total_increasing", "diagnostic": true},
    {"key": "node_tx_relay", "name": "Packets relayed", "state_class": "total_increasing", "diagnostic": true},
    {"key": "node_tx_relay_canceled", "name": "Relays canceled", "state_class": "total_increasing", "diagnostic": true},
    {"key": "node_tx_dropped", "name": "Packets TX dropped", "state_class": "total_increasing", "diagnostic": true},
    {"key": "node_heap_free", "name": "Free memory", "device_class": "data_size", "unit_of_measurement": "kB", "state_class": "measurement", "diagnostic": true},
    {"key": "node_heap_total", "name": "Total memory", "device_class": "data_size", "unit_of_measurement": "kB", "state_class": "measurement", "diagnostic": true},
    {"key": "node_fw", "name": "Firmware", "icon": "mdi:chip", "diagnostic": true},
    {"key": "node_hw", "name": "Hardware", "icon": "mdi:router-wireless", "diagnostic": true},
    {"key": "node_role", "name": "Role", "icon": "mdi:account-cog", "diagnostic": true},
    {"key": "node_radio", "name": "Radio", "icon": "mdi:antenna", "diagnostic": true},
    {"key": "node_id", "name": "ID", "icon": "mdi:identifier", "diagnostic": true},
    {"key": "node_long_name", "name": "Long name", "icon": "mdi:card-account-details-outline", "diagnostic": true},
    {"key": "node_short_name", "name": "Short name", "icon": "mdi:tag-outline", "diagnostic": true},
    {"key": "node_powered", "component": "binary_sensor", "name": "External power", "device_class": "plug", "diagnostic": true}
  ]
}
)JSON";
