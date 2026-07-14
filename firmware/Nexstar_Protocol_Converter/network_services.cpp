#include "network_services.h"

#include "bluetooth_services.h"
#include "logging.h"
#include "lx200_protocol.h"
#include "settings.h"

#include <string.h>

#if defined(ESP8266)
ESP8266WebServer *webServer = nullptr;
ESP8266WebServer *alpacaServer = nullptr;
ESP8266WebServer *activeServer = nullptr;
#else
WebServer *webServer = nullptr;
WebServer *alpacaServer = nullptr;
WebServer *activeServer = nullptr;
#endif

WiFiUDP discoveryUdp;
WiFiServer *lx200ServerPtr = nullptr;
WiFiClient lx200Client;
bool lx200WifiClientConnected = false;

WiFiServer *stellariumServerPtr = nullptr;
WiFiClient stellariumClient;
bool stellariumClientConnected = false;
unsigned long stellariumLastRxMs = 0;
unsigned long stellariumLastTxMs = 0;
uint32_t stellariumRxPackets = 0;
uint32_t stellariumTxPackets = 0;
uint32_t stellariumBadPackets = 0;

WiFiServer *telnetServerPtr = nullptr;
WiFiServer tinySetupServer(80);
WiFiClient telnetClient;
bool telnetClientConnected = false;
bool telnetAuthenticated = false;
String telnetLine = "";
bool telnetLastWasCR = false;
unsigned long telnetLastRxMs = 0;
uint32_t telnetRxCommands = 0;
uint32_t telnetAuthFailures = 0;
bool telnetLiveLogEnabled = true;
uint32_t telnetLiveLogLines = 0;
String telnetPendingConfirmCommand = "";
String telnetPendingConfirmDescription = "";
bool telnetMonitorActive = false;
bool telnetMonitorSavedLiveLog = true;
unsigned long telnetMonitorLastDrawMs = 0;
unsigned long telnetMonitorRefreshMs = 2000;
bool telnetTasksActive = false;
bool telnetTasksSavedLiveLog = true;
unsigned long telnetTasksLastDrawMs = 0;
unsigned long telnetTasksRefreshMs = 5000;
unsigned long telnetTasksLastIdle0 = 0;
unsigned long telnetTasksLastIdle1 = 0;
unsigned long telnetTasksLastRuntimeTotal = 0;
unsigned long telnetTasksLastLoadMs = 0;
int telnetTasksCpuLoadPct = -1;

bool restartPending = false;
unsigned long restartAtMs = 0;
bool apRestartPending = false;
unsigned long apRestartAtMs = 0;
bool staConnected = false;
bool apRunning = false;
bool wifiRuntimeEnabled = true;
String wifiModeText = "AP";
String lastWifiStatus = "AP fallback";
String startupModeSource = "saved settings";
int startupModePinUsed = -1;

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

#define GPIO_STARTUP_MODE_ENABLED 1
#define GPIO_STARTUP_ACTIVE_LOW 1
#define GPIO_STARTUP_FULL_WIFI_PIN 32
#define GPIO_STARTUP_BT_WEB_PIN 33
#define GPIO_STARTUP_BT_TELNET_PIN 25
#define GPIO_STARTUP_SAMPLE_DELAY_MS 75

extern const char* FW_NAME;
extern const char* FW_VERSION;
const uint16_t HTTP_WEB_PORT = 80;
const uint16_t ALPACA_DISCOVERY_PORT = 32227;
extern String runtimeApSsid();
extern bool savePersistentSettings();
extern bool syncTimeFromNTP(bool forceLog);
extern void serviceNtpSync();
extern void handleStellariumPacket(uint8_t *pkt, uint16_t len);
extern void sendStellariumPosition();
extern void markPositionDemand(const char* reason);
extern void telnetDrawMonitor(Print &out);
extern void telnetStopMonitor(Print &out);
extern void telnetDrawTasks(Print &out);
extern void telnetStopTasks(Print &out);
extern void telnetExecuteCommand(String line, Print &out);

static WiFiServer& lx200Server() {
  return *lx200ServerPtr;
}

static WiFiServer& stellariumServer() {
  return *stellariumServerPtr;
}

static WiFiServer& telnetServer() {
  return *telnetServerPtr;
}

class TelnetCRLFPrint : public Print {
public:
  explicit TelnetCRLFPrint(Print &wrapped) : out(wrapped), lastWasCR(false) {}

