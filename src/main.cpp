// Meshtastic Bridge BLE-TCP
// The ESP32 connects to a Meshtastic node over BLE and exposes the standard
// stream API on TCP port 4403, so the Meshtastic apps can connect over WiFi.
// A web page on port 80 shows status/logs and handles WiFi, BLE target and OTA.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Update.h>
#include <NimBLEDevice.h>
#include <PubSubClient.h>
#include <vector>
#include <stdarg.h>
#include <errno.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include "web_page.h"
#include "node_names.h"
#include "ha_entities.h"

// No WiFi network or BLE node is built in: on first boot the bridge opens a
// setup access point and everything is configured on the web page (stored in NVS).
static constexpr uint16_t TCP_PORT = 4403;          // Meshtastic stream API port
static constexpr uint32_t DEFAULT_BLE_PIN = 123456;  // Meshtastic default fixed PIN
// Project name in one place: page title, logs, mDNS, HA device model
static const char* PROJECT_NAME = "Meshtastic Bridge BLE-TCP";
static const char* PROJECT_SLUG = "meshtastic-bridge-ble-tcp";  // hostname, AP, MQTT base topic
static const char* LEGACY_SLUG = "meshtastic-ble-bridge";       // pre-1.3.2 default, migrated on boot

static const char* FW_VERSION = "1.3.5";
static const char* AP_PASSWORD = "meshtastic";  // setup network, min 8 chars (WPA2)

// Meshtastic GATT UUIDs
static const NimBLEUUID SERVICE_UUID("6ba1b218-15a8-461f-9fa8-5dcae273eafd");
static const NimBLEUUID TO_RADIO_UUID("f75c76d2-129e-4dad-a1dd-7866124401e7");
static const NimBLEUUID FROM_RADIO_UUID("2c55e69e-4993-11ed-b878-0242ac120002");
static const NimBLEUUID FROM_NUM_UUID("ed9da18c-a800-4f66-a670-aa7547e34453");

// Stream framing: 0x94 0xC3 <len MSB> <len LSB> <protobuf>
static constexpr uint8_t START1 = 0x94;
static constexpr uint8_t START2 = 0xC3;
static constexpr size_t MAX_PAYLOAD = 512;

static constexpr uint32_t BLE_RETRY_MIN_MS = 5000;
static constexpr uint32_t BLE_RETRY_MAX_MS = 60000;
static constexpr uint32_t POLL_FALLBACK_MS = 2000;   // safety net if a notify is missed
static constexpr uint32_t WIFI_AP_FALLBACK_MS = 30000;
static constexpr uint8_t MAX_WRITE_FAILURES = 3;
static constexpr size_t MAX_CLIENTS = 4;
static constexpr uint32_t CONFIG_TIMEOUT_MS = 30000;  // max time one client may own the config stream
static constexpr uint32_t KEEPALIVE_MS = 15000;       // idle link check (no clients connected)
static constexpr uint32_t SEND_STALL_MS = 500;        // a client that can't take data for this long is dropped

// ---------- settings (NVS) ----------

struct Settings {
  String wifiSsid, wifiPass, bleMac, webUser, webPass, hostname;
  uint8_t bleAddrType;
  uint32_t blePin;
  // MQTT (Home Assistant discovery)
  bool bleEnabled;  // "Bluetooth link" switch: off = node left free for direct Bluetooth
  bool mqttEnabled;
  String mqttHost, mqttUser, mqttPass, mqttDiscovery, mqttBase;
  uint16_t mqttPort;
  uint16_t mqttInterval;  // seconds between periodic state publishes
};
static Settings cfg;
static Preferences prefs;

static void loadSettings() {
  prefs.begin("bridge", true);
  cfg.wifiSsid = prefs.getString("ssid", "");
  cfg.wifiPass = prefs.getString("pass", "");
  cfg.bleMac = prefs.getString("mac", "");
  cfg.bleAddrType = prefs.getUChar("mtype", BLE_ADDR_RANDOM);
  cfg.blePin = prefs.getUInt("pin", DEFAULT_BLE_PIN);
  cfg.webUser = prefs.getString("wuser", "admin");
  cfg.webPass = prefs.getString("wpass", "");
  cfg.hostname = prefs.getString("host", PROJECT_SLUG);
  cfg.bleEnabled = prefs.getBool("ble_en", true);
  cfg.mqttEnabled = prefs.getBool("mq_en", false);
  cfg.mqttHost = prefs.getString("mq_host", "");
  cfg.mqttPort = prefs.getUShort("mq_port", 1883);
  cfg.mqttUser = prefs.getString("mq_user", "");
  cfg.mqttPass = prefs.getString("mq_pass", "");
  cfg.mqttDiscovery = prefs.getString("mq_disc", "homeassistant");
  cfg.mqttBase = prefs.getString("mq_base", PROJECT_SLUG);
  cfg.mqttInterval = prefs.getUShort("mq_int", 30);
  prefs.end();
  // Old default names saved by earlier versions follow the new project name
  if (cfg.hostname == LEGACY_SLUG) cfg.hostname = PROJECT_SLUG;
  if (cfg.mqttBase == LEGACY_SLUG) cfg.mqttBase = PROJECT_SLUG;
}

static void saveSettings() {
  prefs.begin("bridge", false);
  prefs.putString("ssid", cfg.wifiSsid);
  prefs.putString("pass", cfg.wifiPass);
  prefs.putString("mac", cfg.bleMac);
  prefs.putUChar("mtype", cfg.bleAddrType);
  prefs.putUInt("pin", cfg.blePin);
  prefs.putString("wuser", cfg.webUser);
  prefs.putString("wpass", cfg.webPass);
  prefs.putString("host", cfg.hostname);
  prefs.putBool("ble_en", cfg.bleEnabled);
  prefs.putBool("mq_en", cfg.mqttEnabled);
  prefs.putString("mq_host", cfg.mqttHost);
  prefs.putUShort("mq_port", cfg.mqttPort);
  prefs.putString("mq_user", cfg.mqttUser);
  prefs.putString("mq_pass", cfg.mqttPass);
  prefs.putString("mq_disc", cfg.mqttDiscovery);
  prefs.putString("mq_base", cfg.mqttBase);
  prefs.putUShort("mq_int", cfg.mqttInterval);
  prefs.end();
}

// Seconds since boot from the 64-bit timer: millis() wraps after 49.7 days
static uint32_t uptimeSec() { return uint32_t(esp_timer_get_time() / 1000000ULL); }

// ---------- logging (serial + ring buffer for the web page) ----------

static constexpr size_t LOG_LINES = 60;
static String logBuf[LOG_LINES];
static size_t logHead = 0, logCount = 0;

static void logf(const char* fmt, ...) {
  char msg[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  uint32_t s = uptimeSec();
  char line[220];
  snprintf(line, sizeof(line), "%02lu:%02lu:%02lu %s", s / 3600, (s / 60) % 60, s % 60, msg);
  Serial.println(line);
  logBuf[logHead] = line;
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
}

static String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
    else out += c;
  }
  return out;
}

// ---------- state ----------

enum class BleState { NotConfigured, Idle, Connecting, Ready, Failed, Disabled };
static const char* bleStateName(BleState s) {
  switch (s) {
    case BleState::NotConfigured: return "not_configured";
    case BleState::Connecting: return "connecting";
    case BleState::Ready: return "ready";
    case BleState::Failed: return "failed";
    case BleState::Disabled: return "disabled";
    default: return "idle";
  }
}

static WebServer web(80);
static DNSServer dns;
static WiFiServer tcpServer(TCP_PORT);
// Defined in the TCP section; used by BLE state handling
static void closeAllClients(const char* reason);
static void endConfigSession();
static void startNextPendingConfig();
static void resetNodeInfo();

static NimBLEClient* bleClient = nullptr;
static NimBLERemoteCharacteristic* toRadio = nullptr;
static NimBLERemoteCharacteristic* fromRadio = nullptr;
static NimBLERemoteCharacteristic* fromNum = nullptr;

static BleState bleState = BleState::Idle;
static volatile bool bleReady = false;
static volatile bool readPending = false;  // set by FromNum notify / after writes
static uint32_t lastBleAttempt = 0;
static uint32_t bleRetryDelay = 0;          // 0 = try immediately
static int lastBleError = 0;
static const char* lastBleStage = "";
static const char* lastBleErrorKey = "";  // short code the web page translates, e.g. "pin_rejected"
static int bleRssi = 0;
static String bleName;  // the node's advertised Bluetooth name, e.g. "Meshtastic_1d7a"
static uint32_t lastRxMs = 0;
static uint32_t bleConnectedAt = 0;  // millis() when the current BLE link became ready
static uint32_t bleConnectCount = 0;  // successful connections since boot (reconnects = count - 1)
static uint8_t writeFailures = 0;
static volatile int disconnectReason = -1;  // set by BLE task, -1 = nothing pending
static volatile int authResult = -1;        // set by BLE task: 1 = encrypted, 0 = failed, -1 = nothing pending

static bool apActive = false;
static String apSsid;  // unique per chip, so several bridges don't collide
static String chipId;  // last 3 bytes of the chip MAC, e.g. "5b3c91"
static String chipSuffix;  // last 2 bytes, e.g. "3C91" - tells several bridges apart

// Default name carries the chip suffix, so several bridges never share a hostname / HA device name
static String defaultHostname() {
  String sfx = chipSuffix;
  sfx.toLowerCase();
  return String(PROJECT_SLUG) + "-" + sfx;  // e.g. meshtastic-bridge-ble-tcp-3c91
}

// Name shown in Home Assistant and to the apps (mDNS)
static String deviceName() {
  return cfg.hostname == defaultHostname() ? String(PROJECT_NAME) + " " + chipSuffix : cfg.hostname;
}
static uint32_t wifiDownSince = 0;
static uint32_t restartAt = 0;
static bool otaRejected = false;
static bool bridgeConfigPending = false;  // bridge wants its own config stream from the node

// ---------- BLE ----------

class BridgeClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*, int reason) override {
    disconnectReason = reason;  // logged from loop(): log buffer isn't thread-safe
    bleReady = false;
  }
  // Node uses a fixed PIN; we act as the keyboard side
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
    NimBLEDevice::injectPassKey(connInfo, cfg.blePin);
  }
  void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t) override {
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    authResult = connInfo.isEncrypted() ? 1 : 0;  // logged from loop()
  }
};
static BridgeClientCallbacks clientCallbacks;

static void onFromNum(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool) {
  readPending = true;  // never read inside the BLE host task
}

