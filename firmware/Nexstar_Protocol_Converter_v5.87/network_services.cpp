#include "network_services.h"

#include "bluetooth_services.h"
#include "logging.h"
#include "lx200_protocol.h"
#include "mount_transport.h"
#include "observer_time.h"
#include "position_cache.h"
#include "settings.h"
#include "slew_controller.h"

#if defined(ESP32)
  #include "esp_heap_caps.h"
  #include "esp_system.h"
#endif

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
bool telnetLiveLogEnabled = false;
uint32_t telnetLiveLogLines = 0;
bool telnetRedTextEnabled = false;
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

bool telnetMenuActive = false;
static bool telnetMenuSavedLiveLog = true;
static uint8_t telnetMenuScreen = 0; // 0 main, 1 nudge, 2 logging, 3 confirm full WiFi, 4 confirm reboot, 5 info
static uint8_t telnetMenuSelection = 0;
// Focus cycles with Tab: 0 menu body, 1 Reboot header action, 2 BT/WiFi header action.
static uint8_t telnetMenuFocus = 0;
static uint8_t telnetMenuEscapeState = 0;
static bool telnetMenuIgnoreNextLf = false;
static uint8_t telnetMenuEscapeParam = 0;
static bool telnetMenuEscapeHasParam = false;
static char telnetMenuNumericPrefix[5] = "";
static uint8_t telnetMenuNumericLength = 0;
static bool telnetMenuUtf8 = false;
static bool telnetMenuWindowsCompat = true; // Windows 11 built-in telnet.exe safe subset
static const char *telnetMenuInfoCommand = nullptr;
static char telnetMenuPageCommand[48] = "";
static bool telnetMenuPageRefreshable = false;
static uint8_t telnetMenuReturnScreen = 0;
static uint8_t telnetMenuReturnSelection = 0;
static const size_t TELNET_MENU_PAGE_BUFFER_SIZE = 4096;
static const uint8_t TELNET_MENU_PAGE_LINES = 18;
static char telnetMenuPageBuffer[TELNET_MENU_PAGE_BUFFER_SIZE];
static char telnetMenuPreviousPageBuffer[TELNET_MENU_PAGE_BUFFER_SIZE];
static char telnetMenuPageTitle[48] = "Command Output";
static size_t telnetMenuPageLength = 0;
static size_t telnetMenuPageStart = 0;
static uint16_t telnetMenuPageNumber = 0;
static uint16_t telnetTermWidth = 80;
static uint16_t telnetTermHeight = 24;
static bool telnetNawsReceived = false;
static uint8_t telnetIacState = 0; // 0 data, 1 IAC, 2 option verb, 3 SB option, 4 SB data, 5 SB IAC
static uint8_t telnetIacVerb = 0;
static uint8_t telnetSbOption = 0;
static uint8_t telnetSbData[8];
static uint8_t telnetSbLength = 0;
static unsigned long telnetLastKeepaliveMs = 0;
static unsigned long telnetLastUiClockMs = 0;
static unsigned long telnetMenuLastPageRefreshMs = 0;
// Menu-owned status/information auto-refresh. Disabled by default for
// stability. It can be enabled from Setup at conservative intervals.
static unsigned long telnetMenuAutoRefreshMs = 0UL;
static const uint8_t TELNET_BANNER_ROWS = 5;
static char telnetBannerCache[TELNET_BANNER_ROWS][160] = {{0}};
static const unsigned long TELNET_KEEPALIVE_MS = 60000UL;
static const unsigned long TELNET_IDLE_TIMEOUT_MS = 30UL * 60UL * 1000UL;

class TelnetMenuBufferPrint : public Print {
public:
  TelnetMenuBufferPrint() : truncated(false), ansiState(0) {
    telnetMenuPageLength = 0;
    telnetMenuPageBuffer[0] = '\0';
  }
  size_t write(uint8_t b) override {
    // Strip ANSI/VT control sequences from captured console output. Live commands
    // such as tasks/monitor emit cursor controls that must never enter a paged view.
    if (ansiState == 1) {
      if (b == '[') ansiState = 2;
      else ansiState = 0;
      return 1;
    }
    if (ansiState == 2) {
      if (b >= 0x40 && b <= 0x7E) ansiState = 0;
      return 1;
    }
    if (b == 0x1B) { ansiState = 1; return 1; }
    if (b == '\r') return 1;
    if (telnetMenuPageLength + 1 >= TELNET_MENU_PAGE_BUFFER_SIZE) {
      truncated = true;
      return 0;
    }
    telnetMenuPageBuffer[telnetMenuPageLength++] = (char)b;
    telnetMenuPageBuffer[telnetMenuPageLength] = '\0';
    return 1;
  }
  size_t write(const uint8_t *data, size_t size) override {
    size_t written = 0;
    for (size_t i = 0; i < size; ++i) written += write(data[i]);
    return written;
  }
  bool truncated;
private:
  uint8_t ansiState;
};

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
extern void telnetDrawMonitor(Print &out);
extern void telnetStopMonitor(Print &out);
extern void telnetDrawTasks(Print &out);
extern void telnetStopTasks(Print &out);
extern void telnetExecuteCommand(String line, Print &out);
extern void telnetStartMenu(Print &out);
extern void telnetStopMenu(Print &out);
extern String basicStatusText();
extern String basicSystemHealthText();
extern String systemHealthText();
extern String sampleWebCpuLoadText();
extern String sampleBannerSystemText();
extern String currentTimezoneAbbrev();

static const char* telnetResetReasonName() {
#if defined(ESP32)
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
#else
  return "unknown";
#endif
}

static void telnetSerialDiag(const char* event) {
#if defined(ESP32)
  Serial.printf("[TELNET_DIAG] %s free=%u min=%u largest=%u heap_ok=%d reset=%s loop_stack_hwm=%u\n",
                event ? event : "event",
                ESP.getFreeHeap(),
                ESP.getMinFreeHeap(),
                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                heap_caps_check_integrity_all(false) ? 1 : 0,
                telnetResetReasonName(),
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
#else
  Serial.printf("[TELNET_DIAG] %s\n", event ? event : "event");
#endif
}

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
  // Buffer terminal output so borders, padded rows, and ANSI sequences are sent
  // in a few large TCP writes instead of hundreds of one-byte writes.
  explicit TelnetCRLFPrint(Print &wrapped)
      : out(wrapped), lastWasCR(false), used(0) {}

  ~TelnetCRLFPrint() override { flush(); }

  size_t write(uint8_t b) override {
    size_t accepted = 0;
    if (b == '\n' && !lastWasCR) {
      appendByte('\r');
      accepted++;
    }
    appendByte(b);
    accepted++;
    lastWasCR = (b == '\r');
    if (b != '\r' && b != '\n') lastWasCR = false;
    return accepted;
  }

  size_t write(const uint8_t *data, size_t size) override {
    if (!data || !size) return 0;
    size_t accepted = 0;
    for (size_t i = 0; i < size; ++i) accepted += write(data[i]);
    return accepted;
  }

  void flush() override {
    if (used) {
      out.write(txBuffer, used);
      used = 0;
    }
    out.flush();
  }

private:
  static constexpr size_t TX_BUFFER_SIZE = 512;

  void appendByte(uint8_t b) {
    if (used >= TX_BUFFER_SIZE) flush();
    txBuffer[used++] = b;
  }

  Print &out;
  bool lastWasCR;
  size_t used;
  uint8_t txBuffer[TX_BUFFER_SIZE];
};

