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
#include <stdarg.h>
#include "web_page.h"

// No WiFi network or BLE node is built in: on first boot the bridge opens a
// setup access point and everything is configured on the web page (stored in NVS).
static constexpr uint16_t TCP_PORT = 4403;          // Meshtastic stream API port
static constexpr uint32_t DEFAULT_BLE_PIN = 123456;  // Meshtastic default fixed PIN
// Project name in one place: page title, logs, mDNS, HA device model
static const char* PROJECT_NAME = "Meshtastic Bridge BLE-TCP";
static const char* PROJECT_SLUG = "meshtastic-bridge-ble-tcp";  // hostname, AP, MQTT base topic
static const char* LEGACY_SLUG = "meshtastic-ble-bridge";       // pre-1.3.2 default, migrated on boot
static const char* AP_PASSWORD = "meshtastic";            // min 8 chars (WPA2)

static const char* FW_VERSION = "1.3.3";
// Shown in the router/DHCP list and used for mDNS (<name>.local); editable on the web page

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

// ---------- settings (NVS) ----------

struct Settings {
  String wifiSsid, wifiPass, bleMac, webUser, webPass, hostname;
  uint8_t bleAddrType;
  uint32_t blePin;
  // MQTT (Home Assistant discovery)
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

  uint32_t s = millis() / 1000;
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

enum class BleState { NotConfigured, Idle, Connecting, Ready, Failed };
static const char* bleStateName(BleState s) {
  switch (s) {
    case BleState::NotConfigured: return "not_configured";
    case BleState::Connecting: return "connecting";
    case BleState::Ready: return "ready";
    case BleState::Failed: return "failed";
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
static int bleRssi = 0;
static String bleName;  // the node's advertised Bluetooth name, e.g. "JSS8_8903"
static uint32_t lastRxMs = 0;
static uint32_t bleConnectedAt = 0;  // millis() when the current BLE link became ready
static uint32_t bleConnectCount = 0;  // successful connections since boot (reconnects = count - 1)
static uint8_t writeFailures = 0;
static volatile int disconnectReason = -1;  // set by BLE task, -1 = nothing pending

static bool apActive = false;
static String apSsid;  // unique per chip, so several bridges don't collide
static String chipId;  // last 3 bytes of the chip MAC, e.g. "87ae18"
static uint32_t wifiDownSince = 0;
static uint32_t restartAt = 0;
static bool otaRejected = false;

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
    logf("[BLE] auth %s", connInfo.isEncrypted() ? "ok" : "FAILED");
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

static bool bleFail(const char* stage) {
  lastBleError = bleClient ? bleClient->getLastError() : -1;
  lastBleStage = stage;
  bleState = BleState::Failed;
  bleRetryDelay = bleRetryDelay == 0 ? BLE_RETRY_MIN_MS : min(bleRetryDelay * 2, BLE_RETRY_MAX_MS);
  logf("[BLE] %s failed, err=%d, retry in %lus", stage, lastBleError, bleRetryDelay / 1000);
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
  if (!bleClient->secureConnection()) return bleFail("pairing (PIN?)");

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
  bleConnectedAt = millis();
  bleConnectCount++;
  bleRetryDelay = 0;
  writeFailures = 0;
  bleReady = true;
  startNextPendingConfig();  // serve clients that connected while BLE was down
  return true;
}

// Drop the current link and pairing, then reconnect with the new settings
static void applyBleTarget() {
  if (bleClient && bleClient->isConnected()) bleClient->disconnect();
  bleReady = false;
  endConfigSession();
  closeAllClients("BLE target changed");
  bleName = "";
  NimBLEDevice::deleteAllBonds();
  bleState = cfg.bleMac.isEmpty() ? BleState::NotConfigured : BleState::Idle;
  bleRetryDelay = 0;
  lastBleAttempt = millis();
}

static void manageBle() {
  static uint32_t lastCheck = 0;
  NimBLEScan* scan = NimBLEDevice::getScan();

  if (disconnectReason >= 0) {
    logf("[BLE] disconnected, reason=%d (%s)", disconnectReason, disconnectText(disconnectReason));
    disconnectReason = -1;
  }

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
static int configOwner = -1;  // session receiving the config stream, -1 = none
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

static size_t activeClientCount() {
  size_t n = 0;
  for (auto& s : sessions) n += s.active;
  return n;
}

static void sendFramed(WiFiClient& sock, const uint8_t* data, size_t len) {
  uint8_t header[4] = {START1, START2, uint8_t(len >> 8), uint8_t(len & 0xFF)};
  sock.write(header, sizeof(header));
  sock.write(data, len);
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

static void startNextPendingConfig() {
  if (!bleReady || configOwner >= 0) return;
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
  configOwner = -1;
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
  uint32_t field = firstField(data, len, pb::FROM_RADIO_ID);
  if (configOwner >= 0 && isConfigField(field)) {
    TcpSession& owner = sessions[configOwner];
    if (owner.active) sendFramed(owner.sock, data, len);
    if (field == pb::FROM_RADIO_CONFIG_COMPLETE) endConfigSession();
    return;
  }
  for (auto& s : sessions)
    if (s.active) sendFramed(s.sock, data, len);
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

  if (configOwner >= 0 && millis() - configStartedAt > CONFIG_TIMEOUT_MS) {
    logf("[TCP] config stream timed out");
    endConfigSession();
  }

  if (activeClientCount() == 0) {
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

// Byte-wise frame parser for data coming from each client
static void pumpTcpToBle() {
  for (size_t idx = 0; idx < MAX_CLIENTS; idx++) {
    TcpSession& s = sessions[idx];
    if (!s.active) continue;
    if (!s.sock.connected()) {
      closeSession(idx, "disconnected");
      continue;
    }
    FrameParser& p = s.parser;
    while (s.active && s.sock.available()) {
      uint8_t b = s.sock.read();
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

// Fields shared by every discovery payload: unique id, state/availability topics, device card
static String discoveryBase(const char* key, const char* name) {
  String root = mqttRoot();
  String j = "{\"name\":\"" + String(name) + "\",\"unique_id\":\"mbb_" + chipId + "_" + key + "\"";
  j += ",\"availability_topic\":\"" + root + "/status\"";
  // Device shows the project name unless the user gave the bridge its own hostname
  String devName = cfg.hostname == PROJECT_SLUG ? String(PROJECT_NAME) : cfg.hostname;
  j += ",\"device\":{\"identifiers\":[\"mbb_" + chipId + "\"],\"name\":\"" + jsonEscape(devName) + "\"";
  j += ",\"manufacturer\":\"joaopedros2\",\"model\":\"" + String(PROJECT_NAME) + "\",\"sw_version\":\"" + String(FW_VERSION) + "\"";
  j += ",\"configuration_url\":\"http://" + WiFi.localIP().toString() + "/\"}";
  return j;
}

static void publishDiscovery(const char* component, const char* key, const String& payload) {
  String topic = cfg.mqttDiscovery + "/" + component + "/mbb_" + chipId + "/" + key + "/config";
  mqtt.publish(topic.c_str(), payload.c_str(), true);
}

// Sensor reading one field of the JSON state message
static void discoverSensor(const char* key, const char* name, const char* extra, bool diagnostic,
                           const String& tpl = String()) {
  String j = discoveryBase(key, name);
  j += ",\"state_topic\":\"" + mqttRoot() + "/state\",\"value_template\":\"";
  j += tpl.length() ? tpl : "{{ value_json." + String(key) + " }}";
  j += "\"";
  if (diagnostic) j += ",\"entity_category\":\"diagnostic\"";
  j += extra;
  j += "}";
  publishDiscovery("sensor", key, j);
}

// HA-side template: seconds -> "14m", "1h 23m" or "2d 03h 15m" (leading zero units hidden)
static String humanDuration(const char* key) {
  return "{% set s = value_json." + String(key) + " | int(0) %}"
         "{% set d = s // 86400 %}{% set h = s % 86400 // 3600 %}{% set m = s % 3600 // 60 %}"
         "{{ ('%dd %02dh %02dm' % (d, h, m)) if d else (('%dh %02dm' % (h, m)) if h else ('%dm' % m)) }}";
}

static void publishAllDiscovery() {
  String root = mqttRoot();
  String j = discoveryBase("ble_connected", "Bluetooth connected");
  j += ",\"state_topic\":\"" + root + "/state\",\"value_template\":\"{{ value_json.ble_connected }}\"";
  j += ",\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"device_class\":\"connectivity\"}";
  publishDiscovery("binary_sensor", "ble_connected", j);

  discoverSensor("ble_state", "Bluetooth state",
                 ",\"device_class\":\"enum\",\"options\":[\"ready\",\"connecting\",\"failed\",\"idle\",\"not_configured\"]", false);
  discoverSensor("ble_rssi", "Bluetooth RSSI",
                 ",\"device_class\":\"signal_strength\",\"unit_of_measurement\":\"dBm\",\"state_class\":\"measurement\"", false);
  discoverSensor("ble_uptime", "Bluetooth connected for", ",\"icon\":\"mdi:timer-outline\"", false, humanDuration("ble_uptime"));
  discoverSensor("ble_reconnects", "Bluetooth reconnects", ",\"state_class\":\"total_increasing\",\"icon\":\"mdi:bluetooth-off\"", false);
  discoverSensor("node_name", "Node name", ",\"icon\":\"mdi:radio-tower\"", true);
  discoverSensor("tcp_clients", "TCP clients", ",\"state_class\":\"measurement\",\"icon\":\"mdi:lan-connect\"", false);
  discoverSensor("wifi_rssi", "WiFi RSSI",
                 ",\"device_class\":\"signal_strength\",\"unit_of_measurement\":\"dBm\",\"state_class\":\"measurement\"", true);
  discoverSensor("ip", "IP address", ",\"icon\":\"mdi:ip-network\"", true);
  discoverSensor("uptime", "Uptime", ",\"icon\":\"mdi:clock-outline\"", true, humanDuration("uptime"));
  discoverSensor("free_heap", "Free memory",
                 ",\"device_class\":\"data_size\",\"unit_of_measurement\":\"kB\",\"state_class\":\"measurement\"", true);

  for (const char* key : {"restart", "reconnect_ble"}) {
    String b = discoveryBase(key, strcmp(key, "restart") == 0 ? "Restart" : "Reconnect Bluetooth");
    b += ",\"command_topic\":\"" + root + "/cmd\",\"payload_press\":\"" + String(key) + "\"";
    if (strcmp(key, "restart") == 0) b += ",\"device_class\":\"restart\"";
    b += ",\"entity_category\":\"config\"}";
    publishDiscovery("button", key, b);
  }
  logf("[MQTT] discovery published (%s/.../mbb_%s)", cfg.mqttDiscovery.c_str(), chipId.c_str());
}

static void publishState() {
  String j = "{\"ble_connected\":\"" + String(bleState == BleState::Ready ? "ON" : "OFF") + "\"";
  j += ",\"ble_state\":\"" + String(bleStateName(bleState)) + "\"";
  j += ",\"ble_rssi\":" + (bleState == BleState::Ready ? String(bleRssi) : String("null"));
  j += ",\"ble_uptime\":" + String(bleUptimeSeconds());
  j += ",\"ble_reconnects\":" + String(bleConnectCount > 1 ? bleConnectCount - 1 : 0);
  j += ",\"node_name\":\"" + jsonEscape(bleName) + "\"";
  j += ",\"tcp_clients\":" + String(activeClientCount());
  j += ",\"wifi_rssi\":" + String(WiFi.RSSI());
  j += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += ",\"uptime\":" + String(millis() / 1000);
  j += ",\"free_heap\":" + String(ESP.getFreeHeap() / 1024) + "}";
  mqtt.publish((mqttRoot() + "/state").c_str(), j.c_str(), true);
  mqttLastPublish = millis();
  mqttLastBleState = int(bleState);
}

static void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += char(payload[i]);
  if (String(topic) == cfg.mqttDiscovery + "/status") {
    if (msg == "online") mqttNeedsDiscovery = true;  // HA restarted: re-announce entities
  } else if (msg == "restart") {
    logf("[MQTT] restart requested");
    restartAt = millis() + 500;
  } else if (msg == "reconnect_ble") {
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
  // Periodic update, plus an immediate one when the BLE state changes
  if (millis() - mqttLastPublish > cfg.mqttInterval * 1000UL || mqttLastBleState != int(bleState)) publishState();
}

// ---------- web ----------

static bool authorized() {
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
  j.reserve(1400);
  j += "{\"version\":\"" + String(FW_VERSION) + "\",\"uptime\":" + String(millis() / 1000);
  j += ",\"hostname\":\"" + jsonEscape(cfg.hostname) + "\"";
  j += ",\"web_user\":\"" + jsonEscape(cfg.webUser) + "\",\"web_auth\":" + String(cfg.webPass.isEmpty() ? "false" : "true");
  j += ",\"heap\":" + String(ESP.getFreeHeap());
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
  j += ",\"retry_in_ms\":" + String(max(retryIn, 0L)) + "}";
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
  web.sendHeader("X-Uptime", String(millis() / 1000));
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

static void handleBleConfig() {
  if (!authorized()) return;
  String mac = web.arg("mac");
  mac.trim();
  mac.toLowerCase();
  if (!mac.isEmpty() && mac.length() != 17) return sendError("invalid_mac");

  cfg.bleMac = mac;
  cfg.bleAddrType = web.hasArg("type") ? uint8_t(web.arg("type").toInt()) : BLE_ADDR_RANDOM;
  if (web.arg("pin").length()) cfg.blePin = web.arg("pin").toInt();  // empty = keep current
  saveSettings();
  logf("[CFG] BLE target %s (type %u)", cfg.bleMac.c_str(), cfg.bleAddrType);
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

static void handleWifiConfig() {
  if (!authorized()) return;
  if (web.arg("ssid").isEmpty()) return sendError("empty_ssid");
  cfg.wifiSsid = web.arg("ssid");
  cfg.wifiPass = web.arg("pass");
  saveSettings();
  logf("[CFG] WiFi set to '%s', restarting", cfg.wifiSsid.c_str());
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

// DHCP/mDNS hostname: lowercase letters, digits and '-', max 32 chars
static bool validHostname(const String& h) {
  if (h.isEmpty() || h.length() > 32 || h[0] == '-' || h[h.length() - 1] == '-') return false;
  for (size_t i = 0; i < h.length(); i++) {
    char c = h[i];
    if (!(isdigit(c) || (c >= 'a' && c <= 'z') || c == '-')) return false;
  }
  return true;
}

static void handleDeviceConfig() {
  if (!authorized()) return;
  String host = web.arg("hostname");
  host.trim();
  host.toLowerCase();
  if (!validHostname(host)) return sendError("invalid_hostname");
  cfg.hostname = host;
  saveSettings();
  logf("[CFG] hostname '%s', restarting", cfg.hostname.c_str());
  sendOk();
  restartAt = millis() + 1500;  // hostname only applies on a fresh WiFi join
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

  cfg.mqttEnabled = enabled;
  cfg.mqttHost = host;
  cfg.mqttPort = uint16_t(port);
  cfg.mqttUser = web.arg("user");
  if (web.arg("pass").length()) cfg.mqttPass = web.arg("pass");  // empty = keep current
  if (web.arg("clear_pass") == "1") cfg.mqttPass = "";
  cfg.mqttDiscovery = web.arg("discovery").isEmpty() ? String("homeassistant") : web.arg("discovery");
  cfg.mqttBase = web.arg("base").isEmpty() ? String(PROJECT_SLUG) : web.arg("base");
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
    otaRejected = !cfg.webPass.isEmpty() && !web.authenticate(cfg.webUser.c_str(), cfg.webPass.c_str());
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
  web.on("/api/wifi/scan", HTTP_POST, handleWifiScanStart);
  web.on("/api/wifi/scan", HTTP_GET, handleWifiScanResults);
  web.on("/api/wifi/config", HTTP_POST, handleWifiConfig);
  web.on("/api/web/auth", HTTP_POST, handleWebAuth);
  web.on("/api/reboot", HTTP_POST, handleReboot);
  web.on("/api/factory-reset", HTTP_POST, handleFactoryReset);
  web.on("/api/device/config", HTTP_POST, handleDeviceConfig);
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
  apSsid = String(PROJECT_SLUG) + "-" + suffix;  // e.g. meshtastic-bridge-ble-tcp-AE18 (30 chars, SSID max 32)
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
    MDNS.setInstanceName(PROJECT_NAME);
    MDNS.addService("meshtastic", "tcp", TCP_PORT);
    MDNS.addServiceTxt("meshtastic", "tcp", "type", PROJECT_SLUG);
    MDNS.addServiceTxt("meshtastic", "tcp", "node", cfg.bleMac.c_str());
    MDNS.addService("http", "tcp", 80);
  }
  tcpServer.begin();
  setupWeb();

  mqtt.setBufferSize(1024);  // discovery payloads exceed the 256-byte default
  mqtt.setCallback(onMqttMessage);

  NimBLEDevice::init("");
  NimBLEDevice::setMTU(517);
  NimBLEDevice::setSecurityAuth(true, true, true);  // bonding, MITM, secure connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
  if (cfg.bleMac.isEmpty()) bleState = BleState::NotConfigured;
}

void loop() {
  if (restartAt && millis() > restartAt) ESP.restart();

  manageWifi();
  web.handleClient();
  manageBle();
  manageMqtt();

  acceptTcpClient();
  pumpTcpToBle();
  pumpBleToTcp();
  delay(2);
}