// Common NimBLE disconnect reasons (0x200 + HCI error code)
static const char* disconnectText(int reason) {
  switch (reason) {
    case 0x208: return "supervision timeout - weak signal or out of range";
    case 0x213: return "node closed the connection";
    case 0x216: return "closed by bridge";
    case 0x23e: return "connection failed to establish";
    default: return "see NimBLE/HCI error codes";
  }
}

// Classify NimBLE errors (host 0x0xx, HCI 0x2xx, SM local 0x4xx / peer 0x5xx) into causes a user can act on
struct BleErrorInfo { const char* key; const char* text; };
static BleErrorInfo classifyBleError(int err) {
  switch (err) {
    case 0x401: case 0x501:  // passkey entry failed
    case 0x404: case 0x504:  // confirm value failed: the two sides used different PINs
    case 0x40B: case 0x50B:  // DHKey check failed
    case 0x205:              // HCI authentication failure
      return {"pin_rejected", "PIN rejected by the node - check the PIN on both sides"};
    case 0x206:              // PIN or key missing: one side forgot the previous pairing
      return {"stale_pairing", "old pairing no longer valid - pairing again"};
    case 0x403: case 0x503: case 0x405: case 0x505:
      return {"pairing_unsupported", "node refused pairing - set Bluetooth to Fixed PIN"};
    case 13: case 0x208: case 0x23E:
      return {"out_of_range", "node not reachable - out of range or busy with another device"};
    case 0x213:
      return {"node_closed", "node closed the connection"};
    default:
      return {"unknown", "see NimBLE/HCI error codes"};
  }
}

static bool bleFail(const char* stage) {
  lastBleError = bleClient ? bleClient->getLastError() : -1;
  lastBleStage = stage;
  BleErrorInfo info = classifyBleError(lastBleError);
  lastBleErrorKey = info.key;
  bleState = BleState::Failed;
  bleRetryDelay = bleRetryDelay == 0 ? BLE_RETRY_MIN_MS : min(bleRetryDelay * 2, BLE_RETRY_MAX_MS);
  logf("[BLE] %s failed, err=%d (%s), retry in %lus", stage, lastBleError, info.text, bleRetryDelay / 1000);
  if (bleClient && bleClient->isConnected()) bleClient->disconnect();
  return false;
}

static bool connectNode() {
  if (cfg.bleMac.isEmpty()) {
    bleState = BleState::NotConfigured;
    return false;
  }
  bleState = BleState::Connecting;

  if (!bleClient) {
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(&clientCallbacks, false);
    bleClient->setConnectTimeout(5000);  // short, so the web page stays responsive
    // 30-50 ms interval, 6 s supervision timeout (default is much shorter):
    // rides out brief fades on weak links instead of dropping the connection
    bleClient->setConnectionParams(24, 40, 0, 600);
  }
  if (!bleClient->connect(NimBLEAddress(cfg.bleMac.c_str(), cfg.bleAddrType))) return bleFail("connect");
  // Meshtastic characteristics require an encrypted (paired) link
  if (!bleClient->secureConnection()) {
    bleFail("pairing");
    // A stale bond would keep failing: next attempt starts a fresh pairing with the configured PIN
    NimBLEDevice::deleteBond(NimBLEAddress(cfg.bleMac.c_str(), cfg.bleAddrType));
    return false;
  }

  NimBLERemoteService* svc = bleClient->getService(SERVICE_UUID);
  if (!svc) return bleFail("service lookup");
  toRadio = svc->getCharacteristic(TO_RADIO_UUID);
  fromRadio = svc->getCharacteristic(FROM_RADIO_UUID);
  fromNum = svc->getCharacteristic(FROM_NUM_UUID);
  if (!toRadio || !fromRadio || !fromNum || !fromNum->subscribe(true, onFromNum)) return bleFail("characteristics");

  bleRssi = bleClient->getRssi();
  // Same name the node shows in a Bluetooth scan (GAP Device Name, 0x1800/0x2A00)
  bleName = bleClient->getValue(NimBLEUUID((uint16_t)0x1800), NimBLEUUID((uint16_t)0x2A00)).c_str();
  logf("[BLE] ready: %s, MTU=%u, RSSI=%d", bleName.c_str(), bleClient->getMTU(), bleRssi);
  bleState = BleState::Ready;
  lastBleErrorKey = "";
  bleConnectedAt = millis();
  bleConnectCount++;
  bleRetryDelay = 0;
  writeFailures = 0;
  bleReady = true;
  bridgeConfigPending = true;  // learn the node's identity, radio and battery for the status page
  startNextPendingConfig();     // then serve clients that connected while BLE was down
  return true;
}

// Drop the current link and pairing, then reconnect with the new settings
static void applyBleTarget() {
  if (bleClient && bleClient->isConnected()) bleClient->disconnect();
  bleReady = false;
  endConfigSession();
  closeAllClients("BLE target changed");
  bleName = "";
  resetNodeInfo();
  NimBLEDevice::deleteAllBonds();
  bleState = !cfg.bleEnabled ? BleState::Disabled : cfg.bleMac.isEmpty() ? BleState::NotConfigured : BleState::Idle;
  bleRetryDelay = 0;
  lastBleAttempt = millis();
}

// "Bluetooth link" switch: off drops the link and stops reconnecting so a phone can
// use the node directly. Persisted, so it survives a restart.
static void setBleEnabled(bool on, const char* source) {
  if (cfg.bleEnabled == on) return;
  cfg.bleEnabled = on;
  saveSettings();
  if (!on) {
    bleState = BleState::Disabled;  // set first, so the link-loss path in manageBle() doesn't run
    bleReady = false;               // before endConfigSession(): no queued want_config gets sent
    endConfigSession();
    closeAllClients("Bluetooth link off");
    if (bleClient && bleClient->isConnected()) bleClient->disconnect();
  } else {
    bleState = cfg.bleMac.isEmpty() ? BleState::NotConfigured : BleState::Idle;
    bleRetryDelay = 0;  // reconnect right away
  }
  logf("[BLE] link %s (%s)%s", on ? "on" : "off", source, on ? "" : " - node free for direct Bluetooth");
}

static void manageBle() {
  static uint32_t lastCheck = 0;
  NimBLEScan* scan = NimBLEDevice::getScan();

  if (disconnectReason >= 0) {
    logf("[BLE] disconnected, reason=%d (%s)", disconnectReason, disconnectText(disconnectReason));
    disconnectReason = -1;
  }
  if (authResult >= 0) {
    logf("[BLE] auth %s", authResult ? "ok" : "FAILED");
    authResult = -1;
  }

  if (!cfg.bleEnabled) return;

  // Detect silent link loss and refresh RSSI
  if (bleReady && millis() - lastCheck > 5000) {
    lastCheck = millis();
    if (!bleClient->isConnected()) {
      logf("[BLE] link lost (silent)");
      bleReady = false;
    } else {
      bleRssi = bleClient->getRssi();
    }
  }
  if (bleReady) return;

  // Link just dropped: clients lost their node session, make them reconnect
  // (their fresh want_config is queued until BLE is back)
  if (bleState == BleState::Ready) {
    bleState = BleState::Idle;
    endConfigSession();
    closeAllClients("BLE link lost");
  }

  // Don't connect while a web-triggered scan is running
  if (scan->isScanning()) return;
  if (millis() - lastBleAttempt < bleRetryDelay) return;
  lastBleAttempt = millis();
  connectNode();
}

// ---------- TCP <-> BLE ----------
// Several TCP clients share one BLE link. Live mesh traffic is broadcast to all
// clients; the config stream (answer to want_config) goes only to the client
// that asked for it, and concurrent config requests are served one at a time.

struct FrameParser {
  uint8_t buf[MAX_PAYLOAD];
  size_t expected = 0, received = 0;
  uint8_t state = 0;  // 0=START1 1=START2 2=LEN_MSB 3=LEN_LSB 4=PAYLOAD
};

struct TcpSession {
  WiFiClient sock;
  String ip;  // captured at accept: remoteIP() reads 0.0.0.0 once the socket closes
  FrameParser parser;
  bool active = false;
  bool configPending = false;  // want_config waiting for its turn
  uint8_t pendingMsg[32];      // the queued want_config ToRadio (a few bytes)
  size_t pendingLen = 0;
};
static TcpSession sessions[MAX_CLIENTS];
static constexpr int CONFIG_OWNER_NONE = -1;
static constexpr int CONFIG_OWNER_BRIDGE = -2;  // the bridge itself, to learn about the node
static int configOwner = CONFIG_OWNER_NONE;     // session receiving the config stream
// want_config_id = 69420 (SPECIAL_NONCE_ONLY_CONFIG): config + own node only, no node DB,
// so it stays short even on a weak BLE link
static const uint8_t BRIDGE_WANT_CONFIG[] = {0x18, 0xAC, 0x9E, 0x04};
static uint32_t configStartedAt = 0;

// Protobuf field numbers (meshtastic/mesh.proto)
namespace pb {
  constexpr uint32_t FROM_RADIO_ID = 1;
  constexpr uint32_t TO_RADIO_WANT_CONFIG = 3;
  constexpr uint32_t TO_RADIO_DISCONNECT = 4;
  constexpr uint32_t FROM_RADIO_CONFIG_COMPLETE = 7;
}

// FromRadio variants that only appear in a config stream
static bool isConfigField(uint32_t f) {
  // my_info, node_info, config, config_complete_id, moduleConfig, channel, metadata, fileInfo, deviceuiConfig
  return f == 3 || f == 4 || f == 5 || f == 7 || f == 9 || f == 10 || f == 13 || f == 15 || f == 17;
}

static bool readVarint(const uint8_t* p, size_t len, size_t& i, uint64_t& out) {
  out = 0;
  for (int shift = 0; i < len && shift < 64; shift += 7) {
    uint8_t b = p[i++];
    out |= uint64_t(b & 0x7F) << shift;
    if (!(b & 0x80)) return true;
  }
  return false;
}

// First top-level field number, skipping `skipField`; 0 on parse error.
// Enough to identify which oneof variant a FromRadio/ToRadio carries.
static uint32_t firstField(const uint8_t* p, size_t len, uint32_t skipField) {
  size_t i = 0;
  while (i < len) {
    uint64_t key, v;
    if (!readVarint(p, len, i, key)) return 0;
    uint32_t field = key >> 3;
    if (field != skipField) return field;
    switch (key & 7) {
      case 0: if (!readVarint(p, len, i, v)) return 0; break;
      case 1: i += 8; break;
      case 2: if (!readVarint(p, len, i, v)) return 0; i += v; break;
      case 5: i += 4; break;
      default: return 0;
    }
  }
  return 0;
}