  size_t write(uint8_t b) override {
    size_t n = 0;
    if (b == '\n' && !lastWasCR) {
      n += out.write('\r');
    }
    n += out.write(b);
    lastWasCR = (b == '\r');
    if (b != '\r' && b != '\n') lastWasCR = false;
    return n;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    size_t n = 0;
    for (size_t i = 0; i < size; i++) n += write(buffer[i]);
    return n;
  }

private:
  Print &out;
  bool lastWasCR;
};

static uint16_t readLE16Network(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

void serviceHttpServers() {
  if (bridgeMode != BRIDGE_MODE_WIFI_FULL) return;

  if (webServer) {
    activeServer = webServer;
    webServer->handleClient();
  }
  if (alpacaServer) {
    activeServer = alpacaServer;
    alpacaServer->handleClient();
  }
}

void serviceNetworkClients() {
  if (bridgeMode == BRIDGE_MODE_WIFI_FULL) {
    handleAlpacaDiscovery();
    handleLX200Server();
    handleStellariumServer();
    serviceTelnetConsole();
    return;
  }

  serviceTelnetConsole();
}

void serviceNetworkDuringMountWait() {
  serviceHttpServers();

  if (bridgeMode == BRIDGE_MODE_WIFI_FULL) {
    handleLX200Server();
    handleStellariumServer();
    handleAlpacaDiscovery();
  }

#if HAS_CLASSIC_BT
  if (bridgeMode == BRIDGE_MODE_BT_MIN_WEB) {
    delay(1);
    yield();
  }
#endif

  serviceNtpSync();
  delay(1);
  yield();
}

bool parseIpAddress(const String &s, IPAddress &ip) {
  int parts[4] = {0, 0, 0, 0};
  int part = 0;
  String cur = "";

  for (unsigned int i = 0; i <= s.length(); i++) {
    char c = (i < s.length()) ? s[i] : '.';
    if (c == '.') {
      if (cur.length() == 0 || part > 3) return false;
      int v = cur.toInt();
      if (v < 0 || v > 255) return false;
      parts[part++] = v;
      cur = "";
    } else if (c >= '0' && c <= '9') {
      cur += c;
    } else {
      return false;
    }
  }

  if (part != 4) return false;
  ip = IPAddress(parts[0], parts[1], parts[2], parts[3]);
  return true;
}

String wifiStatusCodeText(wl_status_t st) {
  switch (st) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN(" + String((int)st) + ")";
  }
}

void scheduleRestart(const String &reason) {
  restartPending = true;
  restartAtMs = millis() + 1500;
  LOGW("Restart scheduled: %s", reason.c_str());
}

void scheduleApRestart(const String &reason) {
  apRestartPending = true;
  apRestartAtMs = millis() + 1500;
  LOG_WIFI_W("Soft AP restart scheduled: %s", reason.c_str());
}

void serviceRestart() {
  if (apRestartPending && millis() >= apRestartAtMs) {
    apRestartPending = false;
    LOG_WIFI_W("Restarting soft AP now to apply AP settings");
#if defined(ESP32)
    WiFi.softAPdisconnect(true);
    delay(150);
    apRunning = false;
    startFallbackAP();
#endif
  }

  if (restartPending && millis() >= restartAtMs) {
    LOGW("Restarting now to apply network/server configuration");
    stopBluetoothService();
    delay(100);
    ESP.restart();
  }
}

void runtimeWifiOff() {
#if defined(ESP32)
  setBluetoothTinyWebRuntimeEnabled(false);
  wifiRuntimeEnabled = false;

  if (telnetClient) telnetClient.stop();
  telnetClientConnected = false;
  telnetAuthenticated = false;
  telnetLine = "";
  telnetLastWasCR = false;

  WiFi.disconnect(true, true);
  delay(100);
  WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(150);

  apRunning = false;
  staConnected = false;
  wifiModeText = "WiFi runtime OFF";
  lastWifiStatus = "WiFi runtime OFF from serial command";
  Serial.println("WiFi runtime OFF: AP/STA/tiny web stopped. Bluetooth SPP remains active.");
  Serial.printf("Free heap after WiFi off: %u bytes\n", ESP.getFreeHeap());
#if defined(ESP32)
  Serial.printf("Min free heap after WiFi off: %u bytes\n", ESP.getMinFreeHeap());
#endif
#else
  Serial.println("WiFi runtime OFF is only implemented for ESP32 in this build.");
#endif
}

