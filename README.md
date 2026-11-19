# Meshtastic Bridge BLE-TCP

ESP32 firmware that makes a **Bluetooth-only Meshtastic node** reachable over your network.

The ESP32 sits next to the node, connects to it over Bluetooth LE, and exposes the standard Meshtastic **TCP API on port 4403**. The Meshtastic apps, Home Assistant or any other client can then connect to the node by IP — at home or remotely via VPN — with no PC left running.

Built for nRF52 nodes such as the SenseCAP Solar Node, RAK4631, T-Echo or T1000-E, which have no Wi-Fi.

![Home](docs/home.png)

## Features

- **BLE → TCP bridge** on port 4403, compatible with the Meshtastic apps (Android, iOS, Windows), the Python CLI and Home Assistant
- **Up to 4 clients at once** (e.g. Home Assistant and the app); live mesh traffic goes to all, config requests are served one at a time
- **Web page** (EN / PT-PT, desktop and mobile): status, logs, Bluetooth node scan, Wi-Fi setup, network name, page login
- **First-time setup access point** — no credentials or node address are built into the firmware
- **MQTT with Home Assistant discovery**: bridge and node diagnostics (signal, link state, battery, channel utilization, nodes online, packet counters, noise floor…) plus Restart, Bluetooth reconnect and Bluetooth link controls; entities customizable by uploading `ha_mqtt_discovery.json`
- **OTA updates** from the browser
- mDNS (`meshtastic-bridge-ble-tcp-xxxx.local`) so apps can discover the bridge
- Node card on the status page (battery, name, firmware, radio)
- Automatic BLE reconnect with backoff and dead-link detection

![Settings](docs/settings.png)

## Hardware

Any ESP32 with **Wi-Fi + Bluetooth LE** and **at least 4 MB flash**:

| Chip | Supported |
|---|---|
| ESP32, ESP32-S3, ESP32-C3, ESP32-C6 | ✅ |
| ESP32-S2 (no Bluetooth), ESP32-H2 (no Wi-Fi) | ❌ |

Bluetooth range is the main limit: aim for a BLE RSSI better than about −85 dBm. A board with an external antenna (e.g. XIAO ESP32-S3) helps.

## Build and flash