// ---------- node info (decoded from the traffic passing through) ----------

struct NodeStatus {
  uint32_t num = 0;
  String longName, shortName, fwVersion;
  int hwModel = -1, role = -1, region = -1, preset = -1;
  bool usePreset = true;
  int battery = -1;  // 101 = external power
  float voltage = NAN, chUtil = NAN, airUtil = NAN;
  uint32_t nodeUptime = 0, nodeUptimeAt = 0, metricsAt = 0;
  // LocalStats, sent by the node to its client every 15 min
  uint32_t onlineNodes = 0, totalNodes = 0, pktTx = 0, pktRx = 0, pktRxBad = 0;
  uint32_t rxDupe = 0, txRelay = 0, txRelayCanceled = 0, txDropped = 0, heapFree = 0;
  int32_t noiseFloor = 0;
  uint32_t heapTotal = 0;
  uint32_t statsAt = 0;  // millis() of the last LocalStats, 0 = none yet
  // Own position (GPS or fixed), 1e-7 degrees; posAt = 0 until one arrives
  int32_t latI = 0, lonI = 0, altitude = 0;
  uint32_t posAt = 0;
};
static NodeStatus node;
static void resetNodeInfo() { node = NodeStatus(); }

struct PbField {
  uint32_t num;
  uint8_t wireType;
  uint64_t value;        // varint / fixed32 / fixed64
  const uint8_t* data;   // length-delimited payload
  size_t len;
};

// Minimal protobuf walker: enough to read the few fields we display
static bool pbNext(const uint8_t* p, size_t len, size_t& i, PbField& f) {
  uint64_t key, l;
  if (i >= len || !readVarint(p, len, i, key)) return false;
  f.num = key >> 3;
  f.wireType = key & 7;
  f.value = 0;
  f.data = nullptr;
  f.len = 0;
  switch (f.wireType) {
    case 0: return readVarint(p, len, i, f.value);
    case 1: if (i + 8 > len) return false; memcpy(&f.value, p + i, 8); i += 8; return true;
    case 2:
      if (!readVarint(p, len, i, l) || i + l > len) return false;
      f.data = p + i; f.len = l; i += l;
      return true;
    case 5: { uint32_t x; if (i + 4 > len) return false; memcpy(&x, p + i, 4); f.value = x; i += 4; return true; }
    default: return false;
  }
}
static float pbFloat(const PbField& f) { uint32_t x = uint32_t(f.value); float r; memcpy(&r, &x, 4); return r; }
static String pbString(const PbField& f) {
  String s;
  s.reserve(f.len);
  for (size_t k = 0; k < f.len; k++) s += char(f.data[k]);
  return s;
}

static void parseDeviceMetrics(const uint8_t* p, size_t len) {
  size_t i = 0;
  PbField f;
  while (pbNext(p, len, i, f)) {
    switch (f.num) {
      case 1: node.battery = int(f.value); break;
      case 2: node.voltage = pbFloat(f); break;
      case 3: node.chUtil = pbFloat(f); break;
      case 4: node.airUtil = pbFloat(f); break;
      case 5: node.nodeUptime = uint32_t(f.value); node.nodeUptimeAt = millis(); break;
    }
  }
  node.metricsAt = millis();
}

static void parseLocalStats(const uint8_t* p, size_t len) {
  size_t i = 0;
  PbField f;
  while (pbNext(p, len, i, f)) {
    switch (f.num) {
      case 1: node.nodeUptime = uint32_t(f.value); node.nodeUptimeAt = millis(); break;
      case 2: node.chUtil = pbFloat(f); break;
      case 3: node.airUtil = pbFloat(f); break;
      case 4: node.pktTx = uint32_t(f.value); break;
      case 5: node.pktRx = uint32_t(f.value); break;
      case 6: node.pktRxBad = uint32_t(f.value); break;
      case 7: node.onlineNodes = uint32_t(f.value); break;
      case 8: node.totalNodes = uint32_t(f.value); break;
      case 9: node.rxDupe = uint32_t(f.value); break;
      case 10: node.txRelay = uint32_t(f.value); break;
      case 11: node.txRelayCanceled = uint32_t(f.value); break;
      case 12: node.heapTotal = uint32_t(f.value); break;
      case 13: node.heapFree = uint32_t(f.value); break;
      case 14: node.txDropped = uint32_t(f.value); break;
      case 15: node.noiseFloor = int32_t(int64_t(f.value)); break;  // int32: negatives are 64-bit varints
    }
  }
  node.statsAt = millis();
}

static void parseUser(const uint8_t* p, size_t len) {
  size_t i = 0;
  PbField f;
  while (pbNext(p, len, i, f)) {
    switch (f.num) {
      case 2: node.longName = pbString(f); break;
      case 3: node.shortName = pbString(f); break;
      case 5: node.hwModel = int(f.value); break;
      case 7: node.role = int(f.value); break;
    }
  }
}

// Position: latitude_i / longitude_i are sfixed32 (1e-7 degrees), altitude int32 (m)
static void parsePosition(const uint8_t* p, size_t len) {
  size_t i = 0;
  PbField f;
  int32_t lat = 0, lon = 0, alt = 0;
  while (pbNext(p, len, i, f)) {
    if (f.num == 1) lat = int32_t(uint32_t(f.value));
    else if (f.num == 2) lon = int32_t(uint32_t(f.value));
    else if (f.num == 3) alt = int32_t(int64_t(f.value));
  }
  if (lat == 0 && lon == 0) return;  // no fix / position not shared
  node.latI = lat;
  node.lonI = lon;
  node.altitude = alt;
  node.posAt = millis();
}

// NodeInfo: only our own node (my_info arrives first in a config stream)
static void parseNodeInfo(const uint8_t* p, size_t len) {
  size_t i = 0;
  PbField f, user = {}, position = {}, metrics = {};
  uint32_t num = 0;
  while (pbNext(p, len, i, f)) {
    if (f.num == 1) num = uint32_t(f.value);
    else if (f.num == 2) user = f;
    else if (f.num == 3) position = f;
    else if (f.num == 6) metrics = f;
  }
  if (!num || num != node.num) return;
  if (user.data) parseUser(user.data, user.len);
  if (position.data) parsePosition(position.data, position.len);
  if (metrics.data) parseDeviceMetrics(metrics.data, metrics.len);
}

static void parseConfig(const uint8_t* p, size_t len) {
  size_t i = 0, j;
  PbField f, g;
  while (pbNext(p, len, i, f)) {
    if (f.num == 6 && f.data) {  // LoRaConfig
      // proto3 omits zero values: start from them (use_preset false, LONG_FAST, UNSET)
      node.usePreset = false;
      node.preset = 0;
      node.region = 0;
      j = 0;
      while (pbNext(f.data, f.len, j, g)) {
        if (g.num == 1) node.usePreset = g.value != 0;
        else if (g.num == 2) node.preset = int(g.value);
        else if (g.num == 7) node.region = int(g.value);
      }
    } else if (f.num == 1 && f.data) {  // DeviceConfig
      node.role = 0;  // CLIENT when omitted
      j = 0;
      while (pbNext(f.data, f.len, j, g))
        if (g.num == 1) node.role = int(g.value);
    }
  }
}

static void parseMetadata(const uint8_t* p, size_t len) {
  size_t i = 0;
  PbField f;
  while (pbNext(p, len, i, f)) {
    if (f.num == 1) node.fwVersion = pbString(f);
    else if (f.num == 9) node.hwModel = int(f.value);
  }
}

// Own DeviceMetrics telemetry (battery, utilisation) arrives as a normal packet
static void parseMeshPacket(const uint8_t* p, size_t len) {
  size_t i = 0, j, k;
  PbField f, g, h;
  uint32_t from = 0;
  const uint8_t* payload = nullptr;
  size_t payloadLen = 0;
  uint32_t portnum = 0;
  while (pbNext(p, len, i, f)) {
    if (f.num == 1) from = uint32_t(f.value);
    else if (f.num == 4 && f.data) {  // Data
      j = 0;
      while (pbNext(f.data, f.len, j, g)) {
        if (g.num == 1) portnum = uint32_t(g.value);
        else if (g.num == 2) { payload = g.data; payloadLen = g.len; }
      }
    }
  }
  if (!payload || !node.num || from != node.num) return;  // only our own node's packets
  if (portnum == 3) return parsePosition(payload, payloadLen);  // POSITION_APP
  if (portnum != 67) return;                                    // TELEMETRY_APP
  k = 0;
  while (pbNext(payload, payloadLen, k, h)) {
    if (h.num == 2 && h.data) parseDeviceMetrics(h.data, h.len);      // DeviceMetrics
    else if (h.num == 6 && h.data) parseLocalStats(h.data, h.len);    // LocalStats
  }
}

static void inspectFromRadio(const uint8_t* p, size_t len) {
  size_t i = 0, j;
  PbField f, g;
  while (pbNext(p, len, i, f)) {
    switch (f.num) {
      case 2: if (f.data) parseMeshPacket(f.data, f.len); break;
      case 3:  // MyNodeInfo
        j = 0;
        while (f.data && pbNext(f.data, f.len, j, g))
          if (g.num == 1) node.num = uint32_t(g.value);
        break;
      case 4: if (f.data) parseNodeInfo(f.data, f.len); break;
      case 5: if (f.data) parseConfig(f.data, f.len); break;
      case 13: if (f.data) parseMetadata(f.data, f.len); break;
    }
  }
}

static size_t activeClientCount() {
  size_t n = 0;
  for (auto& s : sessions) n += s.active;
  return n;
}

// Non-blocking send with a short grace period: a stalled client (e.g. a dropped VPN)
// must not freeze the bridge. WiFiClient::write() can block for ~10 s.
static bool sendAll(int fd, const uint8_t* d, size_t n) {
  uint32_t t0 = millis();
  while (n) {
    int w = lwip_send(fd, d, n, MSG_DONTWAIT);
    if (w > 0) {
      d += w;
      n -= size_t(w);
    } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      return false;
    } else if (millis() - t0 > SEND_STALL_MS) {
      return false;
    } else {
      delay(5);
    }
  }
  return true;
}

static void closeSession(size_t i, const char* reason);