void runtimeWifiOn() {
#if defined(ESP32)
  if (bridgeMode != BRIDGE_MODE_BT_MIN_WEB) {
    Serial.println("wifi on runtime restore is intended for BT mode only.");
    return;
  }

  wifiRuntimeEnabled = true;
  setBluetoothTinyWebRuntimeEnabled(btLiteBootWebEnabled);

  Serial.println("WiFi runtime ON: restoring saved BT WiFi mode...");
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(150);

  setupWiFiFromSavedConfig();
  if (btLiteBootWebEnabled) {
    tinySetupServer.begin();
    Serial.println("[BT_MIN] tiny setup web server restored by wifi on");
  } else {
    Serial.println("[BT_MIN] tiny setup web server remains disabled by saved Telnet-only option");
  }
  startTelnetConsoleServer("BT wifi on");

  wifiModeText = String("BT runtime WiFi + ") + wifiModeText;
  lastWifiStatus = String("BT runtime WiFi restored. ") + lastWifiStatus;
  Serial.printf("WiFi runtime ON: mode=%s AP=%s APIP=%s STAIP=%s\n",
                wifiModeText.c_str(),
                runtimeApSsid().c_str(),
                WiFi.softAPIP().toString().c_str(),
                WiFi.localIP().toString().c_str());
  Serial.printf("Free heap after WiFi on: %u bytes\n", ESP.getFreeHeap());
#if defined(ESP32)
  Serial.printf("Min free heap after WiFi on: %u bytes\n", ESP.getMinFreeHeap());
#endif
#else
  Serial.println("WiFi runtime ON is only implemented for ESP32 in this build.");
#endif
}

const char* bridgeModeName() {
  return bridgeMode == BRIDGE_MODE_BT_MIN_WEB ? "BT / tiny setup AP" : "Full WiFi web";
}

static bool readStartupModePinActive(int pin) {
#if defined(ESP32)
  if (!GPIO_STARTUP_MODE_ENABLED) return false;
  if (pin < 0) return false;
  pinMode(pin, INPUT_PULLUP);
  delay(GPIO_STARTUP_SAMPLE_DELAY_MS);
  int v = digitalRead(pin);
#if GPIO_STARTUP_ACTIVE_LOW
  return v == LOW;
#else
  return v == HIGH;
#endif
#else
  return false;
#endif
}

void applyGpioStartupModeOverride() {
#if defined(ESP32)
  if (!GPIO_STARTUP_MODE_ENABLED) {
    startupModeSource = "saved settings";
    startupModePinUsed = -1;
    return;
  }

  if (readStartupModePinActive(GPIO_STARTUP_FULL_WIFI_PIN)) {
    bridgeMode = BRIDGE_MODE_WIFI_FULL;
    btLiteBootWebEnabled = true;
    setBluetoothTinyWebRuntimeEnabled(true);
    startupModeSource = "GPIO force Full WiFi";
    startupModePinUsed = GPIO_STARTUP_FULL_WIFI_PIN;
    Serial.printf("[BOOT] GPIO startup override: GPIO%d active -> Full WiFi web mode\n", startupModePinUsed);
    return;
  }

  if (readStartupModePinActive(GPIO_STARTUP_BT_WEB_PIN)) {
    bridgeMode = BRIDGE_MODE_BT_MIN_WEB;
    btLiteBootWebEnabled = true;
    setBluetoothTinyWebRuntimeEnabled(true);
    startupModeSource = "GPIO force BT + web";
    startupModePinUsed = GPIO_STARTUP_BT_WEB_PIN;
    Serial.printf("[BOOT] GPIO startup override: GPIO%d active -> BT + tiny web\n", startupModePinUsed);
    return;
  }

  if (readStartupModePinActive(GPIO_STARTUP_BT_TELNET_PIN)) {
    bridgeMode = BRIDGE_MODE_BT_MIN_WEB;
    btLiteBootWebEnabled = false;
    setBluetoothTinyWebRuntimeEnabled(false);
    startupModeSource = "GPIO force BT Telnet-only";
    startupModePinUsed = GPIO_STARTUP_BT_TELNET_PIN;
    Serial.printf("[BOOT] GPIO startup override: GPIO%d active -> BT Telnet-only\n", startupModePinUsed);
    return;
  }

  startupModeSource = "saved settings";
  startupModePinUsed = -1;
  Serial.println("[BOOT] GPIO startup override: no startup mode pin active; using saved settings");
#else
  startupModeSource = "saved settings";
  startupModePinUsed = -1;
#endif
}

