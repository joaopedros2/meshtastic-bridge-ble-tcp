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
- **MQTT with Home Assistant discovery**: the bridge shows up as a device with BLE/Wi-Fi signal, connection state, reconnect counter, uptime and restart/reconnect buttons
- **OTA updates** from the browser
- mDNS (`meshtastic-bridge-ble-tcp.local`) so apps can discover the bridge
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

## Connecting

- **Meshtastic app:** connection type *Network*, address = the bridge IP (or pick it from the scan).
- **Home Assistant:** the [Meshtastic integration](https://github.com/meshtastic/home-assistant) with a TCP connection to the bridge IP, port 4403.
- **Python CLI:** `meshtastic --host <bridge-ip> --info`

## MQTT / Home Assistant

Enable it in **Settings → MQTT** (broker, port, user, password). Topics live under `meshtastic-bridge-ble-tcp/<chip-id>/`, discovery under `homeassistant/`.

Entities: Bluetooth connected, Bluetooth state, Bluetooth RSSI, Bluetooth connected for, Bluetooth reconnects, TCP clients, Node name, WiFi RSSI, IP address, Uptime, Free memory, plus Restart and Reconnect Bluetooth buttons. The device goes *unavailable* in HA when the bridge is offline.

## Security notes

- Port 4403 has no authentication (same as Meshtastic's own TCP API). Keep the bridge on a trusted network or behind a VPN.
- Set a page password in **Settings → Security**.
- MQTT is unencrypted (no TLS), intended for a local broker.

## License

MIT — see [LICENSE](LICENSE).

Meshtastic® is a registered trademark of Meshtastic LLC. This project is independent and not affiliated with or endorsed by Meshtastic LLC.

Developed by **joaopedros2**.