// Header and payload in one write -> one TCP segment; drops the client if it stalls
static void sendFramed(size_t idx, const uint8_t* data, size_t len) {
  uint8_t frame[4 + MAX_PAYLOAD + 4];
  if (len > sizeof(frame) - 4) return;
  frame[0] = START1;
  frame[1] = START2;
  frame[2] = uint8_t(len >> 8);
  frame[3] = uint8_t(len & 0xFF);
  memcpy(frame + 4, data, len);
  if (!sendAll(sessions[idx].sock.fd(), frame, len + 4)) closeSession(idx, "not reading, dropped");
}

static void writeToRadio(const uint8_t* data, size_t len) {
  if (!bleReady) return;
  if (toRadio->writeValue(data, len, true)) {
    writeFailures = 0;
    readPending = true;  // node usually has a reply queued
  } else if (++writeFailures >= MAX_WRITE_FAILURES) {
    logf("[BLE] %u write failures, reconnecting", writeFailures);
    bleClient->disconnect();
  }
}

// ---------- node administration (AdminMessage to our own node, ADMIN_APP) ----------
// From a local client (BLE) the node accepts admin without a session key. Field numbers: meshtastic/admin.proto
static const uint32_t ADMIN_REBOOT_SECONDS = 97;  // int32: reboot after N seconds

static void pbPutVarint(std::vector<uint8_t>& b, uint64_t v) {
  do {
    uint8_t c = v & 0x7F;
    v >>= 7;
    b.push_back(v ? c | 0x80 : c);
  } while (v);
}
static void pbPutTag(std::vector<uint8_t>& b, uint32_t field, uint8_t wire) { pbPutVarint(b, (uint64_t(field) << 3) | wire); }
static void pbPutBytes(std::vector<uint8_t>& b, uint32_t field, const std::vector<uint8_t>& v) {
  pbPutTag(b, field, 2);
  pbPutVarint(b, v.size());
  b.insert(b.end(), v.begin(), v.end());
}
static void pbPutFixed32(std::vector<uint8_t>& b, uint32_t field, uint32_t v) {
  pbPutTag(b, field, 5);
  for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (8 * i)));
}

// ToRadio{packet: MeshPacket{to: node, decoded: Data{portnum: ADMIN_APP, payload: AdminMessage{field: value}}}}
static bool sendNodeAdmin(uint32_t field, uint32_t value, const char* what) {
  if (!bleReady || !node.num) {
    logf("[NODE] %s: node not connected", what);
    return false;
  }
  std::vector<uint8_t> admin, data, packet, msg;
  pbPutTag(admin, field, 0);
  pbPutVarint(admin, value);
  pbPutTag(data, 1, 0);
  pbPutVarint(data, 6);  // PortNum ADMIN_APP
  pbPutBytes(data, 2, admin);
  pbPutFixed32(packet, 2, node.num);      // to: our own node
  pbPutBytes(packet, 4, data);            // decoded
  pbPutFixed32(packet, 6, esp_random());  // packet id
  pbPutBytes(msg, 1, packet);             // ToRadio.packet
  writeToRadio(msg.data(), msg.size());
  logf("[NODE] %s sent to !%08lx", what, (unsigned long)node.num);
  return true;
}

static bool rebootNode(const char* source) {
  logf("[NODE] restart requested (%s)", source);
  return sendNodeAdmin(ADMIN_REBOOT_SECONDS, 5, "restart");
}

static void startNextPendingConfig() {
  if (!bleReady || configOwner != CONFIG_OWNER_NONE) return;
  if (bridgeConfigPending) {  // the bridge's own short request goes first
    bridgeConfigPending = false;
    configOwner = CONFIG_OWNER_BRIDGE;
    configStartedAt = millis();
    writeToRadio(BRIDGE_WANT_CONFIG, sizeof(BRIDGE_WANT_CONFIG));
    return;
  }
  for (size_t i = 0; i < MAX_CLIENTS; i++) {
    TcpSession& s = sessions[i];
    if (!s.active || !s.configPending) continue;
    s.configPending = false;
    configOwner = int(i);
    configStartedAt = millis();
    logf("[TCP] config stream -> %s", s.ip.c_str());
    writeToRadio(s.pendingMsg, s.pendingLen);
    return;
  }
}

static void endConfigSession() {
  if (configOwner == CONFIG_OWNER_BRIDGE && node.num)
    logf("[NODE] %s (%s), fw %s", node.longName.c_str(), node.shortName.c_str(), node.fwVersion.c_str());
  configOwner = CONFIG_OWNER_NONE;
  startNextPendingConfig();
}

static void closeSession(size_t i, const char* reason) {
  TcpSession& s = sessions[i];
  if (!s.active) return;
  logf("[TCP] client %s closed (%s)", s.ip.c_str(), reason);
  s.sock.stop();
  s.active = false;
  s.configPending = false;
  // If it owned the config stream, the rest of that stream is dropped until
  // config_complete arrives (or the timeout frees the slot)
}

static void closeAllClients(const char* reason) {
  for (size_t i = 0; i < MAX_CLIENTS; i++) closeSession(i, reason);
}

// Route one FromRadio message to the right client(s)
static void dispatchFromRadio(const uint8_t* data, size_t len) {
  inspectFromRadio(data, len);  // keep node info (name, battery…) up to date
  uint32_t field = firstField(data, len, pb::FROM_RADIO_ID);
  if (configOwner != CONFIG_OWNER_NONE && isConfigField(field)) {
    if (configOwner >= 0 && sessions[configOwner].active) sendFramed(size_t(configOwner), data, len);
    if (field == pb::FROM_RADIO_CONFIG_COMPLETE) endConfigSession();
    return;
  }
  for (size_t i = 0; i < MAX_CLIENTS; i++)
    if (sessions[i].active) sendFramed(i, data, len);
}

// Handle one complete ToRadio frame from a client
static void handleToRadio(size_t idx, const uint8_t* data, size_t len) {
  uint32_t field = firstField(data, len, 0);

  // A client's "disconnect" would reset the node's session for every client
  if (field == pb::TO_RADIO_DISCONNECT) return;

  if (field == pb::TO_RADIO_WANT_CONFIG) {
    TcpSession& s = sessions[idx];
    if (len > sizeof(s.pendingMsg)) return;
    memcpy(s.pendingMsg, data, len);
    s.pendingLen = len;
    s.configPending = true;
    startNextPendingConfig();  // immediate if BLE is up and nobody else is configuring
    return;
  }
  writeToRadio(data, len);
}

// Drain FromRadio until the node returns an empty value
static void pumpBleToTcp() {
  static uint32_t lastPoll = 0;
  if (!bleReady) return;

  if (configOwner != CONFIG_OWNER_NONE && millis() - configStartedAt > CONFIG_TIMEOUT_MS) {
    logf("[TCP] config stream timed out");
    endConfigSession();
  }

  if (activeClientCount() == 0 && configOwner != CONFIG_OWNER_BRIDGE) {
    // Nobody listening: leave the node's queue intact, but probe the link
    // with a harmless FromNum read so a silently dead link is noticed
    if (millis() - lastPoll > KEEPALIVE_MS) {
      lastPoll = millis();
      fromNum->readValue();
      if (!bleClient->isConnected()) bleReady = false;
    }
    return;
  }
  if (!readPending && millis() - lastPoll < POLL_FALLBACK_MS) return;

  readPending = false;
  lastPoll = millis();
  for (int i = 0; i < 64; i++) {  // bounded so loop() stays responsive
    NimBLEAttValue value = fromRadio->readValue();
    if (value.size() == 0) break;
    dispatchFromRadio(value.data(), value.size());
    lastRxMs = millis();
  }
  if (!bleClient->isConnected()) bleReady = false;
}

// Frame parser: one byte from a client; complete frames go to the node
static void parseClientByte(size_t idx, uint8_t b) {
  FrameParser& p = sessions[idx].parser;
  switch (p.state) {
    case 0: if (b == START1) p.state = 1; break;
    case 1: p.state = (b == START2) ? 2 : 0; break;
    case 2: p.expected = size_t(b) << 8; p.state = 3; break;
    case 3:
      p.expected |= b;
      p.received = 0;
      p.state = (p.expected > 0 && p.expected <= MAX_PAYLOAD) ? 4 : 0;  // drop bad lengths
      break;
    case 4:
      p.buf[p.received++] = b;
      if (p.received == p.expected) {
        handleToRadio(idx, p.buf, p.expected);
        p.state = 0;
      }
      break;
  }
}

// Read each client in blocks (not byte by byte)
static void pumpTcpToBle() {
  uint8_t chunk[256];
  for (size_t idx = 0; idx < MAX_CLIENTS; idx++) {
    TcpSession& s = sessions[idx];
    if (!s.active) continue;
    if (!s.sock.connected()) {
      closeSession(idx, "disconnected");
      continue;
    }
    int n;
    while (s.active && s.sock.available() && (n = s.sock.read(chunk, sizeof(chunk))) > 0)
      for (int k = 0; k < n; k++) parseClientByte(idx, chunk[k]);
  }
}

static void acceptTcpClient() {
  WiFiClient incoming = tcpServer.accept();
  if (!incoming) return;
  for (auto& s : sessions) {
    if (s.active) continue;
    s.sock = incoming;
    s.sock.setNoDelay(true);
    s.ip = incoming.remoteIP().toString();
    s.parser = FrameParser();
    s.configPending = false;
    s.active = true;
    logf("[TCP] client %s connected (%u/%u)", s.ip.c_str(),
         activeClientCount(), MAX_CLIENTS);
    return;
  }
  logf("[TCP] rejected %s: max %u clients", incoming.remoteIP().toString().c_str(), MAX_CLIENTS);
  incoming.stop();
}

// ---------- WiFi ----------