String gpioStartupModeText() {
  String s;
#if defined(ESP32)
  s += "GPIO startup override: ";
  s += GPIO_STARTUP_MODE_ENABLED ? "enabled" : "disabled";
  s += "\n";
  s += "Startup mode source: ";
  s += startupModeSource;
  s += "\n";
  s += "Startup mode pin used: ";
  s += String(startupModePinUsed);
  s += "\n";
  s += "Pins active-low to GND: Full WiFi GPIO";
  s += String(GPIO_STARTUP_FULL_WIFI_PIN);
  s += ", BT+Web GPIO";
  s += String(GPIO_STARTUP_BT_WEB_PIN);
  s += ", BT Telnet-only GPIO";
  s += String(GPIO_STARTUP_BT_TELNET_PIN);
  s += "\n";
  s += "Priority if multiple pins are grounded: Full WiFi, then BT+Web, then BT Telnet-only\n";
#else
  s += "GPIO startup override: unavailable on this board\n";
#endif
  return s;
}

void startFallbackAP() {
  if (!apRunning) {
#if defined(ESP32)
    WiFi.mode(staRuntimeDisabled ? WIFI_AP : WIFI_STA);
#endif
    IPAddress ip;
    if (!parseIpAddress(apIp, ip)) {
      LOG_WIFI_W("Invalid AP IP '%s', using 192.168.4.1", apIp.c_str());
      ip = IPAddress(192, 168, 4, 1);
      apIp = "192.168.4.1";
    }

    IPAddress gw = ip;
    IPAddress mask(255, 255, 255, 0);
    bool cfgOk = WiFi.softAPConfig(ip, gw, mask);
    bool apOk = WiFi.softAP(runtimeApSsid().c_str(), apPass.length() ? apPass.c_str() : nullptr);
    apRunning = cfgOk && apOk;
    if (!apRunning) {
      LOG_WIFI_E("Fallback AP start FAILED: cfg=%d ap=%d ssid=%s ip=%s", cfgOk ? 1 : 0, apOk ? 1 : 0, apSsid.c_str(), apIp.c_str());
    }
  }
  LOG_WIFI_I("Fallback AP active: SSID=%s IP=%s running=%d", apSsid.c_str(), WiFi.softAPIP().toString().c_str(), apRunning ? 1 : 0);
}

bool connectConfiguredSTA() {
  staConnected = false;

  if (!staConfigured || staSsid.length() == 0) {
    lastWifiStatus = "No STA configured; AP fallback active";
    startFallbackAP();
    wifiModeText = "AP";
    return false;
  }

#if defined(ESP32)
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, true);
  delay(150);
#endif

  LOG_WIFI_I("Connecting STA to SSID '%s' passLen=%u", staSsid.c_str(), (unsigned)staPass.length());
  WiFi.begin(staSsid.c_str(), staPass.c_str());

  unsigned long start = millis();
  wl_status_t lastStatusSeen = WiFi.status();
  while (millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    wl_status_t s = WiFi.status();
    if (s != lastStatusSeen) {
      lastStatusSeen = s;
      LOG_WIFI_I("STA status: %s", wifiStatusCodeText(s).c_str());
    }

    if (s == WL_CONNECTED) {
      staConnected = true;
      lastWifiStatus = "STA connected";
      wifiModeText = apRunning ? "STA-only/AP-only" : "STA";
      LOG_WIFI_I("STA connected: SSID=%s IP=%s RSSI=%d", staSsid.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
      saveLastStaIp(WiFi.localIP().toString());

      if (!ntpEnabled) {
        ntpEnabled = true;
        bool ntpSaveOk = savePersistentSettings();
        LOG_TIME_I("STA connected: NTP automatically enabled and persistent save=%s",
                   ntpSaveOk ? "ok" : "failed");
      }
      syncTimeFromNTP(true);
      return true;
    }

    delay(250);
    yield();
  }

  wl_status_t finalStatus = WiFi.status();
  staConnected = false;
  lastWifiStatus = "STA failed: " + wifiStatusCodeText(finalStatus);
  LOG_WIFI_W("STA connect failed for SSID '%s'; finalStatus=%s; keeping AP fallback active", staSsid.c_str(), wifiStatusCodeText(finalStatus).c_str());
  startFallbackAP();
  wifiModeText = "AP fallback";
  return false;
}