static uint16_t readLE16Network(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint16_t readLE16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

int32_t readLE32s(const uint8_t *p) {
  uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  return (int32_t)v;
}

uint32_t readLE32u(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int64_t readLE64s(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) {
    v <<= 8;
    v |= p[i];
  }
  return (int64_t)v;
}

void writeLE16(uint8_t *p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}

void writeLE32(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

void writeLE64(uint8_t *p, int64_t v) {
  uint64_t u = (uint64_t)v;
  for (int i = 0; i < 8; i++) {
    p[i] = (uint8_t)(u & 0xFF);
    u >>= 8;
  }
}

uint32_t stellariumRaToUint(double raDeg) {
  raDeg = normalizeRA(raDeg);
  double v = (raDeg / 360.0) * 4294967296.0;
  if (v < 0) v = 0;
  if (v > 4294967295.0) v = 4294967295.0;
  return (uint32_t)(v + 0.5);
}

int32_t stellariumDecToInt(double decDeg) {
  double v = (decDeg / 360.0) * 4294967296.0;
  if (v < -2147483648.0) v = -2147483648.0;
  if (v > 2147483647.0) v = 2147483647.0;
  return (int32_t)(v + (v >= 0 ? 0.5 : -0.5));
}

double stellariumUintToRaDeg(uint32_t raw) {
  return normalizeRA(((double)raw / 4294967296.0) * 360.0);
}

double stellariumIntToDecDeg(int32_t raw) {
  return ((double)raw / 4294967296.0) * 360.0;
}

void sendStellariumPosition() {
  if (!stellariumClient || !stellariumClient.connected()) return;
  markPositionDemand("Stellarium position TX");

  double ra = 0.0;
  double dec = 0.0;

  if (!mountPositionApiRaDec(ra, dec, nullptr)) {
    LOGW("Stellarium position send skipped: no RA/Dec cache");
    return;
  }

  uint8_t pkt[24];
  memset(pkt, 0, sizeof(pkt));

  writeLE16(pkt + 0, 24);
  writeLE16(pkt + 2, 0); // current position
  writeLE64(pkt + 4, (int64_t)millis());
  writeLE32(pkt + 12, stellariumRaToUint(ra));
  writeLE32(pkt + 16, (uint32_t)stellariumDecToInt(dec));
  writeLE32(pkt + 20, 0); // status OK/idle

  stellariumClient.write(pkt, sizeof(pkt));
  stellariumLastTxMs = millis();
  stellariumTxPackets++;
  LOG_STEL_D("Stellarium TX position packet #%lu RA=%.6f DEC=%.6f",
       (unsigned long)stellariumTxPackets, ra, dec);
}

void handleStellariumPacket(uint8_t *pkt, uint16_t len) {
  markPositionDemand("Stellarium RX");
  stellariumLastRxMs = millis();
  stellariumRxPackets++;

  if (len < 4) {
    stellariumBadPackets++;
    LOGW("Stellarium RX bad short packet len=%u", len);
    return;
  }

  uint16_t type = readLE16(pkt + 2);
  LOGD("Stellarium RX packet #%lu len=%u type=%u",
       (unsigned long)stellariumRxPackets, len, type);

  if (LOG_LEVEL >= LOG_TRACE) {
    String raw = "Stellarium raw:";
    for (uint16_t i = 0; i < len && i < 32; i++) {
      char b[5];
      snprintf(b, sizeof(b), " %02X", pkt[i]);
      raw += b;
    }
    LOGT("%s", raw.c_str());
  }

  // Stellarium telescope-control client sends a 20-byte GOTO packet:
  // uint16 length, uint16 type, int64 time, uint32 RA, int32 DEC.
  if (len >= 20) {
    uint32_t rawRa = readLE32u(pkt + 12);
    int32_t rawDec = readLE32s(pkt + 16);

    double raDeg = stellariumUintToRaDeg(rawRa);
    double decDeg = stellariumIntToDecDeg(rawDec);

    LOG_STEL_I("Stellarium RX packet len=%u type=%u GOTO RA_deg=%.6f DEC_deg=%.6f",
         len, type, raDeg, decDeg);

    if (mountCommandPathBusy()) {
      LOG_STEL_W("Stellarium GOTO queued: mount busy");
      enqueueGotoRaDec(raDeg, decDeg, LX_SRC_WIFI, "Stellarium GOTO while mount busy");
    } else {
      queueAsyncSlew(raDeg, decDeg);
    }

    // Do not overwrite current-position cache with Stellarium target.
    markGotoQueueImmediateAck("Stellarium", "binary GOTO accepted; position response sent from cache");
    sendStellariumPosition();
    return;
  }

  stellariumBadPackets++;
  LOG_STEL_W("Stellarium RX short/unknown packet len=%u type=%u", len, type);
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

  stopTelnetConsoleServer("WiFi runtime down");

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
  wifiRuntimeEnabled = true;
  const bool restoreTinyWeb = (bridgeMode == BRIDGE_MODE_BT_MIN_WEB) && btLiteBootWebEnabled;
  setBluetoothTinyWebRuntimeEnabled(restoreTinyWeb);

  Serial.println("WiFi runtime ON: restoring saved BT WiFi mode...");
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(150);

  setupWiFiFromSavedConfig();
  if (restoreTinyWeb) {
    tinySetupServer.begin();
    Serial.println("[BT_MIN] tiny setup web server restored by runtime WiFi on");
  } else {
    Serial.println("[TELNET] HTTP/HTTPS servers remain disabled during runtime Telnet restore");
  }
  startTelnetConsoleServer("BT wifi on");

  wifiModeText = String("Telnet runtime WiFi + ") + wifiModeText;
  lastWifiStatus = String("Telnet runtime WiFi restored. ") + lastWifiStatus;
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
    btLiteBootWebEnabled = false;
    setBluetoothTinyWebRuntimeEnabled(false);
    startupModeSource = "GPIO force BT Telnet-only";
    startupModePinUsed = GPIO_STARTUP_BT_WEB_PIN;
    Serial.printf("[BOOT] GPIO startup override: GPIO%d active -> BT Telnet-only (tiny web removed)\n", startupModePinUsed);
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


// ---------------- VT-100 Telnet UI ----------------
// All escape helpers are centralized to keep terminal behavior consistent.
static void vtCursor(Print &out, uint16_t row, uint16_t col) { out.printf("\x1B[%u;%uH", row, col); } // CSI r;c H absolute cursor
static void vtUp(Print &out, uint16_t n=1) { out.printf("\x1B[%uA", n); } // CSI n A cursor up
static void vtDown(Print &out, uint16_t n=1) { out.printf("\x1B[%uB", n); } // CSI n B cursor down
static void vtRight(Print &out, uint16_t n=1) { out.printf("\x1B[%uC", n); } // CSI n C cursor right
static void vtLeft(Print &out, uint16_t n=1) { out.printf("\x1B[%uD", n); } // CSI n D cursor left
static void vtSave(Print &out) { out.print("\x1B\x37\x1B[s"); } // ESC 7 / CSI s save cursor
static void vtRestore(Print &out) { out.print("\x1B\x38\x1B[u"); } // ESC 8 / CSI u restore cursor
static void vtClear(Print &out) { out.print("\x1B[2J\x1B[H"); } // CSI 2 J clear screen, CSI H home cursor
static void vtClearEnd(Print &out) { out.print("\x1B[0J"); } // CSI 0 J clear to end screen
static void vtClearLine(Print &out) { out.print("\x1B[2K"); } // CSI 2 K clear whole line
static void vtScrollRegion(Print &out, uint16_t top, uint16_t bottom) { if (!telnetMenuWindowsCompat) out.printf("\x1B[%u;%ur", top, bottom); } // CSI t;b r; disabled for Windows telnet.exe
static void vtResetScroll(Print &out) { if (!telnetMenuWindowsCompat) out.print("\x1B[r"); } // CSI r reset margins; disabled for Windows telnet.exe
static void vtHideCursor(Print &out) { if (!telnetMenuWindowsCompat) out.print("\x1B[?25l\x1B[?12l"); } // DEC private cursor modes; disabled for Windows telnet.exe
static void vtShowCursor(Print &out) { if (!telnetMenuWindowsCompat) out.print("\x1B[?25h\x1B[?12h"); } // DEC private cursor modes; disabled for Windows telnet.exe
static void vtAltOn(Print &out) { if (!telnetMenuWindowsCompat) out.print("\x1B[?47h\x1B[?1049h"); } // alternate screen; disabled for Windows telnet.exe
static void vtAltOff(Print &out) { if (!telnetMenuWindowsCompat) out.print("\x1B[?1049l\x1B[?47l"); } // leave alternate screen; disabled for Windows telnet.exe
void telnetApplyTextColor(Print &out) { out.print(telnetRedTextEnabled ? "\x1B[31m" : "\x1B[0m"); }
static void vtReset(Print &out) { out.print("\x1B[0m"); if (telnetRedTextEnabled) out.print("\x1B[31m"); } // SGR reset
static void vtSelected(Print &out) { out.print("\x1B[7m"); } // SGR reverse only: safest Windows telnet.exe highlight
static void vtDim(Print &out) { if (!telnetMenuWindowsCompat) out.print("\x1B[2m"); } // SGR dim; omitted for Windows telnet.exe
static void vtTitle(Print &out) { if (!telnetMenuWindowsCompat) out.print("\x1B[1;96m"); else out.print("\x1B[1m"); } // basic bold in Windows compatibility mode
static void vtStatus(Print &out) { if (!telnetMenuWindowsCompat) out.print("\x1B[33m"); } // color omitted for Windows telnet.exe

static const char *gTL(){return telnetMenuUtf8?"╔":"+";} static const char *gTR(){return telnetMenuUtf8?"╗":"+";}
static const char *gBL(){return telnetMenuUtf8?"╚":"+";} static const char *gBR(){return telnetMenuUtf8?"╝":"+";}
static const char *gML(){return telnetMenuUtf8?"╠":"+";} static const char *gMR(){return telnetMenuUtf8?"╣":"+";}
static const char *gV(){return telnetMenuUtf8?"║":"|";} static const char *gH(){return telnetMenuUtf8?"═":"=";}
static const char *gSH(){return telnetMenuUtf8?"─":"-";} static const char *gSV(){return telnetMenuUtf8?"│":"|";}
static const char *gUp(){return telnetMenuUtf8?"▲":"^";} static const char *gDn(){return telnetMenuUtf8?"▼":"v";}
static const char *gThumb(){return telnetMenuUtf8?"█":"#";} static const char *gCrumb(){return telnetMenuUtf8?" ▸ ":" > ";}

static uint16_t uiWidth() {
  // Always reserve the terminal's final column because writing there can wrap.
  uint16_t reported = telnetTermWidth < 40 ? 40 : (telnetTermWidth > 120 ? 120 : telnetTermWidth);
  return reported > 40 ? reported - 1 : 40;
}
static uint16_t uiHeight() {
  // Use NAWS/manual height even in Windows compatibility mode. v5.43 incorrectly
  // forced 24 rows here, which placed the footer above the bottom of taller windows.
  return telnetTermHeight < 16 ? 16 : (telnetTermHeight > 60 ? 60 : telnetTermHeight);
}
static uint16_t uiContentTop() {
  // Four frame/header rows plus the five-line split quick-status banner and
  // one separator row.  The former four-row dashboard has been folded into
  // the right side of the banner, returning those rows to menu content.
  return 11;
}
static uint16_t uiContentBottom() { return uiHeight() - 3; }
static uint16_t uiContentRows() { return uiContentBottom() - uiContentTop() + 1; }

static uint8_t menuCount(uint8_t screen);

enum TelnetMenuControlKind : uint8_t {
  TM_CONTROL_ACTION = 0,
  TM_CONTROL_SUBMENU,
  TM_CONTROL_CHECKBOX,
  TM_CONTROL_NUMBER,
  TM_CONTROL_BACK,
  TM_CONTROL_INFO
};

static TelnetMenuControlKind menuControlKind(uint8_t screen, uint8_t index) {
  if (screen == 0) return index < 5 ? TM_CONTROL_SUBMENU : TM_CONTROL_ACTION;
  if (screen == 2) {
    if (index == 8) return TM_CONTROL_BACK;
    return TM_CONTROL_ACTION;
  }
  if (screen == 3) {
    if (index <= 2) return TM_CONTROL_NUMBER;
    if (index == 3) return TM_CONTROL_CHECKBOX;
    if (index == 5) return TM_CONTROL_BACK;
    return TM_CONTROL_ACTION;
  }
  if (screen == 4) {
    if (index == 5) return TM_CONTROL_CHECKBOX;
    if (index == 6) return TM_CONTROL_BACK;
    return TM_CONTROL_ACTION;
  }
  uint8_t count = menuCount(screen);
  if (count && index == count - 1) return TM_CONTROL_BACK;
  return TM_CONTROL_ACTION;
}

static bool menuItemIsCheckbox(uint8_t screen, uint8_t index) {
  return menuControlKind(screen, index) == TM_CONTROL_CHECKBOX;
}

static bool menuItemIsNumber(uint8_t screen, uint8_t index) {
  return menuControlKind(screen, index) == TM_CONTROL_NUMBER;
}

static const char *screenTitle(uint8_t screen) {
  switch(screen) {
    case 0: return "Main"; case 1: return "Control"; case 2: return "Status";
    case 3: return "Setup"; case 4: return "Logs"; case 5: return "Advanced";
    case 6: return "Manual Nudge"; case 7: return "Confirm"; case 8: return telnetMenuPageTitle;
    default: return "Menu";
  }
}
static uint8_t parentScreen(uint8_t screen){ if(screen==6)return 1; if(screen>=1&&screen<=5)return 0; return telnetMenuReturnScreen; }
static const char *menuLabel(uint8_t screen, uint8_t index) {
  static const char *main[] = {"Control", "Status", "Setup", "Logs", "Advanced", "Exit to Telnet prompt"};
  static const char *control[] = {"Read RA/Dec", "Read Alt/Az", "Manual Nudge", "Mount initialization test", "Drain mount input", "Back"};
  static const char *status[] = {"Observer Status - Basic", "System Health - Basic", "Observer Status - Advanced", "System Health - Advanced", "Network Status", "Time / Location", "Protocols / Ports", "FreeRTOS Tasks", "Back"};
  static const char *setup[] = {"Mount poll interval", "Idle poll interval", "Status auto-refresh interval", "Telnet live logging", "", "Back"};
  static const char *logs[] = {"Errors only", "Warnings", "Information", "Debug", "Trace", "Toggle live Telnet logs", "Back"};
  static const char *adv[] = {"GPIO startup pins", "Telnet status", "Restart controller", "Restore AP defaults", "Back"};
  static const char *nudge[] = {"Azimuth +", "Azimuth -", "Altitude +", "Altitude -", "Back"};
  const char **a=nullptr; uint8_t n=0;
  switch(screen){case 0:a=main;n=6;break;case 1:a=control;n=6;break;case 2:a=status;n=9;break;case 3:a=setup;n=6;break;case 4:a=logs;n=7;break;case 5:a=adv;n=5;break;case 6:a=nudge;n=5;break;}
  if(screen==3 && index==4) return bridgeMode==BRIDGE_MODE_WIFI_FULL ? "Switch to BT Telnet mode" : "Switch to Full WiFi mode";
  return (a && index<n)?a[index]:"";
}
static uint8_t menuCount(uint8_t screen) { switch(screen){case 0:return 6;case 1:return 6;case 2:return 9;case 3:return 6;case 4:return 7;case 5:return 5;case 6:return 5;default:return 0;} }

static void drawBorder(Print &out,uint16_t row,const char *left,const char *fill,const char *right){vtCursor(out,row,1);out.print(left);for(uint16_t i=0;i<uiWidth()-2;i++)out.print(fill);out.print(right);}
static void breadcrumb(char *buf,size_t size){
  if(telnetMenuScreen==0) snprintf(buf,size,"Main");
  else if(telnetMenuScreen==6) snprintf(buf,size,"Main%sControl%sManual Nudge",gCrumb(),gCrumb());
  else if(telnetMenuScreen==7||telnetMenuScreen==8) snprintf(buf,size,"Main%s%s%s%s",gCrumb(),screenTitle(telnetMenuReturnScreen),gCrumb(),screenTitle(telnetMenuScreen));
  else snprintf(buf,size,"Main%s%s",gCrumb(),screenTitle(telnetMenuScreen));
}
static void formatBannerColumns(uint8_t index, char *left, size_t leftSize,
                                char *right, size_t rightSize) {
  if (!left || !leftSize || !right || !rightSize) return;
  left[0] = '\0';
  right[0] = '\0';

  int y=0,mo=0,d=0,h=0,mi=0,se=0;
  currentLocalParts(y,mo,d,h,mi,se);
  const char *mountState = "UNKNOWN";
  if (mountCommFault) mountState = "FAULT";
  else if (mountBusy) mountState = "BUSY";
  else if (lastMountResponseMs != 0) mountState = "READY";

  const char *alpacaState = (alpacaServer != nullptr) ? "ON" : "OFF";
  const char *skyState = (lx200WifiClientConnected || bluetoothLX200ClientConnected()) ? "LINK" :
                         ((lx200WifiRxCommands || lx200BtRxCommands) ? "RX" : "WAIT");
  // Match the SkySafari-style activity wording: idle listeners show WAIT,
  // and only recent protocol traffic shows RX.  "LINK" was ambiguous here.
  const char *stelState = stellariumRxPackets ? "RX" : "WAIT";
  const char *fault = (mountCommFault && lastMountFault.length()) ? lastMountFault.c_str() : "none";

  // Fixed labels keep both columns vertically aligned.  Variable data is
  // clipped later to its assigned column width, so it cannot shift neighbors.
  static const int LEFT_LABEL_WIDTH = 7;
  static const int RIGHT_LABEL_WIDTH = 10;

  switch (index) {
    case 0:
      snprintf(left, leftSize, "%-*s: %-7s Poll:%lums",
               LEFT_LABEL_WIDTH, "Mount", mountState,
               (unsigned long)effectiveMountPollIntervalMs());
      snprintf(right, rightSize, "%-*s: Alp:%s Stel:%s Sky:%s",
               RIGHT_LABEL_WIDTH, "Protocols", alpacaState, stelState, skyState);
      break;

    case 1:
      if (mountCurrentRaDecValid) {
        snprintf(left, leftSize, "%-*s: %10.6fh Dec:%+10.6f",
                 LEFT_LABEL_WIDTH, "RA", mountCurrentRA_deg / 15.0,
                 mountCurrentDec_deg);
      } else {
        snprintf(left, leftSize, "%-*s: ----------h Dec:----------",
                 LEFT_LABEL_WIDTH, "RA");
      }
      // Use full protocol abbreviations when the available right-hand banner
      // column is wide enough. drawTopBanner() now shifts this column left to
      // consume otherwise-unused space instead of forcing a 50/50 split.
      snprintf(right, rightSize, "%-*s: Alp:%u Stel:%u Sky:%u",
               RIGHT_LABEL_WIDTH, "Ports", ALPACA_PORT, STELLARIUM_PORT, LX200_PORT);
      break;

    case 2:
      if (mountCurrentAltAzValid) {
        snprintf(left, leftSize, "%-*s: %+9.5f Az:%9.5f",
                 LEFT_LABEL_WIDTH, "Alt/Az", mountCurrentAlt_deg,
                 mountCurrentAz_deg);
      } else {
        snprintf(left, leftSize, "%-*s: --------- Az:---------",
                 LEFT_LABEL_WIDTH, "Alt/Az");
      }
      snprintf(right, rightSize, "%-*s: %.18s",
               RIGHT_LABEL_WIDTH, "Fault", fault);
      break;

    case 3: {
      if (timeValid) {
        String tz = currentTimezoneAbbrev();
        snprintf(left, leftSize, "%-*s: %04d-%02d-%02d %02d:%02d:%02d %s",
                 LEFT_LABEL_WIDTH, "Time", y, mo, d, h, mi, se,
                 tz.length() ? tz.c_str() : "LOCAL");
      } else {
        snprintf(left, leftSize, "%-*s: not set",
                 LEFT_LABEL_WIDTH, "Time");
      }
      String system = sampleBannerSystemText();
      snprintf(right, rightSize, "%-*s: %s",
               RIGHT_LABEL_WIDTH, "System", system.c_str());
      break;
    }

    case 4:
      if (siteValid) {
        snprintf(left, leftSize, "%-*s: %+10.5f Lon:%+11.5f",
                 LEFT_LABEL_WIDTH, "Site", siteLatitudeDeg, siteLongitudeDeg);
      } else {
        snprintf(left, leftSize, "%-*s: latitude/longitude not set",
                 LEFT_LABEL_WIDTH, "Site");
      }
      // Heap values are easier to scan in KiB and need far fewer columns.
      snprintf(right, rightSize, "%-*s: Free:%luK Min:%luK",
               RIGHT_LABEL_WIDTH, "Heap",
               (unsigned long)(ESP.getFreeHeap() / 1024UL),
               (unsigned long)(ESP.getMinFreeHeap() / 1024UL));
      break;
  }
}

static void drawTopBanner(Print &out, bool force) {
  const uint16_t usable = uiWidth() - 2;
  const uint16_t gap = 2;

  // Build all five rows first. The right-hand block is positioned as one
  // unit: every row begins at the same column, and the longest right-side row
  // ends at the final usable terminal column. Internal formatting is unchanged.
  char leftRows[TELNET_BANNER_ROWS][160];
  char rightRows[TELNET_BANNER_ROWS][160];
  size_t longestRight = 0;
  for (uint8_t i = 0; i < TELNET_BANNER_ROWS; ++i) {
    formatBannerColumns(i, leftRows[i], sizeof(leftRows[i]),
                        rightRows[i], sizeof(rightRows[i]));
    const size_t n = strlen(rightRows[i]);
    if (n > longestRight) longestRight = n;
  }

  // Right-justify the complete block. On narrow terminals, retain a minimum
  // left data area and clip the right block only when there is no alternative.
  const uint16_t minLeft = 20;
  uint16_t rightWidth = (uint16_t)longestRight;
  if (rightWidth > usable - gap - minLeft) {
    rightWidth = usable > gap + minLeft ? usable - gap - minLeft : usable / 2;
  }
  const uint16_t rightStart = usable - rightWidth;
  const uint16_t leftWidth = rightStart > gap ? rightStart - gap : 0;

  for (uint8_t i = 0; i < TELNET_BANNER_ROWS; ++i) {
    char line[240];
    memset(line, ' ', sizeof(line));
    const size_t cap = usable < sizeof(line) - 1 ? usable : sizeof(line) - 1;
    line[cap] = '\0';

    if (leftWidth) {
      const size_t leftLen = strnlen(leftRows[i], leftWidth);
      memcpy(line, leftRows[i], leftLen);
    }

    const size_t rightLen = strnlen(rightRows[i], rightWidth);
    memcpy(line + rightStart, rightRows[i], rightLen);

    if (!force && strcmp(line, telnetBannerCache[i]) == 0) continue;
    strlcpy(telnetBannerCache[i], line, sizeof(telnetBannerCache[i]));
    vtCursor(out, 5 + i, 1);
    out.print(gV());
    out.printf("%-*.*s", (int)usable, (int)usable, line);
    out.print(gV());
  }
}

static void drawTitleActionRow(Print &out) {
  const uint16_t w = uiWidth();
  vtCursor(out,2,1); out.print(gV());
  char title[96];
  const char *modeText = bridgeMode == BRIDGE_MODE_WIFI_FULL ? "BT" : "WiFi";
  snprintf(title, sizeof(title), " NexStar Protocol Converter %s", FW_VERSION);

  const int inner = (int)w - 2;
  const char rebootText[] = "[ Reboot ]";
  char modeButton[16];
  snprintf(modeButton, sizeof(modeButton), "[ %s ]", modeText);
  const int rebootLen = (int)strlen(rebootText);
  const int modeLen = (int)strlen(modeButton);
  const int gap = 2;
  const int buttonsLen = rebootLen + gap + modeLen;
  int titleWidth = inner - buttonsLen;
  if (titleWidth < 1) titleWidth = 1;

  out.printf("%-*.*s", titleWidth, titleWidth, title);
  if (telnetMenuFocus == 1) vtSelected(out);
  out.print(rebootText);
  if (telnetMenuFocus == 1) { vtReset(out); vtTitle(out); }
  out.print("  ");
  if (telnetMenuFocus == 2) vtSelected(out);
  out.print(modeButton);
  if (telnetMenuFocus == 2) { vtReset(out); vtTitle(out); }
  out.print(gV());
}

static void drawHeader(Print &out) {
  uint16_t w=uiWidth(); vtResetScroll(out); vtTitle(out); drawBorder(out,1,gTL(),gH(),gTR());
  drawTitleActionRow(out);
  char line[160];
  breadcrumb(line,sizeof(line)); vtCursor(out,3,1); out.print(gV()); out.printf(" %-*.*s",(int)(w-3),(int)(w-3),line); out.print(gV());
  drawBorder(out,4,gML(),gSH(),gMR()); vtReset(out);
  drawTopBanner(out, true);
  drawBorder(out, uiContentTop() - 1, gML(), gSH(), gMR());
  vtScrollRegion(out,uiContentTop(),uiContentBottom()); telnetLastUiClockMs=millis();
}

static void drawFooter(Print &out, const char *message=nullptr) {
  uint16_t h=uiHeight(),w=uiWidth(); vtResetScroll(out); drawBorder(out,h-2,gML(),gSH(),gMR());
  const char *hint;
  if(telnetMenuScreen==8) hint=telnetMenuPageRefreshable ? "Up/Down Scroll  r Refresh  Enter Refresh  Space/N Page  P Previous  Left/q Back" : "Up/Down Scroll  Space/N Page  P Previous  Left/q Back";
  else if(telnetMenuScreen==7) hint="Y Confirm  N/Esc/Left Cancel";
  else if(telnetMenuFocus==1) hint="Tab/Shift-Tab Focus  Enter/Space Reboot  q Back  ? Help";
  else if(telnetMenuFocus==2) hint="Tab/Shift-Tab Focus  Enter/Space Switch Mode  q Back  ? Help";
  else {
    TelnetMenuControlKind kind = menuControlKind(telnetMenuScreen, telnetMenuSelection);
    if(kind==TM_CONTROL_CHECKBOX) hint="Tab/Shift-Tab Focus  Space Toggle  Up/Down Navigate  Left/q Back  ? Help";
    else if(kind==TM_CONTROL_NUMBER) hint="Tab/Shift-Tab Focus  Left/Right Adjust  Enter Next  q Back  ? Help";
    else hint="Tab/Shift-Tab Focus  Up/Down Navigate  Right/Enter Select  Left/q Back  ? Help";
  }
  vtCursor(out,h-1,1); out.print(gV()); vtDim(out); out.printf("%-*.*s",(int)(w-2),(int)(w-2),hint); vtReset(out); out.print(gV());
  vtCursor(out,h,1); out.print(gBL()); vtStatus(out); char msg[128]; if(telnetMenuNumericLength)snprintf(msg,sizeof(msg),"Jump to item: %s",telnetMenuNumericPrefix);else strlcpy(msg,message?message:"",sizeof(msg)); out.printf("%-*.*s",(int)(w-2),(int)(w-2),msg); vtReset(out); out.print(gBR()); vtScrollRegion(out,uiContentTop(),uiContentBottom());
}
static void drawRow(Print &out,uint8_t index,bool selected,bool clearLine=true) {
  uint16_t row=uiContentTop()+index; if(row>uiContentBottom()) return; vtCursor(out,row,1); if(clearLine)vtClearLine(out); out.print(gSV()); if(selected)vtSelected(out);
  char value[40]="";
  if(telnetMenuScreen==3){
    if(index==0)snprintf(value,sizeof(value),"%lu ms",pollIntervalMs);
    else if(index==1)snprintf(value,sizeof(value),"%lu ms",idlePollIntervalMs);
    else if(index==2){
      if(telnetMenuAutoRefreshMs==0) snprintf(value,sizeof(value),"Off");
      else snprintf(value,sizeof(value),"%lus",telnetMenuAutoRefreshMs/1000UL);
    }
    else if(index==3)snprintf(value,sizeof(value),"[%c]",telnetLiveLogEnabled?'X':' ');
  }
  if(telnetMenuScreen==4){
    if(index<=4)snprintf(value,sizeof(value),"%s",LOG_LEVEL==index+1?"[X]":"[ ]");
    else if(index==5)snprintf(value,sizeof(value),"[%c]",telnetLiveLogEnabled?'X':' ');
  }
  uint16_t usable=uiWidth()-2; char text[160]; snprintf(text,sizeof(text)," %2u. %-42s %s",index+1,menuLabel(telnetMenuScreen,index),value); out.printf("%-*.*s",(int)usable,(int)usable,text); if(selected)vtReset(out); out.print(gSV());
}
static void clearContent(Print &out){for(uint16_t r=uiContentTop();r<=uiContentBottom();r++){vtCursor(out,r,1);vtClearLine(out);out.print(gSV());out.printf("%-*s",(int)(uiWidth()-2),"");out.print(gSV());}}
static void drawMenu(Print &out, bool clearPage=false){
  vtHideCursor(out);
  if(clearPage) vtClear(out); // One CSI 2 J + cursor-home for full screen transitions.
  drawHeader(out);
  if(!clearPage) clearContent(out); // Row clearing is retained only for same-screen redraws.
  for(uint8_t i=0;i<menuCount(telnetMenuScreen);i++) drawRow(out,i,telnetMenuFocus==0 && i==telnetMenuSelection,!clearPage);
  drawFooter(out);
}
static void updateSelection(Print &out,uint8_t oldSel,uint8_t newSel){drawRow(out,oldSel,false);drawRow(out,newSel,telnetMenuFocus==0);drawFooter(out);}

static bool pageLineIsBlank(size_t begin,size_t end){
  while(begin<end){char ch=telnetMenuPageBuffer[begin++];if(ch=='\r'||ch==' '||ch=='\t')continue;if(ch=='\n')break;return false;}return true;
}
// Choose a page boundary at a blank line whenever possible, so related status
// and information sections remain together rather than being split mid-block.
static size_t pageEnd(size_t start,uint16_t lines){
  size_t p=start,lastSectionBreak=0,lineStart=start;uint16_t n=0,lastBreakLine=0;
  while(p<telnetMenuPageLength&&n<lines){
    if(telnetMenuPageBuffer[p++]=='\n'){
      n++;
      if(pageLineIsBlank(lineStart,p)){lastSectionBreak=p;lastBreakLine=n;}
      lineStart=p;
    }
  }
  uint16_t minBreakLines=(lines/2)>3?(lines/2):3;
  if(p<telnetMenuPageLength&&lastSectionBreak>start&&lastBreakLine>=minBreakLines)return lastSectionBreak;
  // Include a final unterminated line when it fits.
  if(p>=telnetMenuPageLength)return telnetMenuPageLength;
  return p;
}
static size_t previousPage(size_t start,uint16_t lines){
  if(!start)return 0;
  size_t page=0,previous=0;
  while(page<start){
    previous=page;
    size_t next=pageEnd(page,lines);
    if(next<=page||next>=start)break;
    page=next;
  }
  return previous;
}
static size_t outputWindowEndIn(const char *buffer,size_t length,size_t start,uint16_t lines){
  size_t p=start;
  uint16_t n=0;
  while(p<length&&n<lines){
    if(buffer[p++]=='\n')n++;
  }
  if(p>=length)return length;
  return p;
}
static size_t outputWindowEnd(size_t start,uint16_t lines){
  return outputWindowEndIn(telnetMenuPageBuffer,telnetMenuPageLength,start,lines);
}
static size_t nextLineStart(size_t start){
  size_t p=start;
  while(p<telnetMenuPageLength&&telnetMenuPageBuffer[p]!='\n')p++;
  if(p<telnetMenuPageLength&&telnetMenuPageBuffer[p]=='\n')p++;
  return p;
}
static size_t previousLineStart(size_t start){
  if(!start)return 0;
  size_t prev=0,p=0;
  while(p<start){
    prev=p;
    while(p<start&&telnetMenuPageBuffer[p]!='\n')p++;
    if(p<start&&telnetMenuPageBuffer[p]=='\n')p++;
  }
  return prev;
}
static uint16_t outputLineNumber(size_t start){
  uint16_t line=1;
  for(size_t p=0;p<start&&p<telnetMenuPageLength;p++)if(telnetMenuPageBuffer[p]=='\n')line++;
  return line;
}
static void drawOutput(Print &out, bool clearPage=false){vtHideCursor(out);if(clearPage)vtClear(out);drawHeader(out);if(!clearPage)clearContent(out);uint16_t lines=uiContentRows();size_t end=outputWindowEnd(telnetMenuPageStart,lines);uint16_t row=uiContentTop();for(size_t i=telnetMenuPageStart;i<end&&row<=uiContentBottom();){vtCursor(out,row++,1);if(!clearPage)vtClearLine(out);out.print(gSV());char buf[192];size_t j=0;while(i<end&&telnetMenuPageBuffer[i]!='\n'&&j<sizeof(buf)-1)buf[j++]=telnetMenuPageBuffer[i++];while(i<end&&telnetMenuPageBuffer[i]!='\n')i++;if(i<end&&telnetMenuPageBuffer[i]=='\n')i++;buf[j]=0;out.printf("%-*.*s",(int)(uiWidth()-2),(int)(uiWidth()-2),buf);out.print(gSV());}char foot[120];bool more=end<telnetMenuPageLength;snprintf(foot,sizeof(foot),"Line %u  %s%s",(unsigned)outputLineNumber(telnetMenuPageStart),telnetMenuPageStart?gUp():"",more?gDn():"");drawFooter(out,foot);}
static void filterCapturedStatusSections(const char *const *wanted, uint8_t wantedCount) {
  char filtered[TELNET_MENU_PAGE_BUFFER_SIZE];
  size_t outLen = 0;
  bool copying = false;
  size_t pos = 0;
  while (pos < telnetMenuPageLength) {
    size_t lineStart = pos;
    while (pos < telnetMenuPageLength && telnetMenuPageBuffer[pos] != '\n') pos++;
    size_t lineEnd = pos;
    if (pos < telnetMenuPageLength && telnetMenuPageBuffer[pos] == '\n') pos++;

    char line[96];
    size_t n = lineEnd - lineStart;
    if (n >= sizeof(line)) n = sizeof(line) - 1;
    memcpy(line, telnetMenuPageBuffer + lineStart, n);
    line[n] = 0;

    if (strncmp(line, "=== ", 4) == 0) {
      copying = false;
      for (uint8_t i = 0; i < wantedCount; i++) {
        if (strcmp(line, wanted[i]) == 0) { copying = true; break; }
      }
    }
    if (copying) {
      size_t bytes = pos - lineStart;
      if (outLen + bytes >= sizeof(filtered) - 1) break;
      memcpy(filtered + outLen, telnetMenuPageBuffer + lineStart, bytes);
      outLen += bytes;
    }
  }
  filtered[outLen] = 0;
  memcpy(telnetMenuPageBuffer, filtered, outLen + 1);
  telnetMenuPageLength = outLen;
}

static void captureProtocolPortStatus(Print &out) {
  out.println("=== PROTOCOLS ===");
  out.printf("Alpaca server: %s\n", alpacaServer ? "active" : "inactive");
  out.printf("Stellarium: %s  RX packets: %lu  TX packets: %lu  Bad packets: %lu\n",
             stellariumClientConnected ? "connected" : "waiting",
             (unsigned long)stellariumRxPackets,
             (unsigned long)stellariumTxPackets,
             (unsigned long)stellariumBadPackets);
  out.printf("SkySafari WiFi: %s  RX commands: %lu\n",
             lx200WifiClientConnected ? "connected" : "waiting",
             (unsigned long)lx200WifiRxCommands);
  out.printf("SkySafari Bluetooth: %s  RX commands: %lu\n",
             bluetoothLX200ClientConnected() ? "connected" : "waiting",
             (unsigned long)lx200BtRxCommands);
  out.println();
  out.println("=== PORTS ===");
  out.printf("Web UI HTTP: %u\n", HTTP_WEB_PORT);
  out.printf("Alpaca HTTP: %u\n", ALPACA_PORT);
  out.printf("Alpaca discovery UDP: %u\n", ALPACA_DISCOVERY_PORT);
  out.printf("Stellarium TCP: %u\n", STELLARIUM_PORT);
  out.printf("SkySafari LX200 TCP: %u\n", LX200_PORT);
  out.printf("Telnet console: %u\n", TELNET_PORT);
}

static void captureMenuCommand(const char *cmd){
  const bool savedMonitor=telnetMonitorActive, savedTasks=telnetTasksActive;
  const bool savedLive=telnetLiveLogEnabled;
  TelnetMenuBufferPrint capture;
  if (strcmp(cmd,"tasks")==0) {
    telnetDrawTasks(capture);
  } else if (strcmp(cmd,"menu_observer_basic")==0) {
    capture.print(basicStatusText());
  } else if (strcmp(cmd,"menu_observer_advanced")==0) {
    telnetExecuteCommand(String("current_state"),capture);
  } else if (strcmp(cmd,"menu_system_basic")==0) {
    capture.print(basicSystemHealthText());
  } else if (strcmp(cmd,"menu_time_location")==0) {
    telnetExecuteCommand(String("current_state"),capture);
    const char *wanted[] = {"=== TIME AND SITE ==="};
    filterCapturedStatusSections(wanted,1);
  } else if (strcmp(cmd,"menu_network")==0) {
    capture.print(systemHealthText());
    const char *wanted[] = {"=== NETWORK ===", "=== CLOCK AND PERSISTENCE ==="};
    filterCapturedStatusSections(wanted,2);
  } else if (strcmp(cmd,"menu_protocol_ports")==0) {
    captureProtocolPortStatus(capture);
  } else {
    telnetExecuteCommand(String(cmd),capture);
  }
  // Menu-owned command capture must never leak into the console's asynchronous
  // monitor/tasks modes, which would overwrite the menu on the next service tick.
  telnetMonitorActive=savedMonitor;
  telnetTasksActive=savedTasks;
  telnetLiveLogEnabled=savedLive;
  if(capture.truncated){const char *t="\n[Output truncated at 4096 bytes]\n";for(size_t i=0;t[i];i++)capture.write((uint8_t)t[i]);}
}
static bool isRefreshableStatusCommand(const char *cmd){
  if(!cmd) return false;
  return strcmp(cmd,"status")==0 || strcmp(cmd,"current_state")==0 ||
         strcmp(cmd,"system_health")==0 || strcmp(cmd,"wifi status")==0 ||
         strcmp(cmd,"tasks")==0 || strcmp(cmd,"menu_observer_basic")==0 ||
         strcmp(cmd,"menu_observer_advanced")==0 || strcmp(cmd,"menu_system_basic")==0 ||
         strcmp(cmd,"menu_time_location")==0 || strcmp(cmd,"menu_network")==0 ||
         strcmp(cmd,"menu_protocol_ports")==0;
}

static bool isAutoRefreshEligibleCommand(const char *cmd){
  // Runtime task statistics are intentionally manual-only because collecting
  // and formatting them is comparatively expensive on a low-heap BT build.
  return cmd && strcmp(cmd,"tasks")!=0;
}
static void showCommand(Print &out,const char *title,const char *cmd){
  const uint8_t sourceScreen=telnetMenuScreen;
  strlcpy(telnetMenuPageTitle,title,sizeof(telnetMenuPageTitle));
  strlcpy(telnetMenuPageCommand,cmd?cmd:"",sizeof(telnetMenuPageCommand));
  telnetMenuPageRefreshable=isRefreshableStatusCommand(telnetMenuPageCommand);
  captureMenuCommand(telnetMenuPageCommand);
  telnetMenuPageStart=0;
  telnetMenuPageNumber=0;
  telnetMenuReturnScreen=sourceScreen;
  telnetMenuReturnSelection=telnetMenuSelection;
  telnetMenuScreen=8;
  telnetMenuLastPageRefreshMs=millis();
  drawOutput(out,true);
}
static size_t pageStartForNumber(uint16_t pageNumber,uint16_t lines){
  size_t start=0;
  for(uint16_t page=0;page<pageNumber && start<telnetMenuPageLength;page++){
    size_t next=pageEnd(start,lines);
    if(next<=start)break;
    start=next;
  }
  return start;
}

static bool pageLineIsBlankIn(const char *buffer,size_t begin,size_t end){
  while(begin<end){
    const char ch=buffer[begin++];
    if(ch=='\r'||ch==' '||ch=='\t')continue;
    if(ch=='\n')break;
    return false;
  }
  return true;
}

static size_t pageEndIn(const char *buffer,size_t length,size_t start,uint16_t lines){
  size_t p=start,lastSectionBreak=0,lineStart=start;
  uint16_t n=0,lastBreakLine=0;
  while(p<length&&n<lines){
    if(buffer[p++]=='\n'){
      n++;
      if(pageLineIsBlankIn(buffer,lineStart,p)){lastSectionBreak=p;lastBreakLine=n;}
      lineStart=p;
    }
  }
  const uint16_t minBreakLines=(lines/2)>3?(lines/2):3;
  if(p<length&&lastSectionBreak>start&&lastBreakLine>=minBreakLines)return lastSectionBreak;
  if(p>=length)return length;
  return p;
}

static void extractVisiblePageLine(const char *buffer,size_t pageStart,size_t pageLimit,
                                   uint16_t visibleIndex,char *dst,size_t dstSize){
  if(!dstSize)return;
  dst[0]=0;
  size_t p=pageStart;
  for(uint16_t line=0;line<visibleIndex && p<pageLimit;line++){
    while(p<pageLimit && buffer[p]!='\n')p++;
    if(p<pageLimit && buffer[p]=='\n')p++;
  }
  size_t n=0;
  while(p<pageLimit && buffer[p]!='\n' && n+1<dstSize)dst[n++]=buffer[p++];

  // Canonicalize captured rows before diffing. Console commands may emit CR,
  // tabs, or padding spaces on otherwise blank lines. Treat those rows as the
  // same visual content so refresh does not waste Telnet traffic repainting
  // blank cells. Trailing whitespace is never visible in the framed data area.
  while(n && (dst[n-1]=='\r' || dst[n-1]==' ' || dst[n-1]=='\t'))n--;
  dst[n]=0;
}

static bool outputLineIsVisuallyBlank(const char *text){
  if(!text)return true;
  while(*text){
    if(*text!=' ' && *text!='\t' && *text!='\r')return false;
    text++;
  }
  return true;
}

static void drawOutputDataRow(Print &out,uint16_t visibleIndex,const char *text){
  const uint16_t row=uiContentTop()+visibleIndex;
  if(row>uiContentBottom())return;

  // One cursor-positioned, fixed-width write replaces the complete framed row.
  // Because the row width is exact, CSI 2K is unnecessary and removing it cuts
  // both bytes and terminal work from every changed status value.
  const int fieldWidth=(int)(uiWidth()-2);
  out.printf("\x1b[%u;1H%s%-*.*s%s",
             (unsigned)row,gSV(),fieldWidth,fieldWidth,text?text:"",gSV());
}

static void refreshOutputPage(Print &out){
  if(!telnetMenuPageRefreshable || !telnetMenuPageCommand[0]) return;

  // Snapshot the previous command output before captureMenuCommand() overwrites it.
  const size_t oldLength=telnetMenuPageLength;
  const size_t oldStart=telnetMenuPageStart;
  const uint16_t oldPageNumber=telnetMenuPageNumber;
  memcpy(telnetMenuPreviousPageBuffer,telnetMenuPageBuffer,oldLength+1);

  captureMenuCommand(telnetMenuPageCommand);

  // Keep the user at the same visible line offset after refresh whenever the
  // refreshed output is long enough.
  telnetMenuPageNumber=oldPageNumber;
  telnetMenuPageStart=oldStart<telnetMenuPageLength?oldStart:pageStartForNumber(telnetMenuPageNumber,uiContentRows());
  if(telnetMenuPageStart>=telnetMenuPageLength){
    telnetMenuPageNumber=0;
    telnetMenuPageStart=0;
  }

  // Compare visible rows and repaint only cells whose text changed. The header,
  // frame, unchanged status rows, and footer remain untouched, eliminating the
  // full-page flash seen in Windows telnet.exe.
  const size_t oldEnd=outputWindowEndIn(telnetMenuPreviousPageBuffer,oldLength,oldStart,uiContentRows());
  const size_t newEnd=outputWindowEnd(telnetMenuPageStart,uiContentRows());
  char oldLine[192],newLine[192];
  for(uint16_t row=0;row<uiContentRows();row++){
    extractVisiblePageLine(telnetMenuPreviousPageBuffer,oldStart,oldEnd,row,oldLine,sizeof(oldLine));
    extractVisiblePageLine(telnetMenuPageBuffer,telnetMenuPageStart,newEnd,row,newLine,sizeof(newLine));

    // Do nothing when both rows are visually blank, even if the source used
    // different combinations of CR, tabs, or padding spaces. Only clear a row
    // when old visible text actually disappeared.
    const bool oldBlank=outputLineIsVisuallyBlank(oldLine);
    const bool newBlank=outputLineIsVisuallyBlank(newLine);
    if(oldBlank && newBlank)continue;
    if(strcmp(oldLine,newLine)!=0)drawOutputDataRow(out,row,newLine);
  }

  // Leave the footer untouched during ordinary refreshes. Redraw it only if
  // the refreshed output changed whether previous/next pages are available.
  const bool oldPrevious=oldStart!=0;
  const bool newPrevious=telnetMenuPageStart!=0;
  const bool oldMore=oldEnd<oldLength;
  const bool newMore=newEnd<telnetMenuPageLength;
  if(oldPrevious!=newPrevious || oldMore!=newMore){
    char foot[120];
    snprintf(foot,sizeof(foot),"Line %u  %s%s",
             (unsigned)outputLineNumber(telnetMenuPageStart),
             newPrevious?gUp():"",
             newMore?gDn():"");
    drawFooter(out,foot);
  }
}
static void drawConfirmOverlay(Print &out){
  // Floating Y/N dialog: draw only the centered box and leave the menu visible beneath it.
  const char *title=telnetMenuPageTitle;
  const char *message=telnetMenuPageBuffer;
  vtHideCursor(out);
  const uint16_t maxBox=(uiWidth()>8)?(uiWidth()-8):32;
  const uint16_t boxW=maxBox<58?maxBox:58;
  const uint16_t boxH=7;
  uint16_t r=uiContentTop()+((uiContentRows()>boxH)?((uiContentRows()-boxH)/2):0);
  uint16_t c=(uiWidth()-boxW)/2+1;
  if(r<uiContentTop())r=uiContentTop();

  vtReset(out);
  vtCursor(out,r,c);out.print(telnetMenuUtf8?"┌":"+");for(uint16_t i=0;i<boxW-2;i++)out.print(gSH());out.print(telnetMenuUtf8?"┐":"+");
  vtCursor(out,r+1,c);out.print(gSV());vtTitle(out);out.printf(" %-*.*s",(int)(boxW-3),(int)(boxW-3),title);vtReset(out);out.print(gSV());
  vtCursor(out,r+2,c);out.print(gSV());out.printf(" %-*s",(int)(boxW-3),"");out.print(gSV());
  vtCursor(out,r+3,c);out.print(gSV());out.printf(" %-*.*s",(int)(boxW-3),(int)(boxW-3),message);out.print(gSV());
  vtCursor(out,r+4,c);out.print(gSV());out.printf(" %-*s",(int)(boxW-3),"");out.print(gSV());
  vtCursor(out,r+5,c);out.print(gSV());vtSelected(out);out.printf(" %-*.*s",(int)(boxW-3),(int)(boxW-3)," Y = Yes     N / Esc / Left = No ");vtReset(out);out.print(gSV());
  vtCursor(out,r+6,c);out.print(telnetMenuUtf8?"└":"+");for(uint16_t i=0;i<boxW-2;i++)out.print(gSH());out.print(telnetMenuUtf8?"┘":"+");
}
static void redrawConfirmWithBackground(Print &out){
  // Used after NAWS resize: rebuild the saved menu, then place the modal over it.
  uint8_t activeScreen=telnetMenuScreen, activeSelection=telnetMenuSelection;
  telnetMenuScreen=telnetMenuReturnScreen;
  telnetMenuSelection=telnetMenuReturnSelection;
  drawMenu(out);
  telnetMenuScreen=activeScreen;
  telnetMenuSelection=activeSelection;
  drawConfirmOverlay(out);
}
static void showConfirm(Print &out,const char *title,const char *message,const char *cmd){strlcpy(telnetMenuPageTitle,title,sizeof(telnetMenuPageTitle));strlcpy(telnetMenuPageBuffer,message,TELNET_MENU_PAGE_BUFFER_SIZE);telnetMenuPageLength=strlen(telnetMenuPageBuffer);telnetMenuInfoCommand=cmd;telnetMenuReturnScreen=telnetMenuScreen;telnetMenuReturnSelection=telnetMenuSelection;telnetMenuScreen=7;drawConfirmOverlay(out);}
static void redrawCurrent(Print &out){if(telnetMenuScreen==8)drawOutput(out);else if(telnetMenuScreen==7)redrawConfirmWithBackground(out);else drawMenu(out);}
static void backMenu(Print &out){telnetMenuFocus=0;telnetMenuNumericLength=0;telnetMenuNumericPrefix[0]=0;if(telnetMenuScreen==0){telnetStopMenu(out);if(telnetClient&&telnetClient.connected())out.print("> ");return;}if(telnetMenuScreen==8||telnetMenuScreen==7){telnetMenuScreen=telnetMenuReturnScreen;telnetMenuSelection=telnetMenuReturnSelection;}else{telnetMenuScreen=parentScreen(telnetMenuScreen);telnetMenuSelection=0;}drawMenu(out,true);}
static void toggleFocusedCheckbox(Print &out) {
  if(!menuItemIsCheckbox(telnetMenuScreen,telnetMenuSelection)) return;
  if(telnetMenuScreen==3 && telnetMenuSelection==3) {
    telnetLiveLogEnabled=!telnetLiveLogEnabled;
    telnetMenuSavedLiveLog=telnetLiveLogEnabled;
    drawRow(out,telnetMenuSelection,true);
    drawFooter(out,"Telnet live logging changed");
  } else if(telnetMenuScreen==4 && telnetMenuSelection==5) {
    telnetLiveLogEnabled=!telnetLiveLogEnabled;
    telnetMenuSavedLiveLog=telnetLiveLogEnabled;
    drawRow(out,telnetMenuSelection,true);
    drawFooter(out,"Telnet live logging changed");
  }
}

static uint32_t previousInterval(uint32_t value, bool idle) {
  (void)idle;
  const uint32_t step = 1000UL;
  const uint32_t maxValue = 60000UL;
  if(value == 0) return maxValue;
  value = (value / step) * step;
  return value > step ? value - step : 0;
}
static uint32_t nextInterval(uint32_t value, bool idle) {
  (void)idle;
  const uint32_t step = 1000UL;
  const uint32_t maxValue = 60000UL;
  value = ((value + step - 1) / step) * step;
  return value >= maxValue ? 0 : value + step;
}

static unsigned long previousMenuRefreshInterval(unsigned long value) {
  const unsigned long values[] = {0UL, 5000UL, 10000UL, 30000UL};
  for(uint8_t i=0;i<4;i++) if(values[i]==value) return values[i?i-1:3];
  return 0UL;
}
static unsigned long nextMenuRefreshInterval(unsigned long value) {
  const unsigned long values[] = {0UL, 5000UL, 10000UL, 30000UL};
  for(uint8_t i=0;i<4;i++) if(values[i]==value) return values[(i+1)%4];
  return 0UL;
}

static bool adjustFocusedNumber(Print &out,int direction) {
  if(!menuItemIsNumber(telnetMenuScreen,telnetMenuSelection)) return false;
  if(telnetMenuScreen==3 && telnetMenuSelection==0) {
    pollIntervalMs = direction < 0 ? previousInterval(pollIntervalMs,false) : nextInterval(pollIntervalMs,false);
    savePersistentSettings();
    drawRow(out,0,true);
    drawFooter(out,"Mount poll interval changed");
    return true;
  }
  if(telnetMenuScreen==3 && telnetMenuSelection==1) {
    idlePollIntervalMs = direction < 0 ? previousInterval(idlePollIntervalMs,true) : nextInterval(idlePollIntervalMs,true);
    savePersistentSettings();
    drawRow(out,1,true);
    drawFooter(out,"Idle poll interval changed");
    return true;
  }
  if(telnetMenuScreen==3 && telnetMenuSelection==2) {
    telnetMenuAutoRefreshMs = direction < 0 ? previousMenuRefreshInterval(telnetMenuAutoRefreshMs) : nextMenuRefreshInterval(telnetMenuAutoRefreshMs);
    telnetMenuLastPageRefreshMs = millis();
    drawRow(out,2,true);
    drawFooter(out,"Status auto-refresh interval changed");
    return true;
  }
  return false;
}

static void selectMenu(Print &out){telnetMenuFocus=0;
  if(telnetMenuScreen==0){if(telnetMenuSelection<5){telnetMenuScreen=telnetMenuSelection+1;telnetMenuSelection=0;drawMenu(out,true);}else{telnetStopMenu(out);if(telnetClient&&telnetClient.connected())out.print("> ");}return;}
  if(telnetMenuScreen==1){const char* cmds[]={"get","getaltaz",nullptr,"testinit","drain"};if(telnetMenuSelection==2){telnetMenuScreen=6;telnetMenuSelection=0;drawMenu(out,true);}else if(telnetMenuSelection==5)backMenu(out);else showCommand(out,menuLabel(1,telnetMenuSelection),cmds[telnetMenuSelection]);return;}
  if(telnetMenuScreen==2){
    if(telnetMenuSelection==8){backMenu(out);return;}
    const char *cmd=nullptr;
    switch(telnetMenuSelection){
      case 0: cmd="menu_observer_basic"; break;
      case 1: cmd="menu_system_basic"; break;
      case 2: cmd="menu_observer_advanced"; break;
      case 3: cmd="system_health"; break;
      case 4: cmd="menu_network"; break;
      case 5: cmd="menu_time_location"; break;
      case 6: cmd="menu_protocol_ports"; break;
      case 7: cmd="tasks"; break;
    }
    if(cmd) showCommand(out,menuLabel(2,telnetMenuSelection),cmd);
    return;
  }
  if(telnetMenuScreen==3){if(telnetMenuSelection<=2)adjustFocusedNumber(out,+1);else if(telnetMenuSelection==3)toggleFocusedCheckbox(out);else if(telnetMenuSelection==4){if(bridgeMode==BRIDGE_MODE_WIFI_FULL)showConfirm(out,"Bluetooth mode","Save BT mode (Telnet only; no web interface) and reboot?","modebt");else showConfirm(out,"Full WiFi mode","Save Full WiFi mode and reboot?","modewifi");}else backMenu(out);return;}
  if(telnetMenuScreen==4){if(telnetMenuSelection<=4){uint8_t oldLevel=LOG_LEVEL;LOG_LEVEL=telnetMenuSelection+1;if(oldLevel>=1&&oldLevel<=5)drawRow(out,oldLevel-1,oldLevel-1==telnetMenuSelection);drawRow(out,telnetMenuSelection,true);drawFooter(out,"Log level changed");}else if(telnetMenuSelection==5)toggleFocusedCheckbox(out);else backMenu(out);return;}
  if(telnetMenuScreen==5){const char* cmds[]={"gpio_startup","telnet status",nullptr,"apdefault"};if(telnetMenuSelection==2)showConfirm(out,"Restart controller",bridgeMode==BRIDGE_MODE_BT_MIN_WEB?"Restart now? BT mode will return with Telnet only; no web interface.":"Restart now?","reboot");else if(telnetMenuSelection==3)showConfirm(out,"Restore AP defaults","Restore AP defaults and reboot?","apdefault");else if(telnetMenuSelection==4)backMenu(out);else showCommand(out,menuLabel(5,telnetMenuSelection),cmds[telnetMenuSelection]);return;}
  if(telnetMenuScreen==6){const char* cmds[]={"nudge az+","nudge az-","nudge alt+","nudge alt-"};if(telnetMenuSelection==4)backMenu(out);else showCommand(out,"Manual Nudge",cmds[telnetMenuSelection]);}
}

void telnetStartMenu(Print &out){memset(telnetBannerCache,0,sizeof(telnetBannerCache));telnetMenuLastPageRefreshMs=millis();telnetMenuWindowsCompat=true;telnetMenuUtf8=false;telnetMenuSavedLiveLog=telnetLiveLogEnabled;telnetLiveLogEnabled=false;telnetMonitorActive=false;telnetTasksActive=false;telnetMenuActive=true;telnetMenuScreen=0;telnetMenuSelection=0;telnetMenuFocus=0;telnetMenuEscapeState=0;telnetMenuEscapeParam=0;telnetMenuEscapeHasParam=false;telnetMenuNumericLength=0;telnetMenuNumericPrefix[0]=0;telnetMenuIgnoreNextLf=telnetLastWasCR;vtAltOn(out);vtClear(out);drawMenu(out);}
void telnetStopMenu(Print &out){telnetMenuActive=false;telnetLiveLogEnabled=telnetMenuSavedLiveLog;telnetMenuEscapeState=0;telnetMenuIgnoreNextLf=false;telnetMenuNumericLength=0;telnetMenuNumericPrefix[0]=0;vtResetScroll(out);vtShowCursor(out);vtReset(out);vtAltOff(out);if(telnetMenuWindowsCompat)vtClear(out);out.println("Menu closed. Type menu to reopen it.");}

static void setMenuFocus(Print &out, uint8_t newFocus) {
  newFocus %= 3;
  uint8_t oldFocus = telnetMenuFocus;
  if (oldFocus == newFocus) return;
  telnetMenuFocus = newFocus;
  // Redraw only the previously/currently focused menu row and header line.
  if (oldFocus == 0 || newFocus == 0) drawRow(out, telnetMenuSelection, newFocus == 0);
  vtTitle(out);
  drawTitleActionRow(out);
  vtReset(out);
  drawFooter(out);
}

static void cycleMenuFocus(Print &out, int direction) {
  int next = (int)telnetMenuFocus + direction;
  while (next < 0) next += 3;
  while (next >= 3) next -= 3;
  setMenuFocus(out, (uint8_t)next);
}

static bool activateHeaderFocus(Print &out) {
  if (telnetMenuFocus == 1) {
    showConfirm(out, "Restart controller",
                bridgeMode == BRIDGE_MODE_BT_MIN_WEB
                  ? "Restart now? BT mode will return with Telnet only; no web interface."
                  : "Restart now?",
                "reboot");
    return true;
  }
  if (telnetMenuFocus == 2) {
    if (bridgeMode == BRIDGE_MODE_WIFI_FULL)
      showConfirm(out, "Bluetooth mode", "Save BT mode (Telnet only; no web interface) and reboot?", "modebt");
    else
      showConfirm(out, "Full WiFi mode", "Save Full WiFi mode and reboot?", "modewifi");
    return true;
  }
  return false;
}

static void moveSelection(Print &out,int delta){if(telnetMenuFocus!=0)setMenuFocus(out,0);uint8_t count=menuCount(telnetMenuScreen),old=telnetMenuSelection;if(!count)return;int n=(int)old+delta;while(n<0)n+=count;while(n>=count)n-=count;telnetMenuSelection=(uint8_t)n;updateSelection(out,old,telnetMenuSelection);drawFooter(out);}
static void handleCsiFinal(uint8_t ch,Print &out){uint8_t count=menuCount(telnetMenuScreen);if(telnetMenuScreen==8){if(ch=='A'&&telnetMenuPageStart){telnetMenuPageStart=previousLineStart(telnetMenuPageStart);drawOutput(out,true);}else if(ch=='B'){size_t n=nextLineStart(telnetMenuPageStart);if(n<telnetMenuPageLength){telnetMenuPageStart=n;drawOutput(out,true);}}else if(ch=='D')backMenu(out);else if(ch=='C'||(ch=='~'&&telnetMenuEscapeParam==6)){size_t n=pageEnd(telnetMenuPageStart,uiContentRows());if(n<telnetMenuPageLength){telnetMenuPageStart=n;telnetMenuPageNumber++;drawOutput(out,true);}}else if(ch=='~'&&telnetMenuEscapeParam==5&&telnetMenuPageStart){telnetMenuPageStart=previousPage(telnetMenuPageStart,uiContentRows());if(telnetMenuPageNumber)telnetMenuPageNumber--;drawOutput(out,true);}return;}uint8_t old=telnetMenuSelection;if(ch=='A')moveSelection(out,-1);else if(ch=='B')moveSelection(out,1);else if(ch=='Z')cycleMenuFocus(out,-1);else if(ch=='C'){if(!activateHeaderFocus(out)){if(!adjustFocusedNumber(out,+1))selectMenu(out);}}else if(ch=='D'){if(telnetMenuFocus!=0)setMenuFocus(out,0);else if(!adjustFocusedNumber(out,-1))backMenu(out);}else if(ch=='H'||(ch=='~'&&telnetMenuEscapeParam==1)|| (ch=='~'&&telnetMenuEscapeParam==7)){telnetMenuSelection=0;updateSelection(out,old,0);drawFooter(out);}else if(ch=='F'||(ch=='~'&&(telnetMenuEscapeParam==4||telnetMenuEscapeParam==8))){telnetMenuSelection=count?count-1:0;updateSelection(out,old,telnetMenuSelection);drawFooter(out);}else if(ch=='~'&&telnetMenuEscapeParam==5){int n=(int)old-(int)uiContentRows();if(n<0)n=0;telnetMenuSelection=(uint8_t)n;updateSelection(out,old,telnetMenuSelection);drawFooter(out);}else if(ch=='~'&&telnetMenuEscapeParam==6){int n=(int)old+(int)uiContentRows();if(n>=count)n=count-1;telnetMenuSelection=(uint8_t)n;updateSelection(out,old,telnetMenuSelection);drawFooter(out);}}
static bool activateNumericSelection(Print &out){if(!telnetMenuNumericLength)return false;uint16_t n=(uint16_t)atoi(telnetMenuNumericPrefix);telnetMenuNumericLength=0;telnetMenuNumericPrefix[0]=0;uint8_t count=menuCount(telnetMenuScreen);if(n<1||n>count){drawFooter(out,"Invalid menu item number");return true;}uint8_t old=telnetMenuSelection;telnetMenuSelection=(uint8_t)(n-1);updateSelection(out,old,telnetMenuSelection);selectMenu(out);return true;}
static void handleMenuByte(uint8_t ch,Print &out){
  if(telnetMenuIgnoreNextLf){telnetMenuIgnoreNextLf=false;if(ch=='\n')return;}
  if(telnetMenuScreen==7){if(ch=='y'||ch=='Y'){const char* cmd=telnetMenuInfoCommand;telnetStopMenu(out);telnetExecuteCommand(String(cmd),out);telnetExecuteCommand(String("y"),out);}else if(ch=='n'||ch=='N'||ch==27||ch=='q'||ch=='Q')backMenu(out);return;}
  if(telnetMenuEscapeState==1){if(ch=='['){telnetMenuEscapeState=2;telnetMenuEscapeParam=0;telnetMenuEscapeHasParam=false;return;}if(ch=='O'){telnetMenuEscapeState=3;return;}telnetMenuEscapeState=0;backMenu(out);return;}
  if(telnetMenuEscapeState==2){if(ch>='0'&&ch<='9'){telnetMenuEscapeHasParam=true;uint16_t nextParam=(uint16_t)telnetMenuEscapeParam*10U+(uint16_t)(ch-'0');telnetMenuEscapeParam=(uint8_t)(nextParam>99U?99U:nextParam);return;}if(ch==';')return;telnetMenuEscapeState=0;handleCsiFinal(ch,out);return;}
  if(telnetMenuEscapeState==3){telnetMenuEscapeState=0;handleCsiFinal(ch,out);return;}
  if(telnetMenuScreen==8){
    if(ch==27){telnetMenuEscapeState=1;return;}
    if((ch=='r'||ch=='R'||ch=='\r'||ch=='\n') && telnetMenuPageRefreshable){
      telnetMenuLastPageRefreshMs=millis();
      drawTopBanner(out,true);
      refreshOutputPage(out);
    } else if(ch==' '||ch=='n'||ch=='N'){
      size_t n=pageEnd(telnetMenuPageStart,uiContentRows());
      if(n<telnetMenuPageLength){telnetMenuPageStart=n;telnetMenuPageNumber++;drawOutput(out,true);}
    } else if(ch=='p'||ch=='P'){
      if(telnetMenuPageStart){telnetMenuPageStart=previousPage(telnetMenuPageStart,uiContentRows());if(telnetMenuPageNumber)telnetMenuPageNumber--;drawOutput(out,true);}
    } else if(ch=='q'||ch=='Q'||ch==3) backMenu(out);
    return;
  }
  if(ch==27){telnetMenuEscapeState=1;return;}if(ch=='q'||ch=='Q'||ch==3){backMenu(out);return;}if(ch==' '){if(activateHeaderFocus(out))return;if(menuItemIsCheckbox(telnetMenuScreen,telnetMenuSelection)){toggleFocusedCheckbox(out);return;}}if(ch=='\r'){telnetMenuIgnoreNextLf=true;if(activateHeaderFocus(out))return;if(!activateNumericSelection(out))selectMenu(out);return;}if(ch=='\n'){if(activateHeaderFocus(out))return;if(!activateNumericSelection(out))selectMenu(out);return;}if(ch=='\t'){cycleMenuFocus(out,1);return;}if(ch>='0'&&ch<='9'){if(telnetMenuNumericLength<4){telnetMenuNumericPrefix[telnetMenuNumericLength++]=(char)ch;telnetMenuNumericPrefix[telnetMenuNumericLength]=0;drawFooter(out);}return;}if(ch=='?'){showCommand(out,"Telnet Menu Help","help");return;}
}

// RFC854 parser. Returns true when byte is application data.
static bool telnetParseByte(uint8_t ch,uint8_t &app){
  enum{TIAC=255,TWILL=251,TWONT=252,TDO=253,TDONT=254,TSB=250,TSE=240,TNOP=241,TAYT=246};
  if(telnetIacState==0){if(ch==TIAC){telnetIacState=1;return false;}app=ch;return true;}
  if(telnetIacState==1){if(ch==TIAC){telnetIacState=0;app=TIAC;return true;}if(ch==TWILL||ch==TWONT||ch==TDO||ch==TDONT){telnetIacVerb=ch;telnetIacState=2;return false;}if(ch==TSB){telnetIacState=3;return false;}if(ch==TNOP){telnetIacState=0;return false;}if(ch==TAYT){const char *yes="\r\n[Yes]\r\n";telnetClient.write((const uint8_t*)yes,strlen(yes));telnetIacState=0;return false;}telnetIacState=0;return false;}
  if(telnetIacState==2){uint8_t reply=TWONT;if(telnetIacVerb==TWILL){reply=(ch==31||ch==3)?TDO:TDONT;}else if(telnetIacVerb==TWONT){reply=TDONT;}else if(telnetIacVerb==TDO){reply=(ch==1||ch==3)?TWILL:TWONT;}else if(telnetIacVerb==TDONT){reply=TWONT;}uint8_t r[]={TIAC,reply,ch};telnetClient.write(r,3);telnetIacState=0;return false;}
  if(telnetIacState==3){telnetSbOption=ch;telnetSbLength=0;telnetIacState=4;return false;}
  if(telnetIacState==4){if(ch==TIAC){telnetIacState=5;return false;}if(telnetSbLength<sizeof(telnetSbData))telnetSbData[telnetSbLength++]=ch;return false;}
  if(telnetIacState==5){if(ch==TSE){if(telnetSbOption==31&&telnetSbLength>=4){uint16_t w=(telnetSbData[0]<<8)|telnetSbData[1],h=(telnetSbData[2]<<8)|telnetSbData[3];if(w>=20&&w<=300&&h>=8&&h<=120){telnetTermWidth=w;telnetTermHeight=h;telnetNawsReceived=true;if(telnetMenuActive){TelnetCRLFPrint o(telnetClient);redrawCurrent(o);}}}telnetIacState=0;}else if(ch==TIAC){if(telnetSbLength<sizeof(telnetSbData))telnetSbData[telnetSbLength++]=TIAC;telnetIacState=4;}else{telnetSbOption=0;telnetSbLength=0;telnetIacState=0;}return false;}return false;
}


void telnetSetMenuTerminalSize(uint16_t columns, uint16_t rows) {
  if (columns >= 40 && columns <= 300) telnetTermWidth = columns;
  if (rows >= 16 && rows <= 120) telnetTermHeight = rows;
  // A manual setting is authoritative for this connection until a later NAWS update.
  if (telnetMenuActive && telnetClient && telnetClient.connected()) {
    TelnetCRLFPrint out(telnetClient);
    redrawCurrent(out);
  }
}

uint16_t telnetGetMenuTerminalColumns() { return telnetTermWidth; }
uint16_t telnetGetMenuTerminalRows() { return telnetTermHeight; }
bool telnetMenuSizeFromNaws() { return telnetNawsReceived; }

bool telnetConsoleServerRunning() {
#if defined(ESP32)
  return wifiRuntimeEnabled && telnetServerPtr != nullptr;
#else
  return false;
#endif
}

void stopTelnetConsoleServer(const char* reason) {
#if defined(ESP32)
  telnetSerialDiag("shutdown");
  if (telnetMenuActive) {
    telnetMenuActive = false;
    telnetLiveLogEnabled = telnetMenuSavedLiveLog;
  }
  if (telnetMonitorActive) {
    telnetMonitorActive = false;
    telnetLiveLogEnabled = telnetMonitorSavedLiveLog;
  }
  if (telnetTasksActive) {
    telnetTasksActive = false;
    telnetLiveLogEnabled = telnetTasksSavedLiveLog;
  }
  if (telnetClient) {
    telnetClient.flush();
    telnetClient.stop();
  }
  telnetClientConnected = false;
  telnetAuthenticated = false;
  telnetLine = "";
  telnetLastWasCR = false;
  telnetRedTextEnabled = false;
  telnetPendingConfirmCommand = "";
  telnetPendingConfirmDescription = "";

  if (telnetServerPtr) {
    telnetServerPtr->end();
    delete telnetServerPtr;
    telnetServerPtr = nullptr;
  }
  Serial.printf("[TELNET] console server stopped and unloaded (%s)\n", reason ? reason : "no reason");
#endif
}

void startTelnetConsoleServer(const char* reason) {
#if defined(ESP32)
  if (!wifiRuntimeEnabled) {
    Serial.println("[TELNET] server start deferred because WiFi runtime is off");
    return;
  }
  if (!telnetServerPtr) telnetServerPtr = new WiFiServer(TELNET_PORT);
  if (!telnetServerPtr) {
    Serial.println("[TELNET] ERROR: unable to allocate console server");
    return;
  }
  telnetServer().begin();
  Serial.printf("[TELNET] console server started on port %u (%s)\n", TELNET_PORT, reason ? reason : "no reason");
#endif
}

void serviceTelnetConsole() {
#if defined(ESP32)
  if (!wifiRuntimeEnabled) return;
  if (!telnetServerPtr) return;

  if (!telnetClient || !telnetClient.connected()) {
    if (telnetClientConnected) {
      telnetSerialDiag("disconnect");
      Serial.println("[telnet] client disconnected");
    }
    if (telnetMenuActive) {
      telnetMenuActive = false;
      telnetLiveLogEnabled = telnetMenuSavedLiveLog;
    }
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
      // Disable Nagle for immediate key-response packets. Screen output is still
      // efficiently coalesced by TelnetCRLFPrint's 512-byte buffer.
      telnetClient.setNoDelay(true);
      telnetClientConnected = true;
      telnetAuthenticated = (telnetPassword.length() == 0);
      telnetLine = "";
      telnetLastWasCR = false;
      telnetRedTextEnabled = false;
      telnetMonitorActive = false;
      telnetTasksActive = false;
      telnetMenuActive = false;
      telnetLastRxMs = millis();
      TelnetCRLFPrint telnetOut(telnetClient);
      telnetIacState = 0; telnetTermWidth = 80; telnetTermHeight = 24; telnetNawsReceived = false;
      { const uint8_t nego[] = {255,251,1,255,251,3,255,253,3,255,253,31,255,254,34}; telnetClient.write(nego,sizeof(nego)); }
      telnetApplyTextColor(telnetOut);
      telnetOut.println();
      if (telnetAuthenticated) {
        telnetOut.println("Type help.");
        telnetOut.print("> ");
      } else {
        telnetOut.print("Password: ");
      }
      Serial.printf("[telnet] client connected on port %u\n", TELNET_PORT);
      telnetSerialDiag("connect");
    }
    return;
  }

  TelnetCRLFPrint telnetOut(telnetClient);

  while (telnetClient.available()) {
    uint8_t ch = (uint8_t)telnetClient.read();

    uint8_t appByte = 0;
    if (!telnetParseByte(ch, appByte)) continue;
    ch = appByte;

    if (telnetMenuActive) {
      handleMenuByte(ch, telnetOut);
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
        telnetSerialDiag("command");
        telnetExecuteCommand(line, telnetOut);
      }

      // Full-screen modes own the terminal and must not receive a normal
      // command prompt after their start command.
      if (telnetClient && telnetClient.connected() &&
          !telnetMenuActive && !telnetMonitorActive && !telnetTasksActive) {
        telnetOut.print("> ");
      }
      continue;
    }

    if (ch == 8 || ch == 127) {
      if (telnetLine.length() > 0) {
        telnetLine.remove(telnetLine.length() - 1);
        // Server negotiated WILL ECHO, so visibly erase the previous character.
        if (telnetAuthenticated) telnetOut.print("\b \b");
      }
      continue;
    }

    if (ch >= 32 && ch <= 126) {
      if (telnetLine.length() < 160) {
        telnetLine += char(ch);
        // Windows telnet disables local echo after WILL ECHO; echo printable
        // command characters here. Password input intentionally remains hidden.
        if (telnetAuthenticated) telnetOut.write(ch);
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