static void startAp() {
  if (apActive) return;
  WiFi.mode(WIFI_AP_STA);  // STA keeps retrying in the background
  WiFi.softAP(apSsid.c_str(), AP_PASSWORD);
  dns.start(53, "*", WiFi.softAPIP());  // captive portal: every hostname -> us
  apActive = true;
  logf("[WiFi] setup AP '%s' at %s", apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

static void stopAp() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
  logf("[WiFi] setup AP stopped");
}

static void manageWifi() {
  if (apActive) dns.processNextRequest();

  if (WiFi.status() == WL_CONNECTED) {
    if (wifiDownSince) logf("[WiFi] connected, IP %s", WiFi.localIP().toString().c_str());
    wifiDownSince = 0;
    if (apActive && WiFi.softAPgetStationNum() == 0) stopAp();
    return;
  }
  if (!wifiDownSince) wifiDownSince = millis();
  if (!apActive && millis() - wifiDownSince > WIFI_AP_FALLBACK_MS) startAp();
}

// ---------- MQTT + Home Assistant discovery ----------

static WiFiClient mqttNet;
static PubSubClient mqtt(mqttNet);
static uint32_t mqttLastAttempt = 0, mqttLastPublish = 0;
static int mqttLastBleState = -1;
static bool mqttNeedsDiscovery = false;

static String mqttRoot() { return cfg.mqttBase + "/" + chipId; }

static uint32_t bleUptimeSeconds() {
  return bleState == BleState::Ready ? (millis() - bleConnectedAt) / 1000 : 0;
}

// Who an entity belongs to in HA: the bridge itself, or the node it is connected to.
// Ids come from hardware (chip id / node number), so renaming never breaks them.
struct Owner {
  String id;            // discovery node_id and unique_id prefix: mbb_<chip> / mbn_<node>
  String entityPrefix;  // default entity id: meshtastic_bridge_<chip>_ / meshtastic_<node>_
  String device;        // HA device card (JSON object)
};

static String nodeHex(uint32_t num) {
  char h[9];
  snprintf(h, sizeof(h), "%08lx", (unsigned long)num);
  return h;
}

static Owner bridgeOwner() {
  Owner o;
  o.id = "mbb_" + chipId;
  o.entityPrefix = "meshtastic_bridge_" + chipId + "_";
  o.device = "{\"identifiers\":[\"" + o.id + "\"],\"name\":\"" + jsonEscape(deviceName()) + "\"";
  o.device += ",\"manufacturer\":\"joaopedros2\",\"model\":\"" + String(PROJECT_NAME) + "\",\"sw_version\":\"" + String(FW_VERSION) + "\"";
  o.device += ",\"configuration_url\":\"http://" + WiFi.localIP().toString() + "/\"}";
  return o;
}

// The node is its own HA device, linked to the bridge (device info only needed when publishing)
static Owner nodeOwner(uint32_t num) {
  Owner o;
  String hex = nodeHex(num);
  o.id = "mbn_" + hex;
  o.entityPrefix = "meshtastic_" + hex + "_";
  // Fixed name from the node number (like the bridge's chip suffix); long/short names are entities
  String sfx = hex.substring(4, 8);
  sfx.toUpperCase();
  o.device = "{\"identifiers\":[\"" + o.id + "\"],\"name\":\"Meshtastic Node " + sfx + "\",\"manufacturer\":\"Meshtastic\"";
  if (node.hwModel >= 0) o.device += ",\"model\":\"" + String(enumName(HW_MODELS, node.hwModel)) + "\"";
  if (node.fwVersion.length()) o.device += ",\"sw_version\":\"" + jsonEscape(node.fwVersion) + "\"";
  o.device += ",\"serial_number\":\"!" + hex + "\",\"via_device\":\"mbb_" + chipId + "\"}";
  return o;
}

// Fields shared by every discovery payload: name, ids, availability, device card
static String discoveryHead(const Owner& o, const char* component, const String& objKey, const char* name,
                            bool availability = true) {
  String j = "{\"name\":";
  if (name) j += "\"" + jsonEscape(name) + "\"";
  else j += "null";  // null = entity takes the device name
  j += ",\"unique_id\":\"" + o.id + "_" + objKey + "\"";
  j += ",\"default_entity_id\":\"" + String(component) + "." + o.entityPrefix + objKey + "\"";
  if (availability) j += ",\"availability_topic\":\"" + mqttRoot() + "/status\"";
  j += ",\"device\":" + o.device;
  return j;
}

static void publishDiscovery(const char* component, const String& ownerId, const String& objKey, const String& payload) {
  String topic = cfg.mqttDiscovery + "/" + component + "/" + ownerId + "/" + objKey + "/config";
  if (!mqtt.publish(topic.c_str(), payload.c_str(), true))
    logf("[MQTT] discovery %s/%s not sent (%u bytes, max %u)", component, objKey.c_str(), payload.length() + topic.length(),
         mqtt.getBufferSize());
}

// HA-side template: seconds -> "42s", "5m 03s", "1h 23m 05s" or "2d 03h 15m 09s" (leading zero units hidden)
static String humanDuration(const char* key) {
  return "{% set t = value_json." + String(key) + " | int(0) %}"
         "{% set d = t // 86400 %}{% set h = t % 86400 // 3600 %}{% set m = t % 3600 // 60 %}{% set s = t % 60 %}"
         "{{ ('%dd %02dh %02dm %02ds' % (d, h, m, s)) if d else (('%dh %02dm %02ds' % (h, m, s)) if h"
         " else (('%dm %02ds' % (m, s)) if m else ('%ds' % s))) }}";
}

// ---------- entities (ha_entities.h) ----------

static bool isNodeEntity(const HaEntity& e) { return strcmp(e.topic, "node") == 0; }
static const char* componentName(const HaEntity& e) { return e.component == HaComponent::BinarySensor ? "binary_sensor" : "sensor"; }

// Node entities drop the "node_" prefix: they already sit on the node's own device
static String objectKey(const HaEntity& e) {
  return isNodeEntity(e) && strncmp(e.key, "node_", 5) == 0 ? String(e.key + 5) : String(e.key);
}

static uint32_t publishedNodeNum = 0;  // node whose device is announced in HA (NVS "dc_node")
static String publishedNodeSig;        // node device info last announced; empty = announce again

// One table entry -> its HA config message
static void publishEntity(const HaEntity& e, const Owner& owner) {
  // "status" reads the availability topic itself, so it shows Disconnected instead of Unavailable
  bool statusField = strcmp(e.topic, "status") == 0;
  String obj = objectKey(e);
  String j = discoveryHead(owner, componentName(e), obj, e.name, !statusField);
  j += ",\"state_topic\":\"" + mqttRoot() + "/" + e.topic + "\"";
  String tpl;
  if (statusField) tpl = "{{ 'ON' if value == 'online' else 'OFF' }}";
  else if (e.duration) tpl = humanDuration(e.key);
  else tpl = "{{ value_json." + String(e.key) + " }}";
  j += ",\"value_template\":\"" + jsonEscape(tpl) + "\"";
  if (e.diagnostic) j += ",\"entity_category\":\"diagnostic\"";
  if (e.component == HaComponent::BinarySensor) j += ",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"";
  if (*e.extra) j += String(",") + e.extra;
  j += "}";
  publishDiscovery(componentName(e), owner.id, obj, j);
}

// Publishes the entities of one device (bridge or node); returns how many
static uint16_t publishEntities(bool nodeEntities, const Owner& owner) {
  uint16_t n = 0;
  for (const HaEntity& e : HA_ENTITIES)
    if (isNodeEntity(e) == nodeEntities) {
      publishEntity(e, owner);
      n++;
    }
  return n;
}

// Entities of the bridge's own device: controls + bridge sensors
static void publishAllDiscovery() {
  String root = mqttRoot();
  Owner bridge = bridgeOwner();
  // Remove entities of older versions first, so the renamed ones keep a clean entity id (empty retained config = delete)
  for (const char* old : {"pause_ble", "resume_ble", "reconnect_ble"}) publishDiscovery("button", bridge.id, old, "");
  publishDiscovery("switch", bridge.id, "ble_link", "");
  // Controls depend on firmware commands, so they stay fixed; no entity_category = HA "Controls" block
  struct Button { const char* key; const char* name; const char* deviceClass; };
  static const Button buttons[] = {
    {"restart", "Restart", "restart"}, {"bluetooth_reconnect", "Bluetooth reconnect", nullptr}};
  for (const Button& btn : buttons) {
    String b = discoveryHead(bridge, "button", btn.key, btn.name);
    b += ",\"command_topic\":\"" + root + "/cmd\",\"payload_press\":\"" + String(btn.key) + "\"";
    if (btn.deviceClass) b += ",\"device_class\":\"" + String(btn.deviceClass) + "\"";
    else b += ",\"icon\":\"mdi:bluetooth-transfer\"";
    b += "}";
    publishDiscovery("button", bridge.id, btn.key, b);
  }
  String sw = discoveryHead(bridge, "switch", "bluetooth_link", "Bluetooth link");
  sw += ",\"state_topic\":\"" + root + "/state\",\"value_template\":\"{{ value_json.ble_enabled }}\"";
  sw += ",\"command_topic\":\"" + root + "/cmd\",\"payload_on\":\"ble_on\",\"payload_off\":\"ble_off\"";
  sw += ",\"state_on\":\"ON\",\"state_off\":\"OFF\",\"icon\":\"mdi:bluetooth-connect\"}";
  publishDiscovery("switch", bridge.id, "bluetooth_link", sw);

  logf("[MQTT] discovery published: bridge, %u entities", publishEntities(false, bridge));
  publishedNodeSig = "";  // node device follows as soon as the node is known
}

// Node device info that HA shows; a change (firmware update, other node, first position) re-announces it
static String nodeSignature() {
  return nodeHex(node.num) + "|" + node.fwVersion + "|" + String(node.hwModel) + "|" + String(node.posAt != 0);
}

// Deletes every entity of a node device (bridge now talks to another node)
static void removeNodeEntities(uint32_t num) {
  String owner = "mbn_" + nodeHex(num);
  for (const HaEntity& e : HA_ENTITIES)
    if (isNodeEntity(e)) publishDiscovery(componentName(e), owner, objectKey(e), "");
  publishDiscovery("device_tracker", owner, "position", "");
  publishDiscovery("button", owner, "restart", "");
  logf("[MQTT] discovery: removed node !%s", nodeHex(num).c_str());
}

// Entities of the node's own device: node sensors + restart button + position tracker
static void publishNodeDiscovery() {
  if (publishedNodeNum != node.num) {
    if (publishedNodeNum) {
      removeNodeEntities(publishedNodeNum);
    } else {
      // First node device on this bridge: remove the node entities v1.3.3 put on the bridge device
      String bridgeId = "mbb_" + chipId;
      for (const HaEntity& e : HA_ENTITIES)
        if (isNodeEntity(e)) publishDiscovery(componentName(e), bridgeId, e.key, "");
    }
    publishedNodeNum = node.num;
    prefs.begin("bridge", false);
    prefs.putUInt("dc_node", publishedNodeNum);
    prefs.end();
  }
  Owner owner = nodeOwner(node.num);
  uint16_t valid = publishEntities(true, owner);
  // Restart the node (Controls block of the node device)
  String rb = discoveryHead(owner, "button", "restart", "Restart");
  rb += ",\"command_topic\":\"" + mqttRoot() + "/cmd\",\"payload_press\":\"node_restart\",\"device_class\":\"restart\"}";
  publishDiscovery("button", owner.id, "restart", rb);
  valid++;
  if (node.posAt) {
    // Name null = the tracker takes the device name
    String t = discoveryHead(owner, "device_tracker", "position", nullptr);
    t += ",\"json_attributes_topic\":\"" + mqttRoot() + "/node\"";
    t += ",\"json_attributes_template\":\"" + jsonEscape(
         "{% if value_json.node_latitude is not none %}"
         "{{ {'latitude': value_json.node_latitude, 'longitude': value_json.node_longitude,"
         " 'altitude': value_json.node_altitude, 'gps_accuracy': 0} | tojson }}{% else %}{}{% endif %}") + "\"";
    t += ",\"source_type\":\"gps\",\"icon\":\"mdi:radio-tower\"}";
    publishDiscovery("device_tracker", owner.id, "position", t);
    valid++;
  }
  publishedNodeSig = nodeSignature();
  logf("[MQTT] discovery published: node !%s, %u entities", nodeHex(node.num).c_str(), valid);
}

static uint32_t mqttLastNodeData = 0;  // newest metricsAt/statsAt already published

static void publishNodeState() {
  auto num = [](bool known, uint32_t v) { return known ? String(v) : String("null"); };
  auto flt = [](float v, int dec) { return isnan(v) ? String("null") : String(v, dec); };
  bool stats = node.statsAt != 0;
  uint32_t up = node.nodeUptimeAt ? node.nodeUptime + (millis() - node.nodeUptimeAt) / 1000 : 0;
  char id[12] = "";
  if (node.num) snprintf(id, sizeof(id), "!%08lx", (unsigned long)node.num);
  String radio = !node.usePreset ? String("CUSTOM") : String(enumName(PRESETS, node.preset));
  if (node.region >= 0) radio += String(" · ") + enumName(REGIONS, node.region);

  String j = "{\"node_battery\":" + (node.battery < 0 ? String("null") : String(min(node.battery, 100)));
  j += ",\"node_powered\":\"" + String(node.battery > 100 ? "ON" : "OFF") + "\"";
  j += ",\"node_voltage\":" + flt(node.voltage, 2);
  j += ",\"node_ch_util\":" + flt(node.chUtil, 1) + ",\"node_air_util\":" + flt(node.airUtil, 2);
  j += ",\"node_uptime\":" + num(up != 0, up);
  j += ",\"node_online\":" + num(stats, node.onlineNodes) + ",\"node_total\":" + num(stats, node.totalNodes);
  j += ",\"node_pkt_rx\":" + num(stats, node.pktRx) + ",\"node_pkt_tx\":" + num(stats, node.pktTx);
  j += ",\"node_pkt_rx_bad\":" + num(stats, node.pktRxBad);
  j += ",\"node_noise_floor\":" + (stats && node.noiseFloor ? String(node.noiseFloor) : String("null"));  // 0 = radio doesn't report it
  j += ",\"node_rx_dupe\":" + num(stats, node.rxDupe) + ",\"node_tx_relay\":" + num(stats, node.txRelay);
  j += ",\"node_tx_relay_canceled\":" + num(stats, node.txRelayCanceled) + ",\"node_tx_dropped\":" + num(stats, node.txDropped);
  j += ",\"node_heap_free\":" + num(stats && node.heapFree, node.heapFree / 1024);
  j += ",\"node_heap_total\":" + num(stats && node.heapTotal, node.heapTotal / 1024);
  bool pos = node.posAt != 0;
  j += ",\"node_latitude\":" + (pos ? String(node.latI / 1e7, 7) : String("null"));
  j += ",\"node_longitude\":" + (pos ? String(node.lonI / 1e7, 7) : String("null"));
  j += ",\"node_altitude\":" + (pos ? String(node.altitude) : String("null"));
  j += ",\"node_fw\":\"" + jsonEscape(node.fwVersion) + "\",\"node_hw\":\"" + String(enumName(HW_MODELS, node.hwModel)) + "\"";
  j += ",\"node_role\":\"" + String(node.role >= 0 ? enumName(ROLES, node.role) : "") + "\"";
  j += ",\"node_radio\":\"" + radio + "\",\"node_id\":\"" + String(id) + "\"";
  j += ",\"node_long_name\":\"" + jsonEscape(node.longName) + "\",\"node_short_name\":\"" + jsonEscape(node.shortName) + "\"}";
  mqtt.publish((mqttRoot() + "/node").c_str(), j.c_str(), true);
  mqttLastNodeData = max(node.metricsAt, node.statsAt);
}

static void publishState() {
  String j = "{\"ble_connected\":\"" + String(bleState == BleState::Ready ? "ON" : "OFF") + "\"";
  j += ",\"ble_state\":\"" + String(bleStateName(bleState)) + "\"";
  j += ",\"ble_enabled\":\"" + String(cfg.bleEnabled ? "ON" : "OFF") + "\"";
  j += ",\"ble_rssi\":" + (bleState == BleState::Ready ? String(bleRssi) : String("null"));
  j += ",\"ble_uptime\":" + String(bleUptimeSeconds());
  j += ",\"ble_reconnects\":" + String(bleConnectCount > 1 ? bleConnectCount - 1 : 0);
  j += ",\"node_name\":\"" + jsonEscape(bleName) + "\"";
  j += ",\"tcp_clients\":" + String(activeClientCount());
  j += ",\"wifi_rssi\":" + String(WiFi.RSSI());
  j += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += ",\"uptime\":" + String(uptimeSec());
  j += ",\"free_heap\":" + String(ESP.getFreeHeap() / 1024);
  j += ",\"wifi_ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",\"wifi_mac\":\"" + WiFi.macAddress() + "\"";
  j += ",\"dns\":\"" + WiFi.dnsIP().toString() + "\",\"temperature\":" + String(temperatureRead(), 1) + "}";
  mqtt.publish((mqttRoot() + "/state").c_str(), j.c_str(), true);
  mqttLastPublish = millis();
  mqttLastBleState = int(bleState);
  if (node.num) publishNodeState();
}

static void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += char(payload[i]);
  if (String(topic) == cfg.mqttDiscovery + "/status") {
    if (msg == "online") mqttNeedsDiscovery = true;  // HA restarted: re-announce entities
  } else if (msg == "restart") {
    logf("[MQTT] restart requested");
    restartAt = millis() + 500;
  } else if (msg == "ble_on" || msg == "ble_off") {
    setBleEnabled(msg == "ble_on", "MQTT");
  } else if (msg == "node_restart") {
    rebootNode("MQTT");
  } else if (msg == "bluetooth_reconnect" || msg == "reconnect_ble") {  // old name kept for existing automations
    logf("[MQTT] Bluetooth reconnect requested");
    if (bleClient && bleClient->isConnected()) bleClient->disconnect();
    bleRetryDelay = 0;
  }
}