void setupWiFiFromSavedConfig() {
  loadWiFiConfig();

#if defined(ESP32)
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  WiFi.softAPdisconnect(true);
  delay(150);
#endif

  apRunning = false;
  staConnected = false;

  if (!staRuntimeDisabled && staSsid.length() > 0) {
#if defined(ESP32)
    WiFi.mode(WIFI_STA);
    delay(100);
#endif
    wifiModeText = "STA only";
    lastWifiStatus = "Trying STA-only connection";
    Serial.printf("[WIFI] Exclusive mode: STA-only, SSID=%s\n", staSsid.c_str());

    connectConfiguredSTA();

    if (WiFi.status() == WL_CONNECTED) {
      staConnected = true;
      apRunning = false;
      wifiModeText = "STA only";
      lastWifiStatus = String("STA connected: ") + WiFi.localIP().toString();
      Serial.printf("[WIFI] STA-only connected, IP=%s\n", WiFi.localIP().toString().c_str());
      return;
    }

    Serial.println("[WIFI] STA-only failed; switching to AP-only fallback");
#if defined(ESP32)
    WiFi.disconnect(true, true);
    delay(150);
    WiFi.mode(WIFI_OFF);
    delay(100);
#endif
  }

#if defined(ESP32)
  WiFi.mode(WIFI_AP);
  delay(100);
#endif
  staConnected = false;
  apRunning = false;
  startFallbackAP();
  wifiModeText = "AP only";
  lastWifiStatus = String("AP-only mode: ") + WiFi.softAPIP().toString();
  Serial.printf("[WIFI] AP-only active, IP=%s\n", WiFi.softAPIP().toString().c_str());
}

void allocateNetworkServiceObjects() {
#if defined(ESP8266)
  if (bridgeMode == BRIDGE_MODE_WIFI_FULL) {
    webServer = new ESP8266WebServer(HTTP_WEB_PORT);
    alpacaServer = new ESP8266WebServer(ALPACA_PORT);
    lx200ServerPtr = new WiFiServer(LX200_PORT);
    stellariumServerPtr = new WiFiServer(STELLARIUM_PORT);
  } else {
    webServer = nullptr;
    alpacaServer = nullptr;
    lx200ServerPtr = nullptr;
    stellariumServerPtr = nullptr;
    telnetServerPtr = nullptr;
  }
#else
  if (bridgeMode == BRIDGE_MODE_WIFI_FULL) {
    webServer = new WebServer(HTTP_WEB_PORT);
    alpacaServer = new WebServer(ALPACA_PORT);
    lx200ServerPtr = new WiFiServer(LX200_PORT);
    stellariumServerPtr = new WiFiServer(STELLARIUM_PORT);
  } else {
    webServer = nullptr;
    alpacaServer = nullptr;
    lx200ServerPtr = nullptr;
    stellariumServerPtr = nullptr;
  }
#endif
}

void startFullNetworkListeners() {
  Serial.println("[BOOT] starting UDP/LX200/Stellarium/Telnet listeners");
  discoveryUdp.begin(ALPACA_DISCOVERY_PORT);
  if (lx200ServerPtr) lx200Server().begin();
  if (stellariumServerPtr) stellariumServer().begin();
  startTelnetConsoleServer("Full WiFi boot");
  Serial.printf("[BOOT] TCP/UDP listeners started; Telnet console port %u\n", TELNET_PORT);
}

void startTinySetupServer() {
#if defined(ESP32)
  tinySetupServer.begin();
#endif
}

void handleAlpacaDiscovery() {
  int packetSize = discoveryUdp.parsePacket();
  if (!packetSize) return;

  char packet[96];
  int len = discoveryUdp.read(packet, sizeof(packet) - 1);
  if (len < 0) len = 0;
  packet[len] = 0;

  IPAddress remoteIp = discoveryUdp.remoteIP();
  uint16_t remotePort = discoveryUdp.remotePort();

  String response = "{\"AlpacaPort\":";
  response += String(ALPACA_PORT);
  response += "}";

  discoveryUdp.beginPacket(remoteIp, remotePort);
  discoveryUdp.write((const uint8_t*)response.c_str(), response.length());
  discoveryUdp.endPacket();

  LOGI("Alpaca discovery response sent to %s:%u", remoteIp.toString().c_str(), remotePort);
}

