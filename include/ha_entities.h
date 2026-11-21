#pragma once

// Home Assistant entities published over MQTT discovery. Changing an entity means a firmware update.
// The key is the unique id suffix: keep it to keep the HA entity and its history.
// topic: where the value is published -> "state" (bridge), "node" (node device) or "status" (online/offline).
// extra: HA config keys passed as they are (device_class, unit_of_measurement, icon, ...).
enum class HaComponent : uint8_t { Sensor, BinarySensor };  // not SENSOR/BIN: Arduino defines BIN

struct HaEntity {
  const char* key;    // also the JSON field it reads
  const char* name;
  const char* topic;
  HaComponent component;
  bool duration;      // seconds shown as 1d 02h 03m 04s
  bool diagnostic;    // listed under Diagnostic in HA
  const char* extra;  // JSON members without braces
};

static const HaEntity HA_ENTITIES[] = {
  {"ble_connected", "Bluetooth connected", "state", HaComponent::BinarySensor, false, false, R"("device_class":"connectivity")"},
  {"ble_state", "Bluetooth state", "state", HaComponent::Sensor, false, false, R"("device_class":"enum","options":["ready","connecting","failed","idle","not_configured","disabled"])"},
  {"ble_rssi", "Bluetooth RSSI", "state", HaComponent::Sensor, false, false, R"("device_class":"signal_strength","unit_of_measurement":"dBm","state_class":"measurement")"},
  {"ble_uptime", "Bluetooth connected for", "state", HaComponent::Sensor, true, false, R"("icon":"mdi:timer-outline")"},
  {"ble_reconnects", "Bluetooth reconnects", "state", HaComponent::Sensor, false, false, R"("state_class":"total_increasing","icon":"mdi:bluetooth-off")"},
  {"tcp_clients", "TCP clients", "state", HaComponent::Sensor, false, false, R"("state_class":"measurement","icon":"mdi:lan-connect")"},
  {"node_name", "Node Bluetooth name", "state", HaComponent::Sensor, false, true, R"("icon":"mdi:radio-tower")"},
  {"wifi_rssi", "WiFi RSSI", "state", HaComponent::Sensor, false, true, R"("device_class":"signal_strength","unit_of_measurement":"dBm","state_class":"measurement")"},
  {"ip", "IP address", "state", HaComponent::Sensor, false, true, R"("icon":"mdi:ip-network")"},
  {"uptime", "Uptime", "state", HaComponent::Sensor, true, true, R"("icon":"mdi:clock-outline")"},
  {"free_heap", "Free memory", "state", HaComponent::Sensor, false, true, R"("device_class":"data_size","unit_of_measurement":"kB","state_class":"measurement")"},
  {"status", "Status", "status", HaComponent::BinarySensor, false, true, R"("device_class":"connectivity")"},
  {"wifi_ssid", "WiFi SSID", "state", HaComponent::Sensor, false, true, R"("icon":"mdi:wifi")"},
  {"dns", "DNS address", "state", HaComponent::Sensor, false, true, R"("icon":"mdi:dns")"},
  {"wifi_mac", "WiFi MAC address", "state", HaComponent::Sensor, false, true, R"("icon":"mdi:network-outline")"},
  {"temperature", "Internal temperature", "state", HaComponent::Sensor, false, true, R"("device_class":"temperature","unit_of_measurement":"°C","state_class":"measurement")"},
  {"node_battery", "Battery", "node", HaComponent::Sensor, false, false, R"("device_class":"battery","unit_of_measurement":"%","state_class":"measurement")"},
  {"node_voltage", "Voltage", "node", HaComponent::Sensor, false, false, R"("device_class":"voltage","unit_of_measurement":"V","state_class":"measurement","suggested_display_precision":2)"},
  {"node_ch_util", "Channel utilization", "node", HaComponent::Sensor, false, false, R"("unit_of_measurement":"%","state_class":"measurement","icon":"mdi:signal-distance-variant")"},
  {"node_air_util", "Airtime TX", "node", HaComponent::Sensor, false, false, R"("unit_of_measurement":"%","state_class":"measurement","icon":"mdi:radio-tower")"},
  {"node_uptime", "Uptime", "node", HaComponent::Sensor, true, false, R"("icon":"mdi:clock-outline")"},
  {"node_online", "Online nodes", "node", HaComponent::Sensor, false, false, R"("state_class":"measurement","icon":"mdi:account-group")"},
  {"node_total", "Known nodes", "node", HaComponent::Sensor, false, false, R"("state_class":"measurement","icon":"mdi:account-multiple-outline")"},
  {"node_pkt_rx", "Packets RX", "node", HaComponent::Sensor, false, false, R"("state_class":"total_increasing","icon":"mdi:download-network")"},
  {"node_pkt_tx", "Packets TX", "node", HaComponent::Sensor, false, false, R"("state_class":"total_increasing","icon":"mdi:upload-network")"},
  {"node_pkt_rx_bad", "Packets RX bad", "node", HaComponent::Sensor, false, false, R"("state_class":"total_increasing","icon":"mdi:alert-circle-outline")"},
  {"node_noise_floor", "Noise floor", "node", HaComponent::Sensor, false, false, R"("device_class":"signal_strength","unit_of_measurement":"dBm","state_class":"measurement")"},
  {"node_rx_dupe", "Packets RX duplicate", "node", HaComponent::Sensor, false, true, R"("state_class":"total_increasing")"},
  {"node_tx_relay", "Packets relayed", "node", HaComponent::Sensor, false, true, R"("state_class":"total_increasing")"},
  {"node_tx_relay_canceled", "Relays canceled", "node", HaComponent::Sensor, false, true, R"("state_class":"total_increasing")"},
  {"node_tx_dropped", "Packets TX dropped", "node", HaComponent::Sensor, false, true, R"("state_class":"total_increasing")"},
  {"node_heap_free", "Free memory", "node", HaComponent::Sensor, false, true, R"("device_class":"data_size","unit_of_measurement":"kB","state_class":"measurement")"},
  {"node_heap_total", "Total memory", "node", HaComponent::Sensor, false, true, R"("device_class":"data_size","unit_of_measurement":"kB","state_class":"measurement")"},
  {"node_fw", "Firmware", "node", HaComponent::Sensor, false, true, R"("icon":"mdi:chip")"},
  {"node_hw", "Hardware", "node", HaComponent::Sensor, false, true, R"("icon":"mdi:router-wireless")"},
  {"node_role", "Role", "node", HaComponent::Sensor, false, true, R"("icon":"mdi:account-cog")"},
  {"node_radio", "Radio", "node", HaComponent::Sensor, false, true, R"("icon":"mdi:antenna")"},
  {"node_id", "ID", "node", HaComponent::Sensor, false, true, R"("icon":"mdi:identifier")"},
  {"node_long_name", "Long name", "node", HaComponent::Sensor, false, true, R"("icon":"mdi:card-account-details-outline")"},
  {"node_short_name", "Short name", "node", HaComponent::Sensor, false, true, R"("icon":"mdi:tag-outline")"},
  {"node_powered", "External power", "node", HaComponent::BinarySensor, false, true, R"("device_class":"plug")"},
};