// Drop the session so new settings apply on the next attempt
static void mqttReset() {
  if (mqtt.connected()) {
    mqtt.publish((mqttRoot() + "/status").c_str(), "offline", true);
    mqtt.disconnect();
  }
  mqttLastAttempt = 0;
}

static void manageMqtt() {
  if (!cfg.mqttEnabled || cfg.mqttHost.isEmpty() || WiFi.status() != WL_CONNECTED) return;

  if (!mqtt.connected()) {
    if (mqttLastAttempt && millis() - mqttLastAttempt < 10000) return;
    mqttLastAttempt = millis();
    mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
    String root = mqttRoot();
    String will = root + "/status";
    String clientId = "mbb_" + chipId;
    bool ok = cfg.mqttUser.isEmpty()
      ? mqtt.connect(clientId.c_str(), will.c_str(), 1, true, "offline")
      : mqtt.connect(clientId.c_str(), cfg.mqttUser.c_str(), cfg.mqttPass.c_str(), will.c_str(), 1, true, "offline");
    if (!ok) {
      logf("[MQTT] connect to %s:%u failed, rc=%d", cfg.mqttHost.c_str(), cfg.mqttPort, mqtt.state());
      return;
    }
    logf("[MQTT] connected to %s:%u", cfg.mqttHost.c_str(), cfg.mqttPort);
    mqtt.publish(will.c_str(), "online", true);
    mqtt.subscribe((root + "/cmd").c_str());
    mqtt.subscribe((cfg.mqttDiscovery + "/status").c_str());
    mqttNeedsDiscovery = true;
  }

  mqtt.loop();
  if (mqttNeedsDiscovery) {
    mqttNeedsDiscovery = false;
    publishAllDiscovery();
    publishState();
  }
  // Node device: once its info has arrived, and again when it is updated / replaced
  static uint32_t lastNodeCheck = 0;
  if (node.num && node.longName.length() && millis() - lastNodeCheck > 1000) {
    lastNodeCheck = millis();
    if (nodeSignature() != publishedNodeSig) {
      publishNodeDiscovery();
      publishNodeState();
    }
  }
  // Periodic update, plus an immediate one when the BLE state changes
  if (millis() - mqttLastPublish > cfg.mqttInterval * 1000UL || mqttLastBleState != int(bleState)) publishState();
  else if (node.num && max(node.metricsAt, node.statsAt) != mqttLastNodeData) publishNodeState();
}

// ---------- web ----------

// Browsers send Origin on cross-site POSTs: refuse those, so another website open in the
// same browser can't change settings (matters most when the page has no password).
// Requests without Origin (curl, scripts) are unaffected.
static bool sameOrigin() {
  String origin = web.header("Origin");
  if (origin.isEmpty()) return true;
  int sep = origin.indexOf("://");  // http or https (e.g. behind a reverse proxy)
  return sep > 0 && origin.substring(sep + 3) == web.hostHeader();
}

static bool authorized() {
  if (web.method() == HTTP_POST && !sameOrigin()) {
    web.send(403, "text/plain", "Cross-origin request refused");
    return false;
  }
  // Empty password = open page (first-time setup); otherwise HTTP Basic auth
  if (cfg.webPass.isEmpty() || web.authenticate(cfg.webUser.c_str(), cfg.webPass.c_str())) return true;
  web.requestAuthentication();
  return false;
}

static void sendJson(const String& body, int code = 200) { web.send(code, "application/json", body); }
static void sendOk() { sendJson("{\"ok\":true}"); }
// Errors are short codes; the web page translates them (EN / PT-PT)
static void sendError(const char* msg) { sendJson(String("{\"ok\":false,\"error\":\"") + msg + "\"}", 400); }