void handleLX200Server() {
  if (!lx200ServerPtr) return;

  if (!lx200Client || !lx200Client.connected()) {
    if (lx200WifiClientConnected) {
      LOGI("LX200/SkySafari WiFi client disconnected");
      lx200WifiClientConnected = false;
    }

    WiFiClient newClient = lx200Server().available();
    if (newClient) {
      lx200Client = newClient;
      lx200Client.setNoDelay(true);
      lx200WifiClientConnected = true;
      lx200WifiLastRxMs = 0;
      lx200WifiLastTxMs = 0;
      LOGI("LX200/SkySafari WiFi client connected from %s:%u",
           lx200Client.remoteIP().toString().c_str(),
           lx200Client.remotePort());
    }
    return;
  }

  int avail = lx200Client.available();
  if (avail > 0) {
    LOGD("LX200/SkySafari WiFi has %d byte(s) available", avail);
  }

  for (uint8_t i = 0; i < 8 && processLX200Stream(lx200Client, LX_SRC_WIFI); i++) {
    yield();
  }
}

void handleStellariumServer() {
  if (!stellariumServerPtr) return;

  if (!stellariumClient || !stellariumClient.connected()) {
    if (stellariumClientConnected) {
      LOG_STEL_I("Stellarium client disconnected");
      stellariumClientConnected = false;
    }

    WiFiClient newClient = stellariumServer().available();
    if (newClient) {
      stellariumClient = newClient;
      stellariumClientConnected = true;
      stellariumLastRxMs = 0;
      stellariumLastTxMs = 0;
      LOG_STEL_I("Stellarium client connected from %s:%u",
           stellariumClient.remoteIP().toString().c_str(),
           stellariumClient.remotePort());
      markPositionDemand("Stellarium connect");
      sendStellariumPosition();
    }
    return;
  }

  int avail = stellariumClient.available();
  if (avail > 0) {
    LOG_STEL_D("Stellarium client has %d byte(s) available", avail);
  }

  static uint8_t stellariumRxBuf[64];
  static uint8_t stellariumRxLen = 0;

  while (stellariumClient.available() > 0 && stellariumRxLen < sizeof(stellariumRxBuf)) {
    int ch = stellariumClient.read();
    if (ch < 0) break;
    stellariumRxBuf[stellariumRxLen++] = (uint8_t)ch;
  }

  while (stellariumRxLen >= 2) {
    uint16_t len = readLE16Network(stellariumRxBuf);

    if (len < 4 || len > sizeof(stellariumRxBuf)) {
      stellariumBadPackets++;
      LOGW("Stellarium bad packet length %u; dropping client", len);
      stellariumClient.stop();
      stellariumClientConnected = false;
      stellariumRxLen = 0;
      return;
    }

    if (stellariumRxLen < len) {
      LOGD("Stellarium partial packet: have=%u need=%u", (unsigned)stellariumRxLen, len);
      return;
    }

    uint8_t pkt[64];
    memcpy(pkt, stellariumRxBuf, len);

    if (stellariumRxLen > len) {
      memmove(stellariumRxBuf, stellariumRxBuf + len, stellariumRxLen - len);
      stellariumRxLen -= len;
    } else {
      stellariumRxLen = 0;
    }

    handleStellariumPacket(pkt, len);
  }

  static unsigned long lastStellariumPositionMs = 0;
  if (millis() - lastStellariumPositionMs > 1000) {
    lastStellariumPositionMs = millis();
    sendStellariumPosition();
  }
}

void startTelnetConsoleServer(const char* reason) {
#if defined(ESP32)
  if (!telnetServerPtr) telnetServerPtr = new WiFiServer(TELNET_PORT);
  telnetServer().begin();
  Serial.printf("[TELNET] console server started on port %u (%s)\n", TELNET_PORT, reason ? reason : "no reason");
#endif
}