Requires [PlatformIO](https://platformio.org/) (VS Code extension or CLI).

```bash
git clone https://github.com/joaopedros2/meshtastic-bridge-ble-tcp.git
cd meshtastic-bridge-ble-tcp
pio run -t upload
pio device monitor
```

The default environment targets `esp32dev`. For other chips, change `board` in `platformio.ini` (e.g. `esp32-s3-devkitc-1`, `esp32-c3-devkitm-1`) and add `-DARDUINO_USB_CDC_ON_BOOT=1` for boards with native USB.

> **Windows:** keep the project in a path without accents or spaces (e.g. `C:\projects\`), otherwise the linker fails.

Later updates can be done from **Settings → Firmware** with `.pio/build/esp32dev/firmware.bin`.

## First-time setup

1. Power the ESP32. It opens the Wi-Fi network **`meshtastic-bridge-ble-tcp-XXXX`** (password `meshtastic`).
2. Join it — the setup page opens on its own (otherwise go to `http://192.168.4.1`).
3. **Settings → Wi-Fi**: pick your network and save. The bridge restarts and joins it.
4. **Settings → Bluetooth node**: scan, pick your node, enter its PIN and save.
   Nodes with a screen show a random PIN — set them to **Fixed PIN** in the Meshtastic app first (default `123456`).
5. Make sure no phone or PC is connected to the node over Bluetooth: a node accepts only one BLE connection.

### Several bridges

Use one bridge per node. Each one gets its own name from the chip id (`XXXX`, the same as its setup network):
`meshtastic-bridge-ble-tcp-xxxx` on the network and `.local`, and **Meshtastic Bridge BLE-TCP XXXX** in Home Assistant.
To use a name of your own (e.g. `bridge-solar-roof`), set it in **Settings → Device** — ideally before enabling MQTT, so the HA entity ids are created with it. MQTT topics and discovery ids always include the chip id, so bridges never clash.

## Connecting

- **Meshtastic app:** connection type *Network*, address = the bridge IP (or pick it from the scan).
- **Home Assistant:** the [Meshtastic integration](https://github.com/meshtastic/home-assistant) with a TCP connection to the bridge IP, port 4403.
- **Python CLI:** `meshtastic --host <bridge-ip> --info`

## MQTT / Home Assistant

Enable it in **Settings → MQTT** (broker, port, user, password). Topics live under `meshtastic-bridge-ble-tcp/<chip-id>/`, discovery under `homeassistant/`.

Two devices appear in Home Assistant:

- **The bridge** (`Meshtastic Bridge BLE-TCP XXXX`): Bluetooth connected, Bluetooth state, Bluetooth RSSI, Bluetooth connected for, Bluetooth reconnects, TCP clients; controls: Restart, Bluetooth reconnect (buttons), Bluetooth link (switch); diagnostics: status, node Bluetooth name, Wi-Fi SSID, Wi-Fi RSSI, IP, uptime, free memory, DNS address, Wi-Fi MAC address, internal temperature.
- **The node** (`Meshtastic Node 8903`, from the last 4 digits of its node id), linked to the bridge (model, firmware and node id as serial number): position (device tracker, when the node shares one), battery, voltage, channel utilization, airtime TX, uptime, online / known nodes, packets RX / TX / RX bad, noise floor; diagnostics: packets RX duplicate, packets relayed, relays canceled, packets TX dropped, free / total memory, firmware, hardware, role, radio, ID, long name, short name, external power.

Ids come from hardware, never from names: the bridge uses its chip id (`mbb_87ae18_…`, `sensor.meshtastic_bridge_87ae18_…`) and the node its node number (`mbn_3f7d8903_…`, `sensor.meshtastic_3f7d8903_…`). Renaming the node in the Meshtastic app only changes the *Long name* / *Short name* entities; device name, entity ids, history, dashboards and automations stay the same. If the bridge is pointed at another node, the old node's device is removed.

Node values come from the telemetry the node sends to its client (every minute) and its local stats (every 15 min), so they update while a client (app, Home Assistant…) is connected. Entities go *unavailable* in HA when the bridge is offline.

### Customizing the entities (`ha_mqtt_discovery.json`)

In **Settings → MQTT → Home Assistant discovery**:

1. **Download current** gives the file in use (the firmware default the first time).
2. Edit it: rename entities, change icons and units, move them to *Diagnostic*, remove the ones you don't want, or add entities for fields that have none (e.g. `node_long_name`).
3. **Upload and publish** validates it, stores it on the ESP32 (kept across firmware updates) and republishes; entities removed from the file are deleted in HA.
4. **Restore default** goes back to the firmware's entities.

```json
{"key": "node_battery", "name": "Node battery", "device_class": "battery",
 "unit_of_measurement": "%", "state_class": "measurement"}
```

| Key | Meaning |
|---|---|
| `key` | Unique id suffix (`a-z`, `0-9`, `_`). Keep it to keep the HA entity and its history |
| `name` | Entity name in HA |
| `field` | Field it reads (defaults to `key`). The list is at `/api/mqtt/fields`; `node_*` fields put the entity on the node's device |
| `component` | `sensor` (default) or `binary_sensor` |
| `format` | `duration`: seconds shown as `1d 02h 03m 04s` |
| `diagnostic` | `true` lists it under *Diagnostic* |

Any other key (`unit_of_measurement`, `device_class`, `state_class`, `icon`, `value_template`…) is passed to Home Assistant unchanged. Entries with an unknown field or an invalid key are ignored and counted on the page. The buttons, the Bluetooth link switch and the node position are part of the firmware and not in the file. The file only changes how existing data is presented; reading new data from the node needs a firmware update.

### HTTP API

`GET /api/status` returns everything the bridge knows, including every value read from the node (identity, radio, battery, channel use, uptime and the local stats: nodes, packet counters, noise floor, free memory). Fields that have not been received yet are `null`.

## Security notes

- Port 4403 has no authentication (same as Meshtastic's own TCP API). Keep the bridge on a trusted network or behind a VPN.
- Set a page password in **Settings → Security**.
- MQTT is unencrypted (no TLS), intended for a local broker.

## License

MIT — see [LICENSE](LICENSE).

Meshtastic® is a registered trademark of Meshtastic LLC. This project is independent and not affiliated with or endorsed by Meshtastic LLC.

Developed by **joaopedros2**.