static void handleStatus() {
  if (!authorized()) return;
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  long retryIn = bleState == BleState::Failed ? long(bleRetryDelay) - long(millis() - lastBleAttempt) : 0;
  long lastRx = lastRxMs ? long((millis() - lastRxMs) / 1000) : -1;

  String j;
  j.reserve(2800);
  j += "{\"version\":\"" + String(FW_VERSION) + "\",\"uptime\":" + String(uptimeSec());
  j += ",\"hostname\":\"" + jsonEscape(cfg.hostname) + "\",\"default_hostname\":\"" + defaultHostname() + "\"";
  j += ",\"web_user\":\"" + jsonEscape(cfg.webUser) + "\",\"web_auth\":" + String(cfg.webPass.isEmpty() ? "false" : "true");
  j += ",\"heap\":" + String(ESP.getFreeHeap()) + ",\"heap_total\":" + String(ESP.getHeapSize());
  j += ",\"wifi\":{\"connected\":" + String(wifiUp ? "true" : "false");
  j += ",\"ssid\":\"" + jsonEscape(cfg.wifiSsid) + "\"";
  j += ",\"ip\":\"" + (wifiUp ? WiFi.localIP().toString() : String("")) + "\"";
  j += ",\"rssi\":" + String(wifiUp ? WiFi.RSSI() : 0);
  j += ",\"ap\":" + String(apActive ? "true" : "false");
  j += ",\"configured\":" + String(cfg.wifiSsid.isEmpty() ? "false" : "true");
  j += ",\"ap_ssid\":\"" + apSsid + "\",\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\"}";
  j += ",\"ble\":{\"state\":\"" + String(bleStateName(bleState)) + "\"";
  j += ",\"target\":\"" + jsonEscape(cfg.bleMac) + "\",\"addr_type\":" + String(cfg.bleAddrType);
  j += ",\"name\":\"" + jsonEscape(bleName) + "\"";
  j += ",\"rssi\":" + String(bleRssi) + ",\"last_rx_s\":" + String(lastRx);
  j += ",\"connected_s\":" + String(bleState == BleState::Ready ? (millis() - bleConnectedAt) / 1000 : 0);
  j += ",\"stage\":\"" + String(lastBleStage) + "\",\"error\":" + String(lastBleError);
  j += ",\"error_key\":\"" + String(lastBleErrorKey) + "\"";
  j += ",\"enabled\":" + String(cfg.bleEnabled ? "true" : "false");
  j += ",\"retry_in_ms\":" + String(max(retryIn, 0L)) + "}";
  // Node identity/radio/battery decoded from the BLE traffic (null = not known yet)
  auto jf = [](float v, int dec) { return isnan(v) ? String("null") : String(v, dec); };
  auto ji = [](int v) { return v < 0 ? String("null") : String(v); };
  char nodeId[12] = "";
  if (node.num) snprintf(nodeId, sizeof(nodeId), "!%08lx", (unsigned long)node.num);
  uint32_t nodeUp = node.nodeUptimeAt ? node.nodeUptime + (millis() - node.nodeUptimeAt) / 1000 : 0;
  j += ",\"node\":{\"id\":\"" + String(nodeId) + "\"";
  j += ",\"long_name\":\"" + jsonEscape(node.longName) + "\",\"short_name\":\"" + jsonEscape(node.shortName) + "\"";
  j += ",\"fw\":\"" + jsonEscape(node.fwVersion) + "\",\"hw_model\":" + ji(node.hwModel) + ",\"role\":" + ji(node.role);
  j += ",\"region\":" + ji(node.region) + ",\"preset\":" + ji(node.preset) + ",\"use_preset\":" + String(node.usePreset ? "true" : "false");
  j += ",\"battery\":" + ji(node.battery) + ",\"voltage\":" + jf(node.voltage, 2);
  j += ",\"ch_util\":" + jf(node.chUtil, 1) + ",\"air_util\":" + jf(node.airUtil, 1);
  j += ",\"uptime\":" + (nodeUp ? String(nodeUp) : String("null"));
  j += ",\"metrics_age_s\":" + (node.metricsAt ? String((millis() - node.metricsAt) / 1000) : String("null"));
  j += ",\"hw_name\":\"" + String(node.hwModel >= 0 ? enumName(HW_MODELS, node.hwModel) : "") + "\"";
  j += ",\"role_name\":\"" + String(node.role >= 0 ? enumName(ROLES, node.role) : "") + "\"";
  j += ",\"powered\":" + String(node.battery > 100 ? "true" : "false");
  // LocalStats (every 15 min, null until the first one arrives)
  bool st = node.statsAt != 0;
  auto ju = [st](uint32_t v) { return st ? String(v) : String("null"); };
  j += ",\"online_nodes\":" + ju(node.onlineNodes) + ",\"total_nodes\":" + ju(node.totalNodes);
  j += ",\"pkt_rx\":" + ju(node.pktRx) + ",\"pkt_tx\":" + ju(node.pktTx) + ",\"pkt_rx_bad\":" + ju(node.pktRxBad);
  j += ",\"rx_dupe\":" + ju(node.rxDupe) + ",\"tx_relay\":" + ju(node.txRelay);
  j += ",\"tx_relay_canceled\":" + ju(node.txRelayCanceled) + ",\"tx_dropped\":" + ju(node.txDropped);
  j += ",\"heap_free\":" + (st && node.heapFree ? String(node.heapFree) : String("null"));
  j += ",\"heap_total\":" + (st && node.heapTotal ? String(node.heapTotal) : String("null"));
  bool pos = node.posAt != 0;
  j += ",\"latitude\":" + (pos ? String(node.latI / 1e7, 7) : String("null"));
  j += ",\"longitude\":" + (pos ? String(node.lonI / 1e7, 7) : String("null"));
  j += ",\"altitude\":" + (pos ? String(node.altitude) : String("null"));
  j += ",\"position_age_s\":" + (pos ? String((millis() - node.posAt) / 1000) : String("null"));
  j += ",\"noise_floor\":" + (st && node.noiseFloor ? String(node.noiseFloor) : String("null"));
  j += ",\"stats_age_s\":" + (st ? String((millis() - node.statsAt) / 1000) : String("null")) + "}";
  j += ",\"mqtt\":{\"enabled\":" + String(cfg.mqttEnabled ? "true" : "false");
  j += ",\"connected\":" + String(mqtt.connected() ? "true" : "false") + ",\"rc\":" + String(mqtt.state());
  j += ",\"host\":\"" + jsonEscape(cfg.mqttHost) + "\",\"port\":" + String(cfg.mqttPort);
  j += ",\"user\":\"" + jsonEscape(cfg.mqttUser) + "\",\"has_pass\":" + String(cfg.mqttPass.isEmpty() ? "false" : "true");
  j += ",\"discovery\":\"" + jsonEscape(cfg.mqttDiscovery) + "\",\"base\":\"" + jsonEscape(cfg.mqttBase) + "\"";
  j += ",\"topic\":\"" + jsonEscape(mqttRoot()) + "\",\"interval\":" + String(cfg.mqttInterval) + "}";
  j += ",\"tcp\":{\"max\":" + String(MAX_CLIENTS) + ",\"clients\":[";
  bool first = true;
  for (auto& s : sessions) {
    if (!s.active) continue;
    if (!first) j += ',';
    first = false;
    j += "\"" + s.ip + "\"";
  }
  j += "]}}";
  sendJson(j);
}

static void handleLogs() {
  if (!authorized()) return;
  String out;
  out.reserve(logCount * 60);
  size_t start = (logHead + LOG_LINES - logCount) % LOG_LINES;
  for (size_t i = 0; i < logCount; i++) {
    out += logBuf[(start + i) % LOG_LINES];
    out += '\n';
  }
  // Lines carry uptime; the page converts them to the viewer's clock (like ESPHome)
  web.sendHeader("X-Uptime", String(uptimeSec()));
  web.send(200, "text/plain; charset=utf-8", out);
}

static void handleBleScanStart() {
  if (!authorized()) return;
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan->isScanning()) {
    scan->clearResults();
    scan->setActiveScan(true);
    scan->start(5000, false, true);  // non-blocking; results polled below
    logf("[SCAN] BLE scan started");
  }
  sendOk();
}

static void handleBleScanResults() {
  if (!authorized()) return;
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan->isScanning()) return sendJson("{\"scanning\":true}");

  NimBLEScanResults results = scan->getResults();
  String j = "{\"scanning\":false,\"devices\":[";
  bool first = true;
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* dev = results.getDevice(i);
    if (!dev->isAdvertisingService(SERVICE_UUID)) continue;  // Meshtastic nodes only
    if (!first) j += ',';
    first = false;
    j += "{\"name\":\"" + jsonEscape(dev->getName().c_str()) + "\"";
    j += ",\"addr\":\"" + String(dev->getAddress().toString().c_str()) + "\"";
    j += ",\"type\":" + String(dev->getAddress().getType());
    j += ",\"rssi\":" + String(dev->getRSSI()) + "}";
  }
  j += "]}";
  sendJson(j);
}

// Node restart from the status page (confirmed on the page first)
static void handleNodeReboot() {
  if (!authorized()) return;
  if (!rebootNode("web")) return sendError("not_connected");
  sendOk();
}

static void handleBleEnable() {
  if (!authorized()) return;
  setBleEnabled(web.arg("enabled") == "1", "web");
  sendOk();
}

// Meshtastic fixed PIN: exactly 6 digits
static bool validPin(const String& p) {
  if (p.length() != 6) return false;
  for (size_t i = 0; i < p.length(); i++)
    if (!isdigit(p[i])) return false;
  return true;
}

static void handleBleConfig() {
  if (!authorized()) return;
  String mac = web.arg("mac");
  mac.trim();
  mac.toLowerCase();
  if (!mac.isEmpty() && mac.length() != 17) return sendError("invalid_mac");

  cfg.bleMac = mac;
  cfg.bleAddrType = web.hasArg("type") ? uint8_t(web.arg("type").toInt()) : BLE_ADDR_RANDOM;
  String pin = web.arg("pin");
  if (pin.length()) {  // empty = keep current
    if (!validPin(pin)) return sendError("invalid_pin");
    cfg.blePin = pin.toInt();
  }
  saveSettings();
  logf("[CFG] BLE target %s (type %u)", cfg.bleMac.c_str(), cfg.bleAddrType);
  if (!cfg.bleEnabled) {
    cfg.bleEnabled = true;  // "Save and connect" turns the link switch back on
    saveSettings();
  }
  applyBleTarget();
  sendOk();
}

static void handleWifiScanStart() {
  if (!authorized()) return;
  if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);  // async
  }
  sendOk();
}