void serviceTelnetConsole() {
#if defined(ESP32)
  if (!wifiRuntimeEnabled) return;
  if (!telnetServerPtr) return;

  if (!telnetClient || !telnetClient.connected()) {
    if (telnetMonitorActive) {
      telnetMonitorActive = false;
      telnetLiveLogEnabled = telnetMonitorSavedLiveLog;
    }
    if (telnetTasksActive) {
      telnetTasksActive = false;
      telnetLiveLogEnabled = telnetTasksSavedLiveLog;
    }
    if (telnetClient) telnetClient.stop();
    WiFiClient nc = telnetServer().available();
    if (nc) {
      telnetClient = nc;
      telnetClientConnected = true;
      telnetAuthenticated = (telnetPassword.length() == 0);
      telnetLine = "";
      telnetLastWasCR = false;
      telnetMonitorActive = false;
      telnetTasksActive = false;
      telnetLastRxMs = millis();
      TelnetCRLFPrint telnetOut(telnetClient);
      telnetOut.println();
      telnetOut.printf("%s %s Telnet Console\n", FW_NAME, FW_VERSION);
      if (telnetAuthenticated) {
        telnetOut.println("Password disabled. Type help.");
        telnetOut.print("> ");
      } else {
        telnetOut.print("Password: ");
      }
      Serial.printf("[telnet] client connected on port %u\n", TELNET_PORT);
    }
    return;
  }

  TelnetCRLFPrint telnetOut(telnetClient);
  if (telnetAuthenticated && telnetMonitorActive && millis() - telnetMonitorLastDrawMs >= telnetMonitorRefreshMs) {
    telnetMonitorLastDrawMs = millis();
    telnetDrawMonitor(telnetOut);
  }
  if (telnetAuthenticated && telnetTasksActive && millis() - telnetTasksLastDrawMs >= telnetTasksRefreshMs) {
    telnetTasksLastDrawMs = millis();
    telnetDrawTasks(telnetOut);
  }

  while (telnetClient.available()) {
    uint8_t ch = (uint8_t)telnetClient.read();

    if (ch == 255) {
      if (telnetClient.available()) telnetClient.read();
      if (telnetClient.available()) telnetClient.read();
      continue;
    }

    if (telnetMonitorActive) {
      if (ch == '\n' && telnetLastWasCR) {
        telnetLastWasCR = false;
        continue;
      }
      if (ch == 'q' || ch == 'Q' || ch == 3 || ch == '\r' || ch == '\n') {
        telnetLastWasCR = (ch == '\r');
        telnetStopMonitor(telnetOut);
        if (telnetClient && telnetClient.connected()) telnetOut.print("> ");
      } else {
        telnetLastWasCR = false;
      }
      continue;
    }

    if (telnetTasksActive) {
      if (ch == '\n' && telnetLastWasCR) {
        telnetLastWasCR = false;
        continue;
      }
      if (ch == 'q' || ch == 'Q' || ch == 3 || ch == '\r' || ch == '\n') {
        telnetLastWasCR = (ch == '\r');
        telnetStopTasks(telnetOut);
        if (telnetClient && telnetClient.connected()) telnetOut.print("> ");
      } else {
        telnetLastWasCR = false;
      }
      continue;
    }

    bool telnetEndOfLine = false;
    if (ch == '\r') {
      telnetEndOfLine = true;
      telnetLastWasCR = true;
    } else if (ch == '\n') {
      if (telnetLastWasCR) {
        telnetLastWasCR = false;
        continue;
      }
      telnetEndOfLine = true;
    } else {
      telnetLastWasCR = false;
    }

    if (telnetEndOfLine) {
      String line = telnetLine;
      telnetLine = "";
      line.trim();

      if (!telnetAuthenticated) {
        if (line == telnetPassword) {
          telnetAuthenticated = true;
          telnetOut.println();
          telnetOut.println("Authenticated. Type help.");
        } else {
          telnetAuthFailures++;
          telnetOut.println();
          telnetOut.println("Bad password. Closing.");
          telnetClient.stop();
          Serial.println("[telnet] bad password; client disconnected");
          return;
        }
      } else {
        telnetOut.println();
        telnetExecuteCommand(line, telnetOut);
      }

      if (telnetClient && telnetClient.connected()) telnetOut.print("> ");
      continue;
    }

    if (ch == 8 || ch == 127) {
      if (telnetLine.length() > 0) {
        telnetLine.remove(telnetLine.length() - 1);
      }
      continue;
    }

    if (ch >= 32 && ch <= 126) {
      if (telnetLine.length() < 160) {
        telnetLine += char(ch);
      }
    }
  }
#endif
}

bool lx200WifiClientActive() {
  return lx200Client && lx200Client.connected();
}

bool stellariumClientActive() {
  return stellariumClient && stellariumClient.connected();
}

String apIpAddressText() {
  return WiFi.softAPIP().toString();
}

String staIpAddressText() {
  return WiFi.localIP().toString();
}

int apClientCount() {
  return WiFi.softAPgetStationNum();
}