static void handleWifiScanResults() {
  if (!authorized()) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return sendJson("{\"scanning\":true}");

  String j = "{\"scanning\":false,\"networks\":[";
  for (int i = 0; i < max(n, 0); i++) {
    if (WiFi.SSID(i).isEmpty()) continue;  // hidden networks
    if (j[j.length() - 1] != '[') j += ',';
    j += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i));
    j += ",\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  j += "]}";
  sendJson(j);
}

// DHCP/mDNS hostname: lowercase letters, digits and '-', max 32 chars
static bool validHostname(const String& h) {
  if (h.isEmpty() || h.length() > 32 || h[0] == '-' || h[h.length() - 1] == '-') return false;
  for (size_t i = 0; i < h.length(); i++) {
    char c = h[i];
    if (!(isdigit(c) || (c >= 'a' && c <= 'z') || c == '-')) return false;
  }
  return true;
}

// Wi-Fi network and the bridge's network name (DHCP/mDNS); both apply on a fresh join, so restart
static void handleWifiConfig() {
  if (!authorized()) return;
  String ssid = web.arg("ssid");
  if (ssid.isEmpty()) return sendError("empty_ssid");
  String host = web.arg("hostname");
  host.trim();
  host.toLowerCase();
  if (host.isEmpty()) host = defaultHostname();  // empty field = back to the default name
  if (!validHostname(host)) return sendError("invalid_hostname");
  // Empty password keeps the current one on the same network (the field is never prefilled)
  if (web.arg("pass").length() || ssid != cfg.wifiSsid) cfg.wifiPass = web.arg("pass");
  cfg.wifiSsid = ssid;
  cfg.hostname = host;
  saveSettings();
  logf("[CFG] WiFi '%s', name '%s', restarting", cfg.wifiSsid.c_str(), cfg.hostname.c_str());
  sendOk();
  restartAt = millis() + 1500;  // let the response go out first
}

static void handleWebAuth() {
  if (!authorized()) return;
  String user = web.arg("user");
  user.trim();
  if (user.isEmpty()) user = "admin";
  if (user.indexOf(':') >= 0) return sendError("invalid_user");  // Basic auth separator
  cfg.webUser = user;
  cfg.webPass = web.arg("pass");
  saveSettings();
  logf("[CFG] web login: user '%s', password %s", cfg.webUser.c_str(), cfg.webPass.isEmpty() ? "none" : "set");
  sendOk();
}

// Wipe all settings and pairings -> next boot is a first-time setup
static void handleFactoryReset() {
  if (!authorized()) return;
  prefs.begin("bridge", false);
  prefs.clear();
  prefs.end();
  NimBLEDevice::deleteAllBonds();
  logf("[CFG] factory reset, restarting");
  sendOk();
  restartAt = millis() + 1500;
}

// MQTT topic prefix: no wildcards, spaces or leading/trailing '/'
static bool validTopic(const String& t) {
  if (t.isEmpty() || t.length() > 64 || t[0] == '/' || t[t.length() - 1] == '/') return false;
  for (size_t i = 0; i < t.length(); i++)
    if (t[i] == '#' || t[i] == '+' || t[i] == ' ' || uint8_t(t[i]) < 0x20) return false;
  return true;
}

static void handleMqttConfig() {
  if (!authorized()) return;
  String host = web.arg("host");
  host.trim();
  bool enabled = web.arg("enabled") == "1";
  if (enabled && host.isEmpty()) return sendError("empty_host");
  long port = web.hasArg("port") ? web.arg("port").toInt() : 1883;
  long interval = web.hasArg("interval") ? web.arg("interval").toInt() : 30;
  if (port < 1 || port > 65535) return sendError("invalid_port");
  if (interval < 5 || interval > 3600) return sendError("invalid_interval");
  String disc = web.arg("discovery"), base = web.arg("base");
  disc.trim();
  base.trim();
  if (disc.isEmpty()) disc = "homeassistant";
  if (base.isEmpty()) base = PROJECT_SLUG;
  if (!validTopic(disc) || !validTopic(base)) return sendError("invalid_topic");

  cfg.mqttEnabled = enabled;
  cfg.mqttHost = host;
  cfg.mqttPort = uint16_t(port);
  cfg.mqttUser = web.arg("user");
  if (web.arg("pass").length()) cfg.mqttPass = web.arg("pass");  // empty = keep current
  if (web.arg("clear_pass") == "1") cfg.mqttPass = "";
  cfg.mqttDiscovery = disc;
  cfg.mqttBase = base;
  cfg.mqttInterval = uint16_t(interval);
  saveSettings();
  logf("[CFG] MQTT %s (%s:%u)", enabled ? "enabled" : "disabled", host.c_str(), cfg.mqttPort);
  mqttReset();
  sendOk();
}

static void handleReboot() {
  if (!authorized()) return;
  sendOk();
  restartAt = millis() + 1000;
}

static void handleOtaDone() {
  if (otaRejected) return web.requestAuthentication();
  if (Update.hasError()) return sendError(Update.errorString());
  sendOk();
  logf("[OTA] success, restarting");
  restartAt = millis() + 1000;
}

static void handleOtaChunk() {
  HTTPUpload& up = web.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaRejected = !sameOrigin() || (!cfg.webPass.isEmpty() && !web.authenticate(cfg.webUser.c_str(), cfg.webPass.c_str()));
    if (otaRejected) return;
    logf("[OTA] receiving %s", up.filename.c_str());
    if (bleClient && bleClient->isConnected()) bleClient->disconnect();  // free radio time
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) logf("[OTA] begin failed: %s", Update.errorString());
  } else if (otaRejected) {
    return;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) logf("[OTA] write failed: %s", Update.errorString());
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) logf("[OTA] %u bytes written", up.totalSize);
    else logf("[OTA] end failed: %s", Update.errorString());
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    logf("[OTA] aborted");
  }
}

static void setupWeb() {
  web.on("/", HTTP_GET, []() {
    if (authorized()) web.send_P(200, "text/html; charset=utf-8", WEB_PAGE);
  });
  web.on("/api/status", HTTP_GET, handleStatus);
  web.on("/api/logs", HTTP_GET, handleLogs);
  web.on("/api/ble/scan", HTTP_POST, handleBleScanStart);
  web.on("/api/ble/scan", HTTP_GET, handleBleScanResults);
  web.on("/api/ble/config", HTTP_POST, handleBleConfig);
  web.on("/api/ble/enable", HTTP_POST, handleBleEnable);
  web.on("/api/node/reboot", HTTP_POST, handleNodeReboot);
  web.on("/api/wifi/scan", HTTP_POST, handleWifiScanStart);
  web.on("/api/wifi/scan", HTTP_GET, handleWifiScanResults);
  web.on("/api/wifi/config", HTTP_POST, handleWifiConfig);
  web.on("/api/web/auth", HTTP_POST, handleWebAuth);
  web.on("/api/reboot", HTTP_POST, handleReboot);
  web.on("/api/factory-reset", HTTP_POST, handleFactoryReset);
  web.on("/api/mqtt/config", HTTP_POST, handleMqttConfig);
  web.on("/api/update", HTTP_POST, handleOtaDone, handleOtaChunk);
  web.onNotFound([]() {
    // Captive portal: send phones/laptops on the setup AP to the config page
    if (apActive) {
      web.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
      web.send(302, "text/plain", "");
    } else {
      web.send(404, "text/plain", "Not found");
    }
  });
  static const char* headers[] = {"Origin"};  // read by sameOrigin()
  web.collectHeaders(headers, 1);
  web.begin();
}

// ---------- setup / loop ----------

void setup() {
  Serial.begin(115200);
  loadSettings();
  char suffix[5];
  uint64_t chipMac = ESP.getEfuseMac();  // byte 0 of the MAC is the lowest byte
  char id[7];
  snprintf(id, sizeof(id), "%02x%02x%02x", uint8_t(chipMac >> 24), uint8_t(chipMac >> 32), uint8_t(chipMac >> 40));
  chipId = id;
  snprintf(suffix, sizeof(suffix), "%02X%02X", uint8_t(chipMac >> 32), uint8_t(chipMac >> 40));
  chipSuffix = suffix;
  // Old single default name -> per-chip default (a name chosen by the user is kept)
  if (cfg.hostname.isEmpty() || cfg.hostname == PROJECT_SLUG) cfg.hostname = defaultHostname();
  apSsid = String(PROJECT_SLUG) + "-" + suffix;  // e.g. meshtastic-bridge-ble-tcp-3C91 (30 chars, SSID max 32)
  logf("[BOOT] %s v%s", PROJECT_NAME, FW_VERSION);

  WiFi.setHostname(cfg.hostname.c_str());  // must precede mode()/begin()
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);  // required for WiFi + BLE coexistence
  WiFi.setAutoReconnect(true);

  if (cfg.wifiSsid.isEmpty()) {
    startAp();
  } else {
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);
    if (WiFi.status() == WL_CONNECTED) logf("[WiFi] %s", WiFi.localIP().toString().c_str());
    else startAp();
  }

  // Lets the apps (and browsers via .local) discover the bridge
  if (MDNS.begin(cfg.hostname.c_str())) {
    MDNS.setInstanceName(deviceName().c_str());
    MDNS.addService("meshtastic", "tcp", TCP_PORT);
    MDNS.addServiceTxt("meshtastic", "tcp", "type", PROJECT_SLUG);
    MDNS.addServiceTxt("meshtastic", "tcp", "node", cfg.bleMac.c_str());
    MDNS.addService("http", "tcp", 80);
  }
  prefs.begin("bridge", false);
  publishedNodeNum = prefs.getUInt("dc_node", 0);
  prefs.remove("dc_name");  // left by the discovery file upload (v1.3.4)
  prefs.remove("dc_ts");
  prefs.end();

  tcpServer.begin();
  setupWeb();

  mqtt.setBufferSize(1536);  // discovery and node payloads exceed the 256-byte default
  mqtt.setCallback(onMqttMessage);

  NimBLEDevice::init("");
  NimBLEDevice::setMTU(517);
  NimBLEDevice::setSecurityAuth(true, true, true);  // bonding, MITM, secure connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
  if (!cfg.bleEnabled) bleState = BleState::Disabled;
  else if (cfg.bleMac.isEmpty()) bleState = BleState::NotConfigured;
}

void loop() {
  if (restartAt && int32_t(millis() - restartAt) >= 0) ESP.restart();  // wrap-safe

  manageWifi();
  web.handleClient();
  manageBle();
  manageMqtt();

  acceptTcpClient();
  pumpTcpToBle();
  pumpBleToTcp();
  delay(2);
}
