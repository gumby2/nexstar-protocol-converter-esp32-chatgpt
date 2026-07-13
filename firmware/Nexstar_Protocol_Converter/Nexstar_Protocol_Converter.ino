#include <Arduino.h>
#include "logging.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#if defined(ESP8266)
  #include <LittleFS.h>
#else
  #include <FS.h>
  #include <LittleFS.h>
#endif

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266HTTPClient.h>
  #include <ESP8266WebServer.h>
  #include <WiFiUdp.h>
  #include <SoftwareSerial.h>
#else
  #include <WiFi.h>
  #include <HTTPClient.h>
  #include <WebServer.h>

  #include <WiFiUdp.h>
  #include <Preferences.h>
  #include <BluetoothSerial.h>
  extern "C" {
    #include "esp_https_server.h"
    #include "esp_coexist.h"
  }
  #include "esp_system.h"
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
#endif

#if defined(ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S2)
  // Classic BT shares radio resources with WiFi on ESP32. Keep the feature in
  // the sketch, but make it an explicit build-time mode while WiFi/TCP is debugged.
  #ifndef ENABLE_CLASSIC_BT
    #define ENABLE_CLASSIC_BT 1
  #endif
  #define HAS_CLASSIC_BT ENABLE_CLASSIC_BT
#else
  #define HAS_CLASSIC_BT 0
#endif

#if defined(ESP8266)
  #define MOUNT_RX_PIN D5
  #define MOUNT_TX_PIN D7
  SoftwareSerial MountSerial(MOUNT_RX_PIN, MOUNT_TX_PIN);
  #define BOARD_NAME "ESP8266 ESP-12E / D1 mini style"
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
  #define MOUNT_RX_PIN 16
  #define MOUNT_TX_PIN 17
  HardwareSerial MountSerial(1);
  #define BOARD_NAME "ESP32-S2"
#else
  #define MOUNT_RX_PIN 16
  #define MOUNT_TX_PIN 17
  HardwareSerial MountSerial(2);
  #define BOARD_NAME "Regular ESP32"
#endif

// Optional hardware startup-mode override pins.
// Wire selected pin to GND before reset/power-up. Pins use INPUT_PULLUP.
// These overrides are runtime-only; they do not erase the saved boot mode.
// Avoid GPIO16/17 because they are used for mount UART.
#define GPIO_STARTUP_MODE_ENABLED 1
#define GPIO_STARTUP_ACTIVE_LOW 1
#define GPIO_STARTUP_FULL_WIFI_PIN 32       // GND at boot: Full WiFi web mode
#define GPIO_STARTUP_BT_WEB_PIN 33          // GND at boot: BT + tiny web
#define GPIO_STARTUP_BT_TELNET_PIN 25       // GND at boot: BT Telnet-only
#define GPIO_STARTUP_SAMPLE_DELAY_MS 75

const char* AP_SSID = "NexStar_Bridge";
const char* AP_PASS = "12345678";
const char* WIFI_FILE = "/wifi.cfg";

// ESP32 keeps AP fallback up, but saved STA credentials should survive reboot.
const bool ESP32_FORCE_AP_DEFAULTS = false;
const bool ESP32_DISABLE_LITTLEFS_SETTINGS = true; // ESP32 uses Preferences/NVS instead
const bool ESP32_BOOT_AP_ONLY = false;
const bool ESP32_BOOT_DISABLE_BACKGROUND_POLLING = false;
const bool ESP32_BOOT_WEB_ONLY = false;
const uint8_t BRIDGE_MODE_WIFI_FULL = 0;
const uint8_t BRIDGE_MODE_BT_MIN_WEB = 1;
uint8_t bridgeMode = BRIDGE_MODE_WIFI_FULL;
bool bluetoothRuntimeEnabled = false;
String coexPreferenceText = "default";
int coexPreferenceResult = 0;

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
const char* BT_NAME = "NexStar_Bridge";

const char* FW_VERSION = "v5.29";
const char* FW_NAME = "NexStar Protocol Converter";

// Stability defaults: preserve all features, but avoid surprise background load.
const bool WEB_STARTUP_READ_MOUNT = false;
const unsigned long WEB_STATUS_REFRESH_MS = 8000;
const unsigned long WEB_LOG_REFRESH_MS = 5000;
extern const bool SERIAL_MIRROR_FULL_WIFI_LOGS = true; // mirror the same formatted Full WiFi log lines to USB serial


const uint16_t HTTP_WEB_PORT = 80;
uint16_t ALPACA_PORT = 11111;
uint16_t LX200_PORT = 4030;
uint16_t STELLARIUM_PORT = 10001;
uint16_t TELNET_PORT = 23;
const uint16_t ALPACA_DISCOVERY_PORT = 32227;

#if defined(ESP8266)
  ESP8266WebServer *webServer = nullptr;
  ESP8266WebServer *alpacaServer = nullptr;
  ESP8266WebServer *activeServer = nullptr;
#else
  WebServer *webServer = nullptr;
  WebServer *alpacaServer = nullptr;
  WebServer *activeServer = nullptr;
#endif
#define server (*activeServer)

// HTTPS setup credentials and state are stored separately.
#include "https_credentials.h"



WiFiUDP discoveryUdp;
WiFiServer *lx200ServerPtr = nullptr;
#define lx200Server (*lx200ServerPtr)
WiFiClient lx200Client;
bool lx200WifiClientConnected = false;
unsigned long lx200WifiLastRxMs = 0;
unsigned long lx200WifiLastTxMs = 0;
uint32_t lx200WifiRxCommands = 0;
uint32_t lx200WifiTxReplies = 0;
uint32_t lx200WifiUnhandledCommands = 0;

bool lx200BtClientConnected = false;
unsigned long lx200BtLastRxMs = 0;
unsigned long lx200BtLastTxMs = 0;
uint32_t lx200BtRxCommands = 0;
uint32_t lx200BtTxReplies = 0;
uint32_t lx200BtUnhandledCommands = 0;
String lx200BtLastCommand = "";
String lx200BtLastReply = "";
String lx200BtLastUnhandled = "";
bool lx200SuppressNextReplyLog = false;
uint32_t lx200BtGotoStageCommands = 0;
uint32_t lx200BtPollOnlyHintCount = 0;
uint32_t lx200CommonRouterCommands = 0;
uint32_t positionApiCacheReplies = 0;
uint32_t positionApiCacheMisses = 0;
WiFiServer *stellariumServerPtr = nullptr;
#define stellariumServer (*stellariumServerPtr)
WiFiServer *telnetServerPtr = nullptr;
#define telnetServer (*telnetServerPtr)
WiFiServer tinySetupServer(80);
WiFiClient telnetClient;
bool telnetClientConnected = false;
bool telnetAuthenticated = false;
String telnetLine = "";
bool telnetLastWasCR = false;
String telnetPassword = "nexstar";
unsigned long telnetLastRxMs = 0;
uint32_t telnetRxCommands = 0;
uint32_t telnetAuthFailures = 0;
bool telnetLiveLogEnabled = true;          // mirror filtered firmware logs to authenticated Telnet clients
uint32_t telnetLiveLogLines = 0;
String telnetPendingConfirmCommand = "";
String telnetPendingConfirmDescription = "";
String serialPendingConfirmCommand = "";
String serialPendingConfirmDescription = "";
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
unsigned long webCpuLastIdle0 = 0;
unsigned long webCpuLastIdle1 = 0;
unsigned long webCpuLastRuntimeTotal = 0;
int webCpuLoadPct = -1;
WiFiClient stellariumClient;
bool stellariumClientConnected = false;
unsigned long stellariumLastRxMs = 0;
unsigned long stellariumLastTxMs = 0;
uint32_t stellariumRxPackets = 0;
uint32_t stellariumTxPackets = 0;
uint32_t stellariumBadPackets = 0;

String staSsid = "";
String staPass = "";
String apSsid = "NexStar_Bridge";
String apPass = "12345678";
String apIp = "192.168.4.1";
String lastStaIp = "";
bool restartPending = false;
unsigned long restartAtMs = 0;
bool apRestartPending = false;
unsigned long apRestartAtMs = 0;
bool staRuntimeDisabled = false;
bool staConfigured = false;
bool staConnected = false;
bool apRunning = false;
bool btLiteBootWebEnabled = true;         // Saved boot option: BT tiny web server on/off. Telnet still starts.
bool tinyWebServerRuntimeEnabled = true;  // Runtime-only serial console toggle. Reboot restores from btLiteBootWebEnabled.
bool wifiRuntimeEnabled = true;           // Runtime-only serial console WiFi radio toggle. Reboot restores saved WiFi mode.
const bool BT_AUTO_WIFI_OFF_ON_CLIENT = false;
unsigned long btClientConnectedAtMs = 0;
bool btAutoWifiOffDone = false;
String wifiModeText = "AP";
String lastWifiStatus = "AP fallback";
String startupModeSource = "saved settings";
int startupModePinUsed = -1;

enum TimeSource {
  TIME_SRC_NONE = 0,
  TIME_SRC_MANUAL = 1,
  TIME_SRC_SKYSAFARI = 2,
  TIME_SRC_WEB = 3,
  TIME_SRC_NTP = 4
};

TimeSource currentTimeSource = TIME_SRC_NONE;
String currentLocationSource = "None";
double approxIpLatitudeDeg = 0.0;
double approxIpLongitudeDeg = 0.0;
String approxIpLocationText = "";
String approxIpLocationStatus = "Not fetched";
bool approxIpLocationValid = false;

bool ntpEnabled = true;
char ntpServer1[64] = "pool.ntp.org";
char ntpServer2[64] = "time.nist.gov";
char tzRule[96] = "MST7MDT,M3.2.0/2,M11.1.0/2";
unsigned long lastNtpSyncMs = 0;
bool ntpSyncValid = false;

#if HAS_CLASSIC_BT
BluetoothSerial SerialBT;
#endif

const unsigned long MOUNT_BAUD = 9600;
const unsigned long HANDSHAKE_TIMEOUT_MS = 3000;
const unsigned long READ_TIMEOUT_MS = 3000;
const unsigned long GOTO_TIMEOUT_MS = 180000;

const unsigned long COOLDOWN_MS = 100;
const unsigned long AFTER_HANDSHAKE_TX_DELAY_MS = 50;
const unsigned long AFTER_COMMAND_TX_DELAY_MS = 50;
const unsigned long BEFORE_PAYLOAD_DELAY_MS = 100;

double nudgeRateDeg[4] = {0.10, 0.25, 0.50, 1.00};
double lx200CurrentNudgeStepDeg = 0.25;
int webSelectedRateIndex = 1;

double safeDecMinDeg = -90.0;
double safeDecMaxDeg = 90.0;
bool safeDecLimitEnabled = false; // deprecated: use altitude safety instead
double safeAltMinDeg = 0.0;
double safeAltMaxDeg = 90.0;
bool safeAltLimitEnabled = false;

unsigned long pollIntervalMs = 2000;
unsigned long idlePollIntervalMs = 8000;
unsigned long minClientPollIntervalMs = 1000;
unsigned long lastMountPollMs = 0;
unsigned long lastComputedCoordMs = 0;
unsigned long lastPositionDemandMs = 0;
const unsigned long POSITION_DEMAND_ACTIVE_MS = 15000;

enum LX200Source {
  LX_SRC_WIFI = 0,
  LX_SRC_BT   = 1
};

const char* currentWebRequestPath = "";


bool mountBusy = false;
bool alpacaConnected = true;
uint32_t alpacaHttpRequests = 0;
bool asyncSlewPending = false;
bool asyncSlewRunning = false;
unsigned long asyncSlewEarliestMs = 0;
unsigned long lastGotoAcceptedMs = 0;

enum QueuedGotoType {
  QUEUED_GOTO_NONE = 0,
  QUEUED_GOTO_RADEC = 1,
  QUEUED_GOTO_ALTAZ = 2
};

QueuedGotoType queuedGotoType = QUEUED_GOTO_NONE;
double queuedGotoRA_deg = 0.0;
double queuedGotoDec_deg = 0.0;
double queuedGotoAlt_deg = 0.0;
double queuedGotoAz_deg = 0.0;
uint8_t queuedGotoSource = LX_SRC_WIFI;
unsigned long queuedGotoMs = 0;
unsigned long queuedGotoLastLogMs = 0;
unsigned long gotoQueueTimeoutMs = 10000UL;  // default: 10s unless a slew is already in progress
const unsigned long GOTO_QUEUE_LOG_MS = 5000UL;
uint32_t gotoQueueAccepted = 0;
uint32_t gotoQueueStarted = 0;
uint32_t gotoQueueTimedOut = 0;
uint32_t gotoQueueReplaced = 0;
uint32_t nudgeGotoQueueRequests = 0;
uint32_t gotoQueueImmediateAcks = 0;
uint32_t queuedGotoPositionCacheReplies = 0;

// SkySafari/LX200 UI slew-state helper.
// This is intentionally separate from the real NexStar mount busy/watch flags:
// - Real mount GOTO remains single-command and cannot be aborted.
// - SkySafari can get stuck showing Stop if our app-side GOTO state stays active
//   through post-GOTO verification or after a harmless :Q# UI stop request.
bool lx200GotoUiActive = false;
uint8_t lx200GotoUiSource = LX_SRC_WIFI;
unsigned long lx200GotoUiStartedMs = 0;
unsigned long lx200GotoUiCompletedMs = 0;
uint32_t lx200GotoUiStartedCount = 0;
uint32_t lx200GotoUiCompletedCount = 0;
uint32_t lx200GotoUiStopRequests = 0;

// Non-blocking SkySafari GOTO completion watcher.
// Original NexStar GOTO is still single-command: after R payload is sent,
// no E/Z mount query is allowed until final '@' arrives.
bool gotoCompletionWatchActive = false;
bool gotoCompletionVerifyPending = false;
unsigned long gotoCompletionWatchStartMs = 0;
unsigned long gotoCompletionWatchLastLogMs = 0;
unsigned long gotoCompletionVerifyDueMs = 0;
double gotoCompletionTargetRA_deg = 0.0;
double gotoCompletionTargetDec_deg = 0.0;
const unsigned long GOTO_COMPLETION_WATCH_LOG_MS = 15000;
const unsigned long POST_GOTO_VERIFY_DELAY_MS = 750;
const unsigned long POST_GOTO_POLL_QUIET_MS = 60000;
const unsigned long BT_POST_GOTO_FAST_POLL_WINDOW_MS = 20000;
const unsigned long BT_POST_GOTO_FAST_POLL_MS = 300;
const unsigned long SKYSAFARI_RECENT_GUARD_MS = 90000;
const unsigned long SKYSAFARI_BT_TRAFFIC_GUARD_MS = 250;
const unsigned long SKYSAFARI_BT_COMMAND_POLL_QUIET_MS = 1500;
const size_t LX200_MAX_CMD_LEN = 96;

unsigned long lastMountResponseMs = 0;
unsigned long lastMountFaultMs = 0;
unsigned long lx200BtLastCommandHandledMs = 0;
bool mountCommFault = false;
String lastMountFault = "No mount communication yet";
bool lastGotoCommandAccepted = false;
uint8_t backgroundPollFailCount = 0;
const uint8_t BACKGROUND_POLL_FAIL_LIMIT = 3;
bool backgroundPollingAutoDisabled = false;
bool suppressNextMountFault = false;
unsigned long lastPersistentSaveMs = 0;
uint32_t persistentSaveCount = 0;

unsigned long lastLoopTimeMs = 0;
unsigned long maxLoopTimeMs = 0;
unsigned long lastLoopLatencyMs = 0;
unsigned long maxLoopLatencyMs = 0;
unsigned long lastPollSchedulerLatencyMs = 0;
unsigned long maxPollSchedulerLatencyMs = 0;
unsigned long nextMountPollDueMs = 0;
unsigned long mountPollsStarted = 0;
unsigned long mountPollsDeferredBusy = 0;
unsigned long mountPollsMissedDeadline = 0;
unsigned long mountPollsSucceeded = 0;
unsigned long firstSuccessfulMountPollMs = 0;
unsigned long lastSuccessfulMountPollMs = 0;
int lastSuccessfulMountPollYear = 0;
int lastSuccessfulMountPollMonth = 0;
int lastSuccessfulMountPollDay = 0;
int lastSuccessfulMountPollHour = 0;
int lastSuccessfulMountPollMinute = 0;
int lastSuccessfulMountPollSecond = 0;

double cachedRA_deg = 0.0;
double cachedDec_deg = 0.0;
bool cacheValid = false;

double cachedAlt_deg = 0.0;
double cachedAz_deg = 0.0;
bool altAzCacheValid = false;
bool altAzComputed = false;

// Banner/current-position values. These are written ONLY by successful mount
// position reads: E for RA/Dec and Z for Alt/Az. Target slews, safety math,
// and coordinate conversions must never write these.
double mountCurrentRA_deg = 0.0;
double mountCurrentDec_deg = 0.0;
bool mountCurrentRaDecValid = false;
double mountCurrentAlt_deg = 0.0;
double mountCurrentAz_deg = 0.0;
bool mountCurrentAltAzValid = false;
unsigned long mountCurrentRaDecMs = 0;
unsigned long mountCurrentAltAzMs = 0;

double targetRA_deg = 0.0;
double targetDec_deg = 0.0;
double lx200TargetRA_deg = 0.0;
double lx200TargetDec_deg = 0.0;
double pendingRA_deg = 0.0;
double pendingDec_deg = 0.0;
double pendingAlt_deg = 0.0;
double pendingAz_deg = 0.0;
double pendingNudgeAltDelta = 0.0;
double pendingNudgeAzDelta = 0.0;
bool asyncAltAzSlewPending = false;
unsigned long asyncAltAzSlewEarliestMs = 0;
bool asyncNudgePending = false;
bool asyncRaDecReadPending = false;
bool asyncAltAzReadPending = false;

double siteLatitudeDeg = 0.0;
double siteLongitudeDeg = 0.0;   // east-positive internally
double siteElevationMeters = 0.0;
int utcOffsetMinutes = 0;
String utcOffsetSource = "unset";
bool siteValid = false;
bool timeValid = false;

int localYear = 2026;
int localMonth = 1;
int localDay = 1;
int localHour = 0;
int localMinute = 0;
int localSecond = 0;
unsigned long timeSetMillis = 0;

// Site/time updates received through Alpaca are staged here and applied from loop(),
// not inside the HTTP handler. This avoids ESP8266 crashes caused by doing
// String-heavy logging/recomputation while the web server is processing PUTs.
bool pendingSiteTimeUpdate = false;
bool pendingSiteLatitudeSet = false;
bool pendingSiteLongitudeSet = false;
bool pendingSiteElevationSet = false;
bool pendingUtcDateSet = false;
double pendingSiteLatitudeDeg = 0.0;
double pendingSiteLongitudeDeg = 0.0;
double pendingSiteElevationMeters = 0.0;
int pendingLocalYear = 2026;
int pendingLocalMonth = 1;
int pendingLocalDay = 1;
int pendingLocalHour = 0;
int pendingLocalMinute = 0;
int pendingLocalSecond = 0;

uint32_t serverTransactionID = 0;

void handleAlpacaDiscovery();
void handleLX200Server();
void handleStellariumServer();
void handleBluetoothLX200();
void serviceGotoCompletionWatch();
void serviceGotoQueue();
void serviceMountPolling();
void startFallbackAP();

// Full WiFi log lines are mirrored to serial by addLogLine().
// Use LOG_* / LOG_*_CAT macros for formatted serial-visible logging.

void computeAltAzFromRaDec();
void computeRaDecFromAltAz();

const uint32_t SETTINGS_MAGIC = 0x4E504333; // "NPC3"
const uint16_t SETTINGS_VERSION = 5;
const char* SETTINGS_FILE = "/settings.bin";

struct PersistentSettings {
  uint32_t magic;
  uint16_t version;
  uint16_t size;

  double siteLatitudeDeg;
  double siteLongitudeDeg;
  double siteElevationMeters;
  int utcOffsetMinutes;
  unsigned long pollIntervalMs;
  unsigned long idlePollIntervalMs;
  unsigned long minClientPollIntervalMs;
  bool siteValid;

  double nudgeRateDeg[4];
  int webSelectedRateIndex;
  double lx200CurrentNudgeStepDeg;
  bool ntpEnabled;
  char ntpServer1[64];
  char ntpServer2[64];
  char tzRule[96];
  char apSsid[33];
  char apPass[65];
  char apIp[16];
  uint16_t alpacaPort;
  uint16_t lx200Port;
  uint16_t stellariumPort;
  double safeDecMinDeg;
  double safeDecMaxDeg;
  bool safeDecLimitEnabled;
  double safeAltMinDeg;
  double safeAltMaxDeg;
  bool safeAltLimitEnabled;
  unsigned long gotoQueueTimeoutMs;

  uint32_t checksum;
};


#if defined(ESP32)
bool saveSettingsNVS() {
  Preferences prefs;
  if (!prefs.begin("nexstar", false)) {
    LOG_SET_E("NVS save FAILED: prefs.begin write");
    return false;
  }

  prefs.putBool("valid", true);
  prefs.putDouble("lat", siteLatitudeDeg);
  prefs.putDouble("lon", siteLongitudeDeg);
  prefs.putDouble("elev", siteElevationMeters);
  prefs.putInt("utcOff", utcOffsetMinutes);
  prefs.putUInt("pollMs", pollIntervalMs);
  prefs.putUInt("idlePoll", idlePollIntervalMs);
  prefs.putUInt("minPoll", minClientPollIntervalMs);
  prefs.putBool("siteValid", siteValid);

  prefs.putDouble("nudge0", nudgeRateDeg[0]);
  prefs.putDouble("nudge1", nudgeRateDeg[1]);
  prefs.putDouble("nudge2", nudgeRateDeg[2]);
  prefs.putDouble("nudge3", nudgeRateDeg[3]);
  prefs.putInt("rateIdx", webSelectedRateIndex);
  prefs.putDouble("curNudge", lx200CurrentNudgeStepDeg);

  prefs.putBool("ntp", ntpEnabled);
  prefs.putString("ntp1", String(ntpServer1));
  prefs.putString("ntp2", String(ntpServer2));
  prefs.putString("tz", String(tzRule));

  prefs.putString("apSsid", apSsid);
  prefs.putString("apPass", apPass);
  prefs.putString("apIp", apIp);
  prefs.putBool("staRunDis", staRuntimeDisabled);
  prefs.putBool("btWebBoot", btLiteBootWebEnabled);
  prefs.putUShort("alpaca", ALPACA_PORT);
  prefs.putUShort("lx200", LX200_PORT);
  prefs.putUShort("stell", STELLARIUM_PORT);
  prefs.putUShort("telnet", TELNET_PORT);
  prefs.putString("telPass", telnetPassword);
  prefs.putUInt("gqTimeout", gotoQueueTimeoutMs);

  prefs.putDouble("decMin", safeDecMinDeg);
  prefs.putDouble("decMax", safeDecMaxDeg);
  prefs.putBool("decEn", false);
  prefs.putDouble("altMin", safeAltMinDeg);
  prefs.putDouble("altMax", safeAltMaxDeg);
  prefs.putBool("altEn", safeAltLimitEnabled);

  prefs.end();
  LOG_SET_I("NVS settings saved");
  return true;
}

bool loadSettingsNVS() {
  Preferences prefs;
  if (!prefs.begin("nexstar", true)) {
    LOG_SET_W("NVS load skipped: prefs.begin read failed");
    return false;
  }

  bool valid = prefs.getBool("valid", false);
  if (!valid) {
    prefs.end();
    LOG_SET_W("NVS has no valid settings yet");
    return false;
  }

  siteLatitudeDeg = prefs.getDouble("lat", siteLatitudeDeg);
  siteLongitudeDeg = prefs.getDouble("lon", siteLongitudeDeg);
  siteElevationMeters = prefs.getDouble("elev", siteElevationMeters);
  utcOffsetMinutes = prefs.getInt("utcOff", utcOffsetMinutes);
  pollIntervalMs = prefs.getUInt("pollMs", pollIntervalMs);
  idlePollIntervalMs = prefs.getUInt("idlePoll", idlePollIntervalMs);
  minClientPollIntervalMs = prefs.getUInt("minPoll", minClientPollIntervalMs);
  siteValid = prefs.getBool("siteValid", siteValid);

  nudgeRateDeg[0] = prefs.getDouble("nudge0", nudgeRateDeg[0]);
  nudgeRateDeg[1] = prefs.getDouble("nudge1", nudgeRateDeg[1]);
  nudgeRateDeg[2] = prefs.getDouble("nudge2", nudgeRateDeg[2]);
  nudgeRateDeg[3] = prefs.getDouble("nudge3", nudgeRateDeg[3]);
  webSelectedRateIndex = prefs.getInt("rateIdx", webSelectedRateIndex);
  lx200CurrentNudgeStepDeg = prefs.getDouble("curNudge", lx200CurrentNudgeStepDeg);

  ntpEnabled = prefs.getBool("ntp", ntpEnabled);
  String n1 = prefs.getString("ntp1", String(ntpServer1));
  String n2 = prefs.getString("ntp2", String(ntpServer2));
  String tz = prefs.getString("tz", String(tzRule));
  strlcpy(ntpServer1, n1.c_str(), sizeof(ntpServer1));
  strlcpy(ntpServer2, n2.c_str(), sizeof(ntpServer2));
  strlcpy(tzRule, tz.c_str(), sizeof(tzRule));

  apSsid = prefs.getString("apSsid", apSsid);
  apPass = prefs.getString("apPass", apPass);
  apIp = prefs.getString("apIp", apIp);
  staRuntimeDisabled = prefs.getBool("staRunDis", staRuntimeDisabled);
  btLiteBootWebEnabled = prefs.getBool("btWebBoot", btLiteBootWebEnabled);
  tinyWebServerRuntimeEnabled = btLiteBootWebEnabled;

#if defined(ESP32)
  ALPACA_PORT = prefs.getUShort("alpaca", ALPACA_PORT);
#else
  ALPACA_PORT = prefs.getUShort("alpaca", ALPACA_PORT);
#endif
  LX200_PORT = prefs.getUShort("lx200", LX200_PORT);
  STELLARIUM_PORT = prefs.getUShort("stell", STELLARIUM_PORT);
  TELNET_PORT = prefs.getUShort("telnet", TELNET_PORT);
  telnetPassword = prefs.getString("telPass", telnetPassword);
  if (TELNET_PORT < 1) TELNET_PORT = 23;
  gotoQueueTimeoutMs = prefs.getUInt("gqTimeout", gotoQueueTimeoutMs);
  if (gotoQueueTimeoutMs < 1000UL || gotoQueueTimeoutMs > 600000UL) gotoQueueTimeoutMs = 10000UL;

  safeDecMinDeg = prefs.getDouble("decMin", safeDecMinDeg);
  safeDecMaxDeg = prefs.getDouble("decMax", safeDecMaxDeg);
  safeDecLimitEnabled = false;
  safeAltMinDeg = prefs.getDouble("altMin", safeAltMinDeg);
  safeAltMaxDeg = prefs.getDouble("altMax", safeAltMaxDeg);
  safeAltLimitEnabled = prefs.getBool("altEn", safeAltLimitEnabled);

  prefs.end();

  if (apIp == "192.168.4.1") apIp = "192.168.4.1";
  if (apPass.length() < 8) apPass = "12345678";

  if (siteValid) currentLocationSource = "Saved NVS";
  if (utcOffsetMinutes != 0) utcOffsetSource = "Saved NVS";

  LOG_SET_I("NVS settings loaded");
  return true;
}

bool clearSettingsNVS() {
  Preferences prefs;
  if (!prefs.begin("nexstar", false)) return false;
  bool ok = prefs.clear();
  prefs.end();
  return ok;
}
#endif

uint32_t checksumSettings(const void *settingsPtr) {
  const PersistentSettings &s = *(const PersistentSettings*)settingsPtr;
  const uint8_t *p = (const uint8_t*)&s;
  uint32_t sum = 0x12345678;

  // Use the actual offset of checksum, not sizeof(struct)-4.
  // Some ESP8266/ESP32 struct layouts add trailing padding after the checksum field.
  // Using sizeof(struct)-4 can accidentally include the checksum bytes in the checksum,
  // causing every EEPROM verification to fail.
  const size_t checksumOffset = offsetof(PersistentSettings, checksum);

  for (size_t i = 0; i < checksumOffset; i++) {
    sum = (sum << 5) | (sum >> 27);
    sum ^= p[i];
  }
  return sum;
}

bool savePersistentSettings() {
  LOG_SET_T("savePersistentSettings begin");
#if defined(ESP32)
  return saveSettingsNVS();
#endif
  PersistentSettings s;
  memset(&s, 0, sizeof(s));
  s.magic = SETTINGS_MAGIC;
  s.version = SETTINGS_VERSION;
  s.size = sizeof(PersistentSettings);

  s.siteLatitudeDeg = siteLatitudeDeg;
  s.siteLongitudeDeg = siteLongitudeDeg;
  s.siteElevationMeters = siteElevationMeters;
  s.utcOffsetMinutes = utcOffsetMinutes;
  s.pollIntervalMs = pollIntervalMs;
  s.idlePollIntervalMs = idlePollIntervalMs;
  s.minClientPollIntervalMs = minClientPollIntervalMs;
  s.siteValid = siteValid;

  for (int i = 0; i < 4; i++) s.nudgeRateDeg[i] = nudgeRateDeg[i];
  s.webSelectedRateIndex = webSelectedRateIndex;
  s.lx200CurrentNudgeStepDeg = lx200CurrentNudgeStepDeg;
  s.ntpEnabled = ntpEnabled;
  strlcpy(s.ntpServer1, ntpServer1, sizeof(s.ntpServer1));
  strlcpy(s.ntpServer2, ntpServer2, sizeof(s.ntpServer2));
  strlcpy(s.tzRule, tzRule, sizeof(s.tzRule));
  strlcpy(s.apSsid, apSsid.c_str(), sizeof(s.apSsid));
  strlcpy(s.apPass, apPass.c_str(), sizeof(s.apPass));
  strlcpy(s.apIp, apIp.c_str(), sizeof(s.apIp));
  s.alpacaPort = ALPACA_PORT;
  s.lx200Port = LX200_PORT;
  s.stellariumPort = STELLARIUM_PORT;
  s.safeDecMinDeg = safeDecMinDeg;
  s.safeDecMaxDeg = safeDecMaxDeg;
  s.safeDecLimitEnabled = false;
  s.safeAltMinDeg = safeAltMinDeg;
  s.safeAltMaxDeg = safeAltMaxDeg;
  s.safeAltLimitEnabled = safeAltLimitEnabled;
  s.gotoQueueTimeoutMs = gotoQueueTimeoutMs;

  s.checksum = checksumSettings(&s);

  File f = LittleFS.open(SETTINGS_FILE, "w");
  if (!f) {
    LOG_SET_E("LittleFS settings save FAILED: could not open %s for write", SETTINGS_FILE);
    return false;
  }

  size_t written = f.write((const uint8_t*)&s, sizeof(PersistentSettings));
  f.close();

  if (written != sizeof(PersistentSettings)) {
    LOG_SET_E("LittleFS settings save FAILED: wrote %u of %u bytes",
         (unsigned)written, (unsigned)sizeof(PersistentSettings));
    return false;
  }

  PersistentSettings check;
  memset(&check, 0, sizeof(check));
  File r = LittleFS.open(SETTINGS_FILE, "r");
  if (!r) {
    LOG_SET_E("LittleFS settings verify FAILED: could not reopen %s", SETTINGS_FILE);
    return false;
  }

  size_t readBytes = r.read((uint8_t*)&check, sizeof(PersistentSettings));
  r.close();

  uint32_t calc = checksumSettings(&check);
  bool verifyOk =
    readBytes == sizeof(PersistentSettings) &&
    check.magic == SETTINGS_MAGIC &&
    check.version == SETTINGS_VERSION &&
    check.size == sizeof(PersistentSettings) &&
    check.checksum == calc;

  if (!verifyOk) {
    LOG_SET_E("LittleFS settings verify FAILED: read=%u magic=0x%08lX ver=%u size=%u storedCks=0x%08lX calcCks=0x%08lX",
         (unsigned)readBytes,
         (unsigned long)check.magic,
         (unsigned)check.version,
         (unsigned)check.size,
         (unsigned long)check.checksum,
         (unsigned long)calc);
    return false;
  }

  lastPersistentSaveMs = millis();
  persistentSaveCount++;

  LOG_SET_I("LittleFS settings saved/verified: lastSite lat=%.6f lon=%.6f elev=%.1f offset=%d poll=%lu throttle=%lu rates=%.3f/%.3f/%.3f/%.3f",
       siteLatitudeDeg, siteLongitudeDeg, siteElevationMeters, utcOffsetMinutes,
       pollIntervalMs, minClientPollIntervalMs,
       nudgeRateDeg[0], nudgeRateDeg[1], nudgeRateDeg[2], nudgeRateDeg[3]);

  return true;
}

bool loadPersistentSettings() {
#if defined(ESP32)
  return loadSettingsNVS();
#endif
  if (!LittleFS.exists(SETTINGS_FILE)) {
    LOG_SET_I("No LittleFS settings file exists: %s", SETTINGS_FILE);
    return false;
  }

  PersistentSettings s;
  memset(&s, 0, sizeof(s));

  File f = LittleFS.open(SETTINGS_FILE, "r");
  if (!f) {
    LOGW("Could not open LittleFS settings file: %s", SETTINGS_FILE);
    return false;
  }

  size_t readBytes = f.read((uint8_t*)&s, sizeof(PersistentSettings));
  f.close();

  if (readBytes != sizeof(PersistentSettings)) {
    LOG_SET_W("Persistent settings invalid length: read=%u expected=%u",
         (unsigned)readBytes, (unsigned)sizeof(PersistentSettings));
    return false;
  }

  if (s.magic != SETTINGS_MAGIC || s.version != SETTINGS_VERSION || s.size != sizeof(PersistentSettings)) {
    LOG_SET_W("Persistent settings invalid header: magic=0x%08lX version=%u size=%u expectedMagic=0x%08lX expectedVersion=%u expectedSize=%u",
         (unsigned long)s.magic, (unsigned)s.version, (unsigned)s.size,
         (unsigned long)SETTINGS_MAGIC, (unsigned)SETTINGS_VERSION, (unsigned)sizeof(PersistentSettings));
    return false;
  }

  uint32_t calc = checksumSettings(&s);
  if (s.checksum != calc) {
    LOG_SET_W("Persistent settings invalid checksum: stored=0x%08lX calculated=0x%08lX",
         (unsigned long)s.checksum, (unsigned long)calc);
    return false;
  }

  siteLatitudeDeg = s.siteLatitudeDeg;
  siteLongitudeDeg = s.siteLongitudeDeg;
  siteElevationMeters = s.siteElevationMeters;
  utcOffsetMinutes = s.utcOffsetMinutes;
  pollIntervalMs = s.pollIntervalMs;
  idlePollIntervalMs = s.idlePollIntervalMs;
  minClientPollIntervalMs = s.minClientPollIntervalMs;
  siteValid = s.siteValid;
  timeValid = false; // exact clock time is not retained through power-off

  for (int i = 0; i < 4; i++) {
    if (!isnan(s.nudgeRateDeg[i]) && s.nudgeRateDeg[i] > 0.0 && s.nudgeRateDeg[i] <= 5.0) {
      nudgeRateDeg[i] = s.nudgeRateDeg[i];
    }
  }

  webSelectedRateIndex = s.webSelectedRateIndex;
  if (webSelectedRateIndex < 0 || webSelectedRateIndex > 3) webSelectedRateIndex = 1;

  lx200CurrentNudgeStepDeg = s.lx200CurrentNudgeStepDeg;
  if (isnan(lx200CurrentNudgeStepDeg) || lx200CurrentNudgeStepDeg <= 0.0 || lx200CurrentNudgeStepDeg > 5.0) {
    lx200CurrentNudgeStepDeg = nudgeRateDeg[webSelectedRateIndex];
  }

  ntpEnabled = s.ntpEnabled;
  if (s.ntpServer1[0]) strlcpy(ntpServer1, s.ntpServer1, sizeof(ntpServer1));
  if (s.ntpServer2[0]) strlcpy(ntpServer2, s.ntpServer2, sizeof(ntpServer2));
  if (s.tzRule[0]) strlcpy(tzRule, s.tzRule, sizeof(tzRule));
  if (s.apSsid[0]) apSsid = String(s.apSsid);
  if (s.apPass[0]) apPass = String(s.apPass);
  if (s.apIp[0]) apIp = String(s.apIp);
  ALPACA_PORT = 11111; // separate Alpaca HTTP listener; Web UI remains on port 80
  if (s.lx200Port >= 1024 && s.lx200Port <= 65535) LX200_PORT = s.lx200Port;
  if (s.stellariumPort >= 1024 && s.stellariumPort <= 65535) STELLARIUM_PORT = s.stellariumPort;
  if (!isnan(s.safeDecMinDeg) && !isnan(s.safeDecMaxDeg) && s.safeDecMinDeg >= -90.0 && s.safeDecMaxDeg <= 90.0 && s.safeDecMinDeg < s.safeDecMaxDeg) {
    safeDecMinDeg = s.safeDecMinDeg;
    safeDecMaxDeg = s.safeDecMaxDeg;
    safeDecLimitEnabled = false; // deprecated
  }
  if (!isnan(s.safeAltMinDeg) && !isnan(s.safeAltMaxDeg) && s.safeAltMinDeg >= 0.0 && s.safeAltMaxDeg <= 90.0 && s.safeAltMinDeg < s.safeAltMaxDeg) {
    safeAltMinDeg = s.safeAltMinDeg;
    safeAltMaxDeg = s.safeAltMaxDeg;
    safeAltLimitEnabled = s.safeAltLimitEnabled;
  }
  if (s.gotoQueueTimeoutMs >= 1000UL && s.gotoQueueTimeoutMs <= 600000UL) gotoQueueTimeoutMs = s.gotoQueueTimeoutMs;

  if (pollIntervalMs > 60000) pollIntervalMs = 5000;
  if (idlePollIntervalMs > 60000) idlePollIntervalMs = 8000;
  if (minClientPollIntervalMs < 250 || minClientPollIntervalMs > 60000) minClientPollIntervalMs = 2000;

  currentLocationSource = siteValid ? "Saved" : "None";
  LOG_SET_I("LittleFS settings loaded: lastSite lat=%.6f lon=%.6f elev=%.1f offset=%d poll=%lu throttle=%lu rates=%.3f/%.3f/%.3f/%.3f",
       siteLatitudeDeg, siteLongitudeDeg, siteElevationMeters, utcOffsetMinutes,
       pollIntervalMs, minClientPollIntervalMs,
       nudgeRateDeg[0], nudgeRateDeg[1], nudgeRateDeg[2], nudgeRateDeg[3]);

  return true;
}

void clearPersistentSettings() {
  if (LittleFS.exists(SETTINGS_FILE)) {
    LittleFS.remove(SETTINGS_FILE);
  }

  siteLatitudeDeg = 0.0;
  siteLongitudeDeg = 0.0;
  siteElevationMeters = 0.0;
  utcOffsetMinutes = 0;
  siteValid = false;
  timeValid = false;

  LOG_SET_I("LittleFS persistent last location/settings cleared");
}


void markMountResponse() {
  lastMountResponseMs = millis();
  mountCommFault = false;
  lastMountFault = "";
  backgroundPollFailCount = 0;
}

void markMountFault(const String &reason) {
  lastMountFaultMs = millis();
  lastMountFault = reason;

  if (suppressNextMountFault) {
    LOGD("Suppressed transient mount fault: %s", reason.c_str());
    suppressNextMountFault = false;
    return;
  }

  mountCommFault = true;
}

bool mountAlive() {
  return lastMountResponseMs > 0 && !mountCommFault;
}

unsigned long mountLastResponseAge() {
  if (lastMountResponseMs == 0) return 0;
  return millis() - lastMountResponseMs;
}

bool isPrintableByte(uint8_t b) {
  return b >= 32 && b <= 126;
}

void safeDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    delay(1);
    yield();
  }
}

double degToRad(double d) { return d * PI / 180.0; }
double radToDeg(double r) { return r * 180.0 / PI; }

double normalizeRA(double ra) {
  while (ra < 0.0) ra += 360.0;
  while (ra >= 360.0) ra -= 360.0;
  return ra;
}

double normalizeAz(double az) {
  while (az < 0.0) az += 360.0;
  while (az >= 360.0) az -= 360.0;
  return az;
}

double clampAlt(double alt) {
  if (alt > 90.0) return 90.0;
  if (alt < -90.0) return -90.0;
  return alt;
}

double numberToAngle(int16_t number) {
  return 180.0 * ((double)number / 32768.0);
}

int16_t angleToNumber(double angleDeg) {
  long value = floor((angleDeg / 180.0) * 32768.0);
  if (value < -32768) value = -32768;
  if (value > 32767) value = 32767;
  return (int16_t)value;
}

double signedRAForMount(double ra) {
  ra = normalizeRA(ra);
  if (ra > 180.0) return 0.0 - (360.0 - ra);
  return ra;
}

double signedAzForMount(double az) {
  az = normalizeAz(az);
  if (az > 180.0) return az - 360.0;
  return az;
}

double rateToNudgeStep(double rate) {
  double r = fabs(rate);
  if (r <= 0.0001) return 0.0;
  if (r <= 0.25) return nudgeRateDeg[0];
  if (r <= 0.50) return nudgeRateDeg[1];
  if (r <= 1.00) return nudgeRateDeg[2];
  return nudgeRateDeg[3];
}

void printRxByte(uint8_t b) {
  LOG_MOUNT_T("RX MOUNT 0x%02X '%c'", b, isPrintableByte(b) ? b : '.');
}

void printTxByte(uint8_t b) {
  LOG_MOUNT_T("TX MOUNT 0x%02X '%c'", b, isPrintableByte(b) ? b : '.');
}

void mountWriteByte(uint8_t b) {
  printTxByte(b);
  MountSerial.write(b);
  MountSerial.flush();
}

void mountWriteBE16(int16_t value) {
  mountWriteByte((uint8_t)((value >> 8) & 0xFF));
  mountWriteByte((uint8_t)(value & 0xFF));
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

void serviceNetworkDuringMountWait() {
  serviceHttpServers();

  if (bridgeMode == BRIDGE_MODE_WIFI_FULL) {
    // Full WiFi has been the stable reference path. Keep its normal network
    // servicing during mount waits so HTTP/LX200/Stellarium stay responsive.
    handleLX200Server();
    handleStellariumServer();
    handleAlpacaDiscovery();
  }

#if HAS_CLASSIC_BT
  if (bridgeMode == BRIDGE_MODE_BT_MIN_WEB) {
    // Do NOT call handleBluetoothLX200() here.
    // The original NexStar mount is strictly single-command. Re-entering the
    // Bluetooth LX200 parser while a mount transaction is waiting for bytes can
    // allow SkySafari polls or staged commands to run in the middle of the
    // mount's '?'/'E'/'Z'/'R'/'A' transaction. BT should use the
    // same stable single-command discipline as the Full WiFi path.
    delay(1);
    yield();
  }
#endif

  serviceNtpSync();
  delay(1);
  yield();
}

bool mountReadByte(uint8_t &b, unsigned long timeoutMs) {
  unsigned long start = millis();

  while (millis() - start < timeoutMs) {
    if (MountSerial.available()) {
      b = MountSerial.read();
      printRxByte(b);
      markMountResponse();
      return true;
    }

    serviceNetworkDuringMountWait();
  }

  String reason = "mountReadByte timeout after " + String(timeoutMs) + " ms";
  LOGW("%s", reason.c_str());
  markMountFault(reason);
  return false;
}


bool mountReadByteQuiet(uint8_t &b, unsigned long timeoutMs) {
  unsigned long start = millis();

  while (millis() - start < timeoutMs) {
    if (MountSerial.available()) {
      b = MountSerial.read();
      printRxByte(b);
      markMountResponse();
      return true;
    }

    serviceNetworkDuringMountWait();
  }

  return false;
}

bool mountReadExact(uint8_t* buf, size_t len, unsigned long timeoutMs) {
  size_t got = 0;
  unsigned long start = millis();

  while (got < len && millis() - start < timeoutMs) {
    if (MountSerial.available()) {
      uint8_t b = MountSerial.read();
      buf[got++] = b;
      printRxByte(b);
      markMountResponse();
      start = millis();
    } else {
      serviceNetworkDuringMountWait();
    }
  }

  if (got != len) {
    String reason = "mountReadExact timeout: expected " + String((unsigned)len) + " got " + String((unsigned)got);
    LOG_MOUNT_W("%s", reason.c_str());
    markMountFault(reason);
    return false;
  }

  return true;
}

void drainMount() {
  int count = 0;
  while (MountSerial.available()) {
    uint8_t b = MountSerial.read();
    LOG_MOUNT_I("[DRAIN] 0x%02X '%c'", b, isPrintableByte(b) ? b : '.');
    markMountResponse();
    count++;
  }
  LOG_MOUNT_I("Drained %d byte(s)", count);
}

bool beginMountCommand(const char* name) {
  if (mountBusy) {
    LOG_MOUNT_D("Blocked mount command '%s': mountBusy=true", name);
    return false;
  }

  mountBusy = true;
  LOG_MOUNT_D("Begin mount command: %s", name);
  return true;
}

void endMountCommand(const char* name) {
  LOG_MOUNT_D("End mount command: %s", name);
  mountBusy = false;
}

bool nexstarHandshakeLocked(const char* name) {
  safeDelay(COOLDOWN_MS);

  LOG_MOUNT_D("%s: sending handshake '?'", name);
  mountWriteByte('?');

  safeDelay(AFTER_HANDSHAKE_TX_DELAY_MS);

  uint8_t resp = 0;
  if (!mountReadByte(resp, HANDSHAKE_TIMEOUT_MS)) {
    String reason = String(name) + ": handshake failed, no '#'";
    if (suppressNextMountFault) LOGW("%s", reason.c_str());
    else LOGE("%s", reason.c_str());
    markMountFault(reason);
    return false;
  }

  if (resp != '#') {
    String reason = String(name) + ": handshake failed, expected '#', got 0x" + String(resp, HEX);
    if (suppressNextMountFault) LOGW("%s", reason.c_str());
    else LOGE("%s", reason.c_str());
    markMountFault(reason);
    return false;
  }

  LOG_MOUNT_D("%s: handshake OK", name);
  markMountResponse();
  safeDelay(COOLDOWN_MS);
  return true;
}

void computeAltAzFromRaDec();
void computeRaDecFromAltAz();

bool testInit() {
  const char* name = "testInit";
  if (!beginMountCommand(name)) return false;

  bool ok = false;

  if (nexstarHandshakeLocked(name)) {
    LOG_MOUNT_D("testInit: completing transaction with safe E command");
    mountWriteByte('E');
    safeDelay(AFTER_COMMAND_TX_DELAY_MS);

    uint8_t payload[4];
    if (mountReadExact(payload, 4, READ_TIMEOUT_MS)) {
      LOG_MOUNT_I("testInit OK: handshake plus E payload received");
      ok = true;
    } else {
      LOG_MOUNT_E("testInit failed: handshake OK but E payload failed");
    }
  }

  endMountCommand(name);
  return ok;
}

bool getRaDec(double &ra, double &dec, bool forcePoll = false) {
  if (!forcePoll && cacheValid && millis() - lastMountPollMs < minClientPollIntervalMs) {
    ra = cachedRA_deg;
    dec = cachedDec_deg;
    return true;
  }

  const char* name = "GET RA/Dec E";
  if (!beginMountCommand(name)) return false;

  bool ok = false;

  if (nexstarHandshakeLocked(name)) {
    LOG_MOUNT_D("Sending command E");
    mountWriteByte('E');
    safeDelay(AFTER_COMMAND_TX_DELAY_MS);

    uint8_t payload[4];

    if (mountReadExact(payload, 4, READ_TIMEOUT_MS)) {
      int16_t ran = (int16_t)((payload[0] << 8) | payload[1]);
      int16_t decn = (int16_t)((payload[2] << 8) | payload[3]);

      ra = numberToAngle(ran);
      dec = numberToAngle(decn);

      if (ra < 0.0) ra += 360.0;
      ra = normalizeRA(ra);

      cachedRA_deg = ra;
      cachedDec_deg = dec;
      cacheValid = true;
      mountCurrentRA_deg = ra;
      mountCurrentDec_deg = dec;
      mountCurrentRaDecValid = true;
      mountCurrentRaDecMs = millis();
      lastMountPollMs = millis();

      // Do not compute/overwrite current Alt/Az from RA/Dec here.
      // Banner/current Alt/Az must come from the mount Z command only.
      LOG_MOUNT_I("GET RA/Dec OK FROM MOUNT: RA_deg=%.6f DEC_deg=%.6f RA_hours=%.6f", ra, dec, ra / 15.0);
      ok = true;
    } else {
      LOG_MOUNT_E("GET RA/Dec failed: expected 4 bytes");
    }
  }

  endMountCommand(name);
  return ok;
}

bool getAltAzFromMount(double &alt, double &az) {
  const char* name = "GET Az/Alt Z";
  if (!beginMountCommand(name)) return false;

  bool ok = false;

  if (nexstarHandshakeLocked(name)) {
    LOG_MOUNT_D("Sending command Z");
    mountWriteByte('Z');
    safeDelay(AFTER_COMMAND_TX_DELAY_MS);

    uint8_t payload[4];

    if (mountReadExact(payload, 4, READ_TIMEOUT_MS)) {
      int16_t azn = (int16_t)((payload[0] << 8) | payload[1]);
      int16_t altn = (int16_t)((payload[2] << 8) | payload[3]);

      az = numberToAngle(azn);
      alt = numberToAngle(altn);

      if (az < 0.0) az += 360.0;
      az = normalizeAz(az);

      cachedAlt_deg = alt;
      cachedAz_deg = az;
      altAzCacheValid = true;
      altAzComputed = false;
      mountCurrentAlt_deg = alt;
      mountCurrentAz_deg = az;
      mountCurrentAltAzValid = true;
      mountCurrentAltAzMs = millis();

      LOG_MOUNT_I("GET Alt/Az FROM MOUNT OK: ALT_deg=%.6f AZ_deg=%.6f", alt, az);
      ok = true;
    } else {
      LOG_MOUNT_E("GET Alt/Az failed: expected 4 bytes");
    }
  }

  endMountCommand(name);
  return ok;
}

bool getAltAz(double &alt, double &az, bool forceMountPoll = false) {
  if (forceMountPoll) return getAltAzFromMount(alt, az);

  // Current Alt/Az should be mount-reported only. Computed Alt/Az may be useful
  // for safety math, but it must not feed banner/current-position display.
  if (altAzCacheValid && !altAzComputed) {
    alt = cachedAlt_deg;
    az = cachedAz_deg;
    return true;
  }

  return getAltAzFromMount(alt, az);
}

bool gotoRaDec(double ra, double dec) {
  const char* name = "GOTO RA/Dec R";
  if (!beginMountCommand(name)) return false;

  bool ok = false;
  lastGotoCommandAccepted = false;
  asyncSlewRunning = true;

  ra = normalizeRA(ra);
  double mountRA = signedRAForMount(ra);

  int16_t ran = angleToNumber(mountRA);
  int16_t decn = angleToNumber(dec);

  LOG_MOUNT_I("GOTO RA/Dec request: RA_deg=%.6f DEC_deg=%.6f mountRA=%.6f RA_int=%d DEC_int=%d",
       ra, dec, mountRA, ran, decn);

  if (nexstarHandshakeLocked(name)) {
    mountWriteByte('R');
    safeDelay(AFTER_COMMAND_TX_DELAY_MS);
    safeDelay(BEFORE_PAYLOAD_DELAY_MS);

    mountWriteBE16(ran);
    mountWriteBE16(decn);

    // If the payload was fully written after a valid handshake, the original NexStar
    // has accepted the command. Do NOT overwrite cached/current banner position
    // with the target. Current position is updated only by actual read/poll.
    ok = true;
    lastGotoCommandAccepted = true;
    lastGotoAcceptedMs = millis();

    LOG_MOUNT_I("GOTO RA/Dec command accepted/sent; completion will be handled by caller/watch logic");
  }

  asyncSlewRunning = false;
  endMountCommand(name);
  return ok;
}


bool gotoRaDecAndWait(double ra, double dec, const char* reasonName) {
  const char* name = reasonName ? reasonName : "GOTO RA/Dec R wait";
  if (!beginMountCommand(name)) return false;

  bool ok = false;
  lastGotoCommandAccepted = false;
  asyncSlewRunning = true;

  ra = normalizeRA(ra);
  double mountRA = signedRAForMount(ra);

  int16_t ran = angleToNumber(mountRA);
  int16_t decn = angleToNumber(dec);

  LOG_MOUNT_I("%s request: RA_deg=%.6f DEC_deg=%.6f mountRA=%.6f RA_int=%d DEC_int=%d",
              name, ra, dec, mountRA, ran, decn);

  if (nexstarHandshakeLocked(name)) {
    mountWriteByte('R');
    safeDelay(AFTER_COMMAND_TX_DELAY_MS);
    safeDelay(BEFORE_PAYLOAD_DELAY_MS);

    mountWriteBE16(ran);
    mountWriteBE16(decn);

    lastGotoCommandAccepted = true;
    lastGotoAcceptedMs = millis();

    ok = waitForNexStarCompletion(name, GOTO_TIMEOUT_MS);
    if (ok) {
      LOG_MOUNT_I("%s complete", name);

      // Do not fake the cache to the requested target. The original NexStar mount
      // must be re-read after '@' so SkySafari sees the actual mount-reported
      // position, not just the requested coordinates.
      lastMountPollMs = 0;
    }
  }

  asyncSlewRunning = false;
  endMountCommand(name);
  return ok;
}

bool gotoAltAz(double alt, double az) {
  const char* name = "GOTO Az/Alt A";
  if (!beginMountCommand(name)) return false;

  bool ok = false;
  lastGotoCommandAccepted = false;
  asyncSlewRunning = true;

  alt = clampAlt(alt);
  az = normalizeAz(az);

  double mountAz = signedAzForMount(az);
  int16_t azn = angleToNumber(mountAz);
  int16_t altn = angleToNumber(alt);

  LOG_MOUNT_I("GOTO Alt/Az request: ALT=%.6f AZ=%.6f mountAZ=%.6f AZ_int=%d ALT_int=%d",
       alt, az, mountAz, azn, altn);

  if (nexstarHandshakeLocked(name)) {
    mountWriteByte('A');
    safeDelay(AFTER_COMMAND_TX_DELAY_MS);
    safeDelay(BEFORE_PAYLOAD_DELAY_MS);

    mountWriteBE16(azn);
    mountWriteBE16(altn);

    // If the payload was fully written after a valid handshake, the original NexStar
    // has accepted the command. Do NOT overwrite cached/current banner position
    // with the target. Current position is updated only by actual read/poll.
    ok = true;
    lastGotoCommandAccepted = true;
    lastGotoAcceptedMs = millis();

    LOG_MOUNT_I("GOTO Alt/Az command accepted/sent; completion will be handled by caller/watch logic");
  }

  asyncSlewRunning = false;
  endMountCommand(name);
  return ok;
}


bool waitForNexStarCompletion(const char* name, unsigned long timeoutMs) {
  unsigned long start = millis();
  LOG_MOUNT_D("%s: waiting for NexStar completion '@'", name);

  while (millis() - start < timeoutMs) {
    uint8_t b = 0;
    if (mountReadByteQuiet(b, 250)) {
      if (b == '@') {
        LOG_MOUNT_I("%s: completion '@' received", name);
        markMountResponse();
        return true;
      }
      LOG_MOUNT_W("%s: ignored byte while waiting for '@': 0x%02X '%c'",
                  name, b, isPrintableByte(b) ? b : '.');
    }

#if HAS_CLASSIC_BT
    // Keep the Bluetooth stack alive, but do not process new SkySafari commands
    // while the original NexStar mount is completing this single-command move.
    if (bridgeMode == BRIDGE_MODE_BT_MIN_WEB) {
      delay(1);
      yield();
    } else
#endif
    {
      serviceNetworkDuringMountWait();
    }
  }

  String reason = String(name) + ": timed out waiting for completion '@'";
  LOG_MOUNT_W("%s", reason.c_str());
  markMountFault(reason);
  return false;
}


bool gotoAltAzAndWait(double alt, double az, const char* reasonName) {
  const char* name = reasonName ? reasonName : "GOTO Az/Alt A wait";
  if (!beginMountCommand(name)) return false;

  bool ok = false;
  lastGotoCommandAccepted = false;
  asyncSlewRunning = true;

  alt = clampAlt(alt);
  az = normalizeAz(az);

  double mountAz = signedAzForMount(az);
  int16_t azn = angleToNumber(mountAz);
  int16_t altn = angleToNumber(alt);

  LOG_MOUNT_I("%s request: ALT=%.6f AZ=%.6f mountAZ=%.6f AZ_int=%d ALT_int=%d",
              name, alt, az, mountAz, azn, altn);

  if (nexstarHandshakeLocked(name)) {
    mountWriteByte('A');
    safeDelay(AFTER_COMMAND_TX_DELAY_MS);
    safeDelay(BEFORE_PAYLOAD_DELAY_MS);

    mountWriteBE16(azn);
    mountWriteBE16(altn);

    lastGotoCommandAccepted = true;
    lastGotoAcceptedMs = millis();

    ok = waitForNexStarCompletion(name, GOTO_TIMEOUT_MS);
    if (ok) {
      LOG_MOUNT_I("%s complete", name);
      lastMountPollMs = 0;
    }
  }

  asyncSlewRunning = false;
  endMountCommand(name);
  return ok;
}

bool nudgeRelativeAltAz(double altDelta, double azDelta) {
  // Web/Alpaca/local nudges use the same GOTO queue path as SkySafari nudges.
  return queueRelativeAltAzGoto(altDelta, azDelta, LX_SRC_WIFI, "relative nudge while mount busy");
}

bool nudgeAxis(int axis, double rate) {
  double step = rateToNudgeStep(rate);

  if (step <= 0.0) {
    LOGI("Nudge stop command accepted: axis=%d rate=%.6f", axis, rate);
    return true;
  }

  if (axis == 0) return nudgeRelativeAltAz(0.0, rate > 0 ? step : -step);
  if (axis == 1) return nudgeRelativeAltAz(rate > 0 ? step : -step, 0.0);

  LOGE("Nudge failed: unsupported axis=%d", axis);
  return false;
}

long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int)(doe) - 719468;
}

int estimateUtcOffsetMinutesFromLongitude(double lonDeg) {
  // Alpaca sends UTCDate, but it does not normally send a time-zone offset.
  // For display and local-time math, estimate the standard-zone offset from longitude.
  // Example: Colorado longitude around -104.8 gives -420 minutes.
  int offset = (int)round(lonDeg / 15.0) * 60;
  if (offset < -720) offset = -720;
  if (offset > 840) offset = 840;
  return offset;
}

void civilFromDays(long z, int &y, int &mo, int &d) {
  z += 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  mo = mp + (mp < 10 ? 3 : -9);
  y += (mo <= 2);
}

void setLocalFromUtcWithOffset(int uy, int umo, int ud, int uh, int umi, int usec, int offsetMinutes) {
  long utcDays = daysFromCivil(uy, umo, ud);
  long seconds = (long)uh * 3600L + (long)umi * 60L + (long)usec + (long)offsetMinutes * 60L;

  while (seconds < 0) {
    seconds += 86400L;
    utcDays--;
  }
  while (seconds >= 86400L) {
    seconds -= 86400L;
    utcDays++;
  }

  civilFromDays(utcDays, localYear, localMonth, localDay);
  localHour = seconds / 3600L;
  seconds %= 3600L;
  localMinute = seconds / 60L;
  localSecond = seconds % 60L;
}

void currentUtcParts(int &y, int &mo, int &d, int &h, int &mi, int &se) {
  unsigned long elapsed = timeValid ? (millis() - timeSetMillis) / 1000 : millis() / 1000;

  long localSeconds =
    (long)localHour * 3600L +
    (long)localMinute * 60L +
    (long)localSecond +
    (long)elapsed;

  long days = daysFromCivil(localYear, localMonth, localDay);
  long utcSeconds = days * 86400L + localSeconds - (long)utcOffsetMinutes * 60L;

  long utcDays = floor((double)utcSeconds / 86400.0);
  long sod = utcSeconds - utcDays * 86400L;
  if (sod < 0) {
    sod += 86400L;
    utcDays--;
  }

  h = sod / 3600;
  sod %= 3600;
  mi = sod / 60;
  se = sod % 60;

  long z = utcDays + 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  mo = mp + (mp < 10 ? 3 : -9);
  y += (mo <= 2);
}


void currentLocalParts(int &y, int &mo, int &d, int &h, int &mi, int &se) {
  unsigned long elapsed = timeValid ? (millis() - timeSetMillis) / 1000 : 0;

  long localSeconds =
    (long)localHour * 3600L +
    (long)localMinute * 60L +
    (long)localSecond +
    (long)elapsed;

  long days = daysFromCivil(localYear, localMonth, localDay);
  long outSeconds = days * 86400L + localSeconds;

  long outDays = floor((double)outSeconds / 86400.0);
  long sod = outSeconds - outDays * 86400L;
  if (sod < 0) {
    sod += 86400L;
    outDays--;
  }

  h = sod / 3600;
  sod %= 3600;
  mi = sod / 60;
  se = sod % 60;

  long z = outDays + 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  mo = mp + (mp < 10 ? 3 : -9);
  y += (mo <= 2);
}

double julianDateUtc() {
  int y, mo, d, h, mi, se;
  currentUtcParts(y, mo, d, h, mi, se);

  if (mo <= 2) {
    y -= 1;
    mo += 12;
  }

  int A = y / 100;
  int B = 2 - A + A / 4;

  double dayFrac = (h + mi / 60.0 + se / 3600.0) / 24.0;
  double JD = floor(365.25 * (y + 4716)) +
              floor(30.6001 * (mo + 1)) +
              d + dayFrac + B - 1524.5;

  return JD;
}

double gmstDegrees() {
  double jd = julianDateUtc();
  double T = (jd - 2451545.0) / 36525.0;

  double gmst = 280.46061837 +
                360.98564736629 * (jd - 2451545.0) +
                0.000387933 * T * T -
                (T * T * T) / 38710000.0;

  return normalizeRA(gmst);
}

void computeAltAzFromRaDec() {
  if (!cacheValid || !siteValid || !timeValid) return;

  double lstDeg = normalizeRA(gmstDegrees() + siteLongitudeDeg);
  double haDeg = normalizeRA(lstDeg - cachedRA_deg);
  if (haDeg > 180.0) haDeg -= 360.0;

  double ha = degToRad(haDeg);
  double dec = degToRad(cachedDec_deg);
  double lat = degToRad(siteLatitudeDeg);

  double sinAlt = sin(dec) * sin(lat) + cos(dec) * cos(lat) * cos(ha);
  sinAlt = constrain(sinAlt, -1.0, 1.0);
  double alt = asin(sinAlt);

  double cosAz = (sin(dec) - sin(alt) * sin(lat)) / (cos(alt) * cos(lat));
  cosAz = constrain(cosAz, -1.0, 1.0);

  double az = acos(cosAz);
  if (sin(ha) > 0) az = 2.0 * PI - az;

  cachedAlt_deg = radToDeg(alt);
  cachedAz_deg = normalizeAz(radToDeg(az));
  altAzCacheValid = true;
  altAzComputed = true;
  mountCurrentAlt_deg = cachedAlt_deg;
  mountCurrentAz_deg = cachedAz_deg;
  mountCurrentAltAzValid = true;
  mountCurrentAltAzMs = millis();
  lastComputedCoordMs = millis();
}

void computeRaDecFromAltAz() {
  if (!altAzCacheValid || !siteValid || !timeValid) return;

  double lstDeg = normalizeRA(gmstDegrees() + siteLongitudeDeg);
  double alt = degToRad(cachedAlt_deg);
  double az = degToRad(cachedAz_deg);
  double lat = degToRad(siteLatitudeDeg);

  double sinDec = sin(alt) * sin(lat) + cos(alt) * cos(lat) * cos(az);
  sinDec = constrain(sinDec, -1.0, 1.0);
  double dec = asin(sinDec);

  double y = -sin(az) * cos(alt);
  double x = sin(alt) * cos(lat) - cos(alt) * sin(lat) * cos(az);
  double haDeg = normalizeRA(radToDeg(atan2(y, x)));
  if (haDeg > 180.0) haDeg -= 360.0;

  double ra = normalizeRA(lstDeg - haDeg);

  cachedRA_deg = ra;
  cachedDec_deg = radToDeg(dec);
  cacheValid = true;
  lastComputedCoordMs = millis();

  LOGD("Computed RA/Dec from Alt/Az: RA_deg=%.6f DEC_deg=%.6f RA_hours=%.6f",
       cachedRA_deg, cachedDec_deg, cachedRA_deg / 15.0);
}


double parseLX200RA(String cmd) {
  String raw = cmd.substring(3);
  int c1 = raw.indexOf(':');
  int c2 = raw.indexOf(':', c1 + 1);
  if (c1 < 0) return 0.0;

  double h = raw.substring(0, c1).toFloat();
  double m = 0.0;
  double s = 0.0;

  if (c2 >= 0) {
    m = raw.substring(c1 + 1, c2).toFloat();
    s = raw.substring(c2 + 1).toFloat();
  } else {
    m = raw.substring(c1 + 1).toFloat();
  }

  return normalizeRA((h + m / 60.0 + s / 3600.0) * 15.0);
}

double parseLX200Dec(String cmd) {
  String raw = cmd.substring(3);
  raw.replace("*", ":");
  raw.replace("'", ":");
  raw.replace("\"", ":");

  int sign = raw.startsWith("-") ? -1 : 1;
  if (raw.startsWith("+") || raw.startsWith("-")) raw = raw.substring(1);

  int c1 = raw.indexOf(':');
  int c2 = raw.indexOf(':', c1 + 1);

  double d = c1 >= 0 ? raw.substring(0, c1).toFloat() : raw.toFloat();
  double m = c1 >= 0 ? raw.substring(c1 + 1, c2 >= 0 ? c2 : raw.length()).toFloat() : 0.0;
  double s = c2 >= 0 ? raw.substring(c2 + 1).toFloat() : 0.0;

  return sign * (d + m / 60.0 + s / 3600.0);
}

double parseSignedDegMin(String raw) {
  raw.replace("*", ":");
  raw.replace("'", ":");
  int sign = 1;
  if (raw.startsWith("-")) sign = -1;
  if (raw.startsWith("+") || raw.startsWith("-")) raw = raw.substring(1);

  int c = raw.indexOf(':');
  double deg = c >= 0 ? raw.substring(0, c).toFloat() : raw.toFloat();
  double min = c >= 0 ? raw.substring(c + 1).toFloat() : 0.0;
  return sign * (deg + min / 60.0);
}

String lx200Ra(double ra) {
  ra = normalizeRA(ra);
  double totalMinutes = (ra / 15.0) * 60.0;
  int hours = ((int)(totalMinutes / 60.0)) % 24;
  double mf = totalMinutes - hours * 60.0;
  int minutes = (int)mf;
  int tenths = (int)round((mf - minutes) * 10.0);

  if (tenths >= 10) {
    tenths = 0;
    minutes++;
  }
  if (minutes >= 60) {
    minutes = 0;
    hours = (hours + 1) % 24;
  }

  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d.%d#", hours, minutes, tenths);
  return String(buf);
}

String lx200Dec(double dec) {
  char sign = dec >= 0 ? '+' : '-';
  double dabs = fabs(dec);
  int totalMinutes = (int)round(dabs * 60.0);
  int deg = totalMinutes / 60;
  int minutes = totalMinutes % 60;

  if (deg > 90) {
    deg = 90;
    minutes = 0;
  }

  char buf[16];
  snprintf(buf, sizeof(buf), "%c%02d*%02d#", sign, deg, minutes);
  return String(buf);
}

String lx200Alt(double alt) {
  return lx200Dec(alt);
}

String lx200Az(double az) {
  az = normalizeAz(az);
  int totalMinutes = (int)round(az * 60.0);
  int deg = totalMinutes / 60;
  int minutes = totalMinutes % 60;
  if (deg >= 360) deg = 0;

  char buf[16];
  snprintf(buf, sizeof(buf), "%03d*%02d#", deg, minutes);
  return String(buf);
}

String lx200SiteLat() {
  return lx200Dec(siteLatitudeDeg);
}

String lx200SiteLong() {
  // LX200/Meade longitudes are west-positive. Internally we use east-positive.
  double lon = -siteLongitudeDeg;
  char sign = lon >= 0 ? '+' : '-';
  double dabs = fabs(lon);
  int totalMinutes = (int)round(dabs * 60.0);
  int deg = totalMinutes / 60;
  int minutes = totalMinutes % 60;

  char buf[18];
  snprintf(buf, sizeof(buf), "%c%03d*%02d#", sign, deg, minutes);
  return String(buf);
}

String lx200Time() {
  unsigned long elapsed = timeValid ? (millis() - timeSetMillis) / 1000 : 0;
  long sec = localHour * 3600L + localMinute * 60L + localSecond + elapsed;
  sec %= 86400L;
  if (sec < 0) sec += 86400L;

  int h = sec / 3600;
  sec %= 3600;
  int m = sec / 60;
  int s = sec % 60;

  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d#", h, m, s);
  return String(buf);
}

String lx200Date() {
  char buf[16];
  int yy = localYear % 100;
  snprintf(buf, sizeof(buf), "%02d/%02d/%02d#", localMonth, localDay, yy);
  return String(buf);
}

String lx200UtcOffset() {
  // LX200 :GG/:SG use hours added to local time to get UTC.
  // Internally utcOffsetMinutes is local-minus-UTC.
  int totalHours = -utcOffsetMinutes / 60;
  char buf[8];
  snprintf(buf, sizeof(buf), "%+03d#", totalHours);
  return String(buf);
}

void lx200Send(uint8_t source, const String &s) {
  if (source == LX_SRC_WIFI) {
    if (lx200Client && lx200Client.connected()) {
      lx200Client.print(s);
      lx200Client.flush();
      lx200WifiLastTxMs = millis();
      lx200WifiTxReplies++;
      if (!lx200SuppressNextReplyLog) {
        LOG_SKY_I("LX200 WiFi TX reply #%lu: %s", (unsigned long)lx200WifiTxReplies, s.c_str());
      }
    } else {
      LOG_SKY_W("LX200 WiFi TX dropped; no connected SkySafari client: %s", s.c_str());
    }
  }

#if HAS_CLASSIC_BT
  if (source == LX_SRC_BT) {
    if (SerialBT.hasClient()) {
      SerialBT.print(s);
      SerialBT.flush();
      lx200BtLastTxMs = millis();
      lx200BtTxReplies++;
      lx200BtLastReply = s;
      if (!lx200SuppressNextReplyLog) {
        LOG_SKY_D("LX200 BT TX reply #%lu: %s", (unsigned long)lx200BtTxReplies, s.c_str());
      }
    }
  }
#endif
}

bool mountPositionApiRaDec(double &raDeg, double &decDeg, unsigned long *ageMs = nullptr) {
  if (!mountCurrentRaDecValid) {
    positionApiCacheMisses++;
    return false;
  }
  raDeg = mountCurrentRA_deg;
  decDeg = mountCurrentDec_deg;
  if (ageMs) *ageMs = millis() - mountCurrentRaDecMs;
  positionApiCacheReplies++;
  return true;
}

bool mountPositionApiAltAz(double &altDeg, double &azDeg, unsigned long *ageMs = nullptr) {
  if (!mountCurrentAltAzValid) {
    positionApiCacheMisses++;
    return false;
  }
  altDeg = mountCurrentAlt_deg;
  azDeg = mountCurrentAz_deg;
  if (ageMs) *ageMs = millis() - mountCurrentAltAzMs;
  positionApiCacheReplies++;
  return true;
}

bool mountPositionApiHasRaDec() {
  return mountCurrentRaDecValid;
}

bool mountPositionApiHasAltAz() {
  return mountCurrentAltAzValid;
}

void markPositionDemand(const char* reason = nullptr) {
  lastPositionDemandMs = millis();
  if (nextMountPollDueMs == 0 || !mountCurrentRaDecValid) nextMountPollDueMs = lastPositionDemandMs;
  if (reason) LOG_MOUNT_T("Position demand active: %s", reason);
}

bool positionDemandActive() {
  unsigned long nowMs = millis();
  if (lastPositionDemandMs && nowMs - lastPositionDemandMs < POSITION_DEMAND_ACTIVE_MS) return true;
  if (bridgeMode == BRIDGE_MODE_WIFI_FULL) {
    if (lx200Client && lx200Client.connected()) return true;
    if (stellariumClientConnected) return true;
  }
#if HAS_CLASSIC_BT
  if (bluetoothRuntimeEnabled && SerialBT.hasClient()) return true;
#endif
  return false;
}

unsigned long effectiveMountPollIntervalMs() {
  if (!timeValid) return 0;
  if (positionDemandActive()) return pollIntervalMs;
  return idlePollIntervalMs;
}

const char* lx200SourceName(uint8_t source) {
  return source == LX_SRC_BT ? "BT" : "WiFi";
}

bool lx200SourceIsPollCommand(const String &cmd) {
  return cmd == ":GR" || cmd == ":GD" || cmd == ":GA" || cmd == ":GZ";
}

void lx200MarkRxCommand(uint8_t source, const String &cmd, bool lineTerminated) {
  if (source == LX_SRC_WIFI) {
    lx200WifiLastRxMs = millis();
    lx200WifiRxCommands++;
    if (!lx200SourceIsPollCommand(cmd)) {
      LOG_SKY_D("LX200 WiFi %s command #%lu: %s%s",
                lineTerminated ? "line" : "complete",
                (unsigned long)lx200WifiRxCommands,
                cmd.c_str(),
                lineTerminated ? "" : "#");
    }
  }

#if HAS_CLASSIC_BT
  if (source == LX_SRC_BT) {
    lx200BtLastRxMs = millis();
    lx200BtRxCommands++;
    if (!lx200SourceIsPollCommand(cmd)) {
      LOG_SKY_D("LX200 BT %s command #%lu: %s%s",
                lineTerminated ? "line" : "complete",
                (unsigned long)lx200BtRxCommands,
                cmd.c_str(),
                lineTerminated ? "" : "#");
    }
  }
#endif
}

String readLX200CommandFromStream(Stream &io, uint8_t source, String &buffer) {
  const char* srcName = lx200SourceName(source);

  while (io.available()) {
    char c = (char)io.read();

    if ((uint8_t)c == 0x06 && buffer.length() == 0) {
      if (source == LX_SRC_WIFI) {
        lx200WifiLastRxMs = millis();
        lx200WifiRxCommands++;
        LOG_SKY_D("LX200 WiFi ACK query #%lu", (unsigned long)lx200WifiRxCommands);
      }
#if HAS_CLASSIC_BT
      else if (source == LX_SRC_BT) {
        lx200BtLastRxMs = millis();
        lx200BtRxCommands++;
        LOG_SKY_D("LX200 BT ACK query #%lu", (unsigned long)lx200BtRxCommands);
      }
#endif
      return String((char)0x06);
    }

    if (c == '\r' || c == '\n') {
      if (buffer.length() > 0 && buffer.charAt(0) == ':') {
        String cmd = buffer;
        buffer = "";
        cmd.trim();
        if (cmd.endsWith("#")) cmd.remove(cmd.length() - 1);
        lx200MarkRxCommand(source, cmd, true);
        return cmd;
      }
      buffer = "";
      continue;
    }

    buffer += c;

    if (buffer.length() > LX200_MAX_CMD_LEN) {
      LOG_SKY_W("LX200 %s command buffer overflow; clearing partial data: %s", srcName, buffer.c_str());
      buffer = "";
      continue;
    }

    if (c == '#') {
      String cmd = buffer;
      buffer = "";
      cmd.trim();
      if (cmd.endsWith("#")) cmd.remove(cmd.length() - 1);
      lx200MarkRxCommand(source, cmd, false);
      return cmd;
    }
  }

  return "";
}

String readLX200CommandForSource(uint8_t source) {
  static String wifiBuffer = "";
  static String btBuffer = "";

  if (source == LX_SRC_WIFI) {
    return readLX200CommandFromStream(lx200Client, source, wifiBuffer);
  }

#if HAS_CLASSIC_BT
  if (source == LX_SRC_BT) {
    return readLX200CommandFromStream(SerialBT, source, btBuffer);
  }
#endif

  return "";
}

bool routeLX200SourceThroughCommonMountCore(uint8_t source) {
  String cmd = readLX200CommandForSource(source);
  if (cmd.length() == 0) return false;

  lx200CommonRouterCommands++;
  LOG_SKY_T("LX200 common router #%lu source=%s cmd=%s",
            (unsigned long)lx200CommonRouterCommands,
            lx200SourceName(source),
            cmd.c_str());

  // This is the single shared LX200-to-mount command core.
  // WiFi and Bluetooth differ only at the transport read/write layer.
  handleLX200Command(cmd, source);
  return true;
}

// Compatibility wrappers retained for any older internal call sites.
String readLX200WiFiCommand() {
  return readLX200CommandForSource(LX_SRC_WIFI);
}

String readLX200BTCommand() {
  return readLX200CommandForSource(LX_SRC_BT);
}

void setLX200NudgeRate(const String &cmd) {
  double oldRate = lx200CurrentNudgeStepDeg;

  if (cmd == ":RS") lx200CurrentNudgeStepDeg = nudgeRateDeg[0];
  else if (cmd == ":RM") lx200CurrentNudgeStepDeg = nudgeRateDeg[1];
  else if (cmd == ":RC") lx200CurrentNudgeStepDeg = nudgeRateDeg[2];
  else if (cmd == ":RG") lx200CurrentNudgeStepDeg = nudgeRateDeg[3];

  if (fabs(lx200CurrentNudgeStepDeg - oldRate) > 0.000001) {
    LOG_SKY_I("LX200 nudge rate selected %s -> %.4f deg", cmd.c_str(), lx200CurrentNudgeStepDeg);
  } else {
    LOG_SKY_T("LX200 nudge rate repeat %s -> %.4f deg", cmd.c_str(), lx200CurrentNudgeStepDeg);
  }
}

void clearQueuedGoto(const char* reason);

bool isLX200GotoCommand(const String &cmd) {
  String c = cmd;
  c.trim();
  if (c.endsWith("#")) c.remove(c.length() - 1);
  c.toUpperCase();
  return c == ":MS" || c == ":MS0" || c == ":MS1" || c == ":MA" || c == ":MA0" || c == ":MA1";
}

void markLX200GotoUiStarted(uint8_t source, const char* detail) {
  lx200GotoUiActive = true;
  lx200GotoUiSource = source;
  lx200GotoUiStartedMs = millis();
  lx200GotoUiStartedCount++;
  LOG_SKY_I("LX200 %s UI GOTO state STARTED: %s", lx200SourceName(source), detail ? detail : "goto accepted");
}

void clearLX200GotoUiState(const char* reason) {
  if (lx200GotoUiActive) {
    unsigned long ageMs = millis() - lx200GotoUiStartedMs;
    LOG_SKY_I("LX200 %s UI GOTO state CLEARED after %lu ms: %s",
              lx200SourceName(lx200GotoUiSource),
              (unsigned long)ageMs,
              reason ? reason : "complete");
    lx200GotoUiCompletedCount++;
  } else {
    LOG_SKY_D("LX200 UI GOTO state already idle: %s", reason ? reason : "clear requested");
  }
  lx200GotoUiActive = false;
  lx200GotoUiCompletedMs = millis();
}

void handleLX200StopUiRequest(uint8_t source) {
  lx200GotoUiStopRequests++;

  // Cancel only app-side GOTO work that has not yet reached the mount.
  // Never send an abort command and never clear the real completion watch while
  // the original NexStar is executing a GOTO, because that mount must run to '@'.
  bool cancelledPending = false;
  if (!asyncSlewRunning && !gotoCompletionWatchActive && !gotoCompletionVerifyPending) {
    if (hasQueuedGoto()) {
      clearQueuedGoto("LX200 :Q# stop before mount command started");
      cancelledPending = true;
    }
    if (asyncSlewPending) {
      asyncSlewPending = false;
      cancelledPending = true;
      LOG_SKY_I("LX200 %s :Q# cleared pending RA/Dec async slew before mount command started", lx200SourceName(source));
    }
    if (asyncAltAzSlewPending) {
      asyncAltAzSlewPending = false;
      cancelledPending = true;
      LOG_SKY_I("LX200 %s :Q# cleared pending Alt/Az async slew before mount command started", lx200SourceName(source));
    }
  }

  clearLX200GotoUiState(cancelledPending ? "LX200 :Q# stop/cancel before mount command" : "LX200 :Q# UI stop; real NexStar abort unsupported");
}

void clearPendingReadRequestsForLX200Goto(const char* srcName) {
  bool hadPending = asyncRaDecReadPending || asyncAltAzReadPending;

  asyncRaDecReadPending = false;
  asyncAltAzReadPending = false;

  if (hadPending) {
    LOG_SKY_D("LX200 %s GOTO cleared pending read request(s) before mount command", srcName);
  }
}

bool mountCommandPathBusy() {
  return mountBusy || asyncSlewRunning || asyncSlewPending || asyncAltAzSlewPending ||
         asyncNudgePending || asyncRaDecReadPending || asyncAltAzReadPending ||
         gotoCompletionWatchActive || gotoCompletionVerifyPending;
}

bool gotoQueueSlewInProgress() {
  return asyncSlewRunning || gotoCompletionWatchActive || gotoCompletionVerifyPending;
}

unsigned long effectiveGotoQueueTimeoutMs() {
  // Default timeout is 10 seconds unless a slew is already in progress.
  // During an active slew/completion watch, timeout is deferred until that slew clears.
  if (gotoQueueSlewInProgress()) return 0UL;
  return gotoQueueTimeoutMs;
}

unsigned long sanitizeGotoQueueTimeoutMs(unsigned long v) {
  if (v < 1000UL) v = 1000UL;
  if (v > 600000UL) v = 600000UL;
  return v;
}

bool saveGotoQueueTimeoutMs(unsigned long v) {
  gotoQueueTimeoutMs = sanitizeGotoQueueTimeoutMs(v);
  return savePersistentSettings();
}

const char* queuedGotoTypeName(QueuedGotoType t) {
  if (t == QUEUED_GOTO_RADEC) return "RA/Dec";
  if (t == QUEUED_GOTO_ALTAZ) return "Alt/Az";
  return "none";
}

bool hasQueuedGoto() {
  return queuedGotoType != QUEUED_GOTO_NONE;
}

bool queuedGotoOrSlewActive() {
  return hasQueuedGoto() || asyncSlewPending || asyncAltAzSlewPending ||
         asyncSlewRunning || gotoCompletionWatchActive || gotoCompletionVerifyPending;
}

void markGotoQueueImmediateAck(const char* protocol, const char* detail) {
  gotoQueueImmediateAcks++;
  LOG_MOUNT_I("Queued GOTO caller acknowledged immediately: protocol=%s detail=%s activeQueue=%s",
              protocol ? protocol : "unknown",
              detail ? detail : "",
              hasQueuedGoto() ? "yes" : "no");
}

void clearQueuedGoto(const char* reason) {
  if (queuedGotoType != QUEUED_GOTO_NONE) {
    LOG_MOUNT_I("Clearing queued GOTO type=%s reason=%s",
                queuedGotoTypeName(queuedGotoType),
                reason ? reason : "none");
  }
  queuedGotoType = QUEUED_GOTO_NONE;
  queuedGotoLastLogMs = 0;
}

bool enqueueGotoRaDec(double raDeg, double decDeg, uint8_t source, const char* reason) {
  if (hasQueuedGoto()) {
    gotoQueueReplaced++;
    LOG_MOUNT_W("Replacing existing queued GOTO type=%s with new RA/Dec GOTO; old queued age=%lu ms",
                queuedGotoTypeName(queuedGotoType),
                (unsigned long)(millis() - queuedGotoMs));
  }

  queuedGotoType = QUEUED_GOTO_RADEC;
  queuedGotoRA_deg = normalizeRA(raDeg);
  queuedGotoDec_deg = decDeg;
  queuedGotoSource = source;
  queuedGotoMs = millis();
  queuedGotoLastLogMs = 0;
  gotoQueueAccepted++;

  targetRA_deg = queuedGotoRA_deg;
  targetDec_deg = queuedGotoDec_deg;
  lastGotoAcceptedMs = millis();

  LOG_MOUNT_I("Queued RA/Dec GOTO waiting for mount idle: RA_deg=%.6f DEC_deg=%.6f source=%s reason=%s timeout=%lu ms",
              queuedGotoRA_deg,
              queuedGotoDec_deg,
              lx200SourceName(source),
              reason ? reason : "busy",
              gotoQueueTimeoutMs);
  return true;
}

bool enqueueGotoAltAz(double altDeg, double azDeg, uint8_t source, const char* reason) {
  if (hasQueuedGoto()) {
    gotoQueueReplaced++;
    LOG_MOUNT_W("Replacing existing queued GOTO type=%s with new Alt/Az GOTO; old queued age=%lu ms",
                queuedGotoTypeName(queuedGotoType),
                (unsigned long)(millis() - queuedGotoMs));
  }

  queuedGotoType = QUEUED_GOTO_ALTAZ;
  queuedGotoAlt_deg = clampAlt(altDeg);
  queuedGotoAz_deg = normalizeAz(azDeg);
  queuedGotoSource = source;
  queuedGotoMs = millis();
  queuedGotoLastLogMs = 0;
  gotoQueueAccepted++;

  lastGotoAcceptedMs = millis();

  LOG_MOUNT_I("Queued Alt/Az GOTO waiting for mount idle: ALT_deg=%.6f AZ_deg=%.6f source=%s reason=%s timeout=%lu ms",
              queuedGotoAlt_deg,
              queuedGotoAz_deg,
              lx200SourceName(source),
              reason ? reason : "busy",
              gotoQueueTimeoutMs);
  return true;
}

void serviceGotoQueue() {
  if (!hasQueuedGoto()) return;

  unsigned long nowMs = millis();
  unsigned long ageMs = nowMs - queuedGotoMs;
  unsigned long timeoutMs = effectiveGotoQueueTimeoutMs();

  if (timeoutMs > 0UL && ageMs > timeoutMs) {
    gotoQueueTimedOut++;
    LOG_MOUNT_W("Queued %s GOTO timed out after %lu ms waiting for mount idle; timeout=%lu ms; command dropped",
                queuedGotoTypeName(queuedGotoType),
                (unsigned long)ageMs,
                timeoutMs);
    clearQueuedGoto("timeout");
    clearLX200GotoUiState("queued GOTO timed out before mount command started");
    return;
  }

  if (mountCommandPathBusy()) {
    if (queuedGotoLastLogMs == 0 || nowMs - queuedGotoLastLogMs >= GOTO_QUEUE_LOG_MS) {
      queuedGotoLastLogMs = nowMs;
      LOG_MOUNT_I("Queued %s GOTO still waiting for mount idle; age=%lu ms timeout=%lu ms%s",
                  queuedGotoTypeName(queuedGotoType),
                  (unsigned long)ageMs,
                  timeoutMs,
                  timeoutMs == 0UL ? " (deferred while slew in progress)" : "");
    }
    return;
  }

  QueuedGotoType type = queuedGotoType;
  double ra = queuedGotoRA_deg;
  double dec = queuedGotoDec_deg;
  double alt = queuedGotoAlt_deg;
  double az = queuedGotoAz_deg;

  clearPendingReadRequestsForLX200Goto(lx200SourceName(queuedGotoSource));
  clearQueuedGoto("starting");
  gotoQueueStarted++;

  if (type == QUEUED_GOTO_RADEC) {
    LOG_MOUNT_I("Starting queued RA/Dec GOTO now after wait: RA_deg=%.6f DEC_deg=%.6f", ra, dec);
    queueAsyncSlew(ra, dec);
  } else if (type == QUEUED_GOTO_ALTAZ) {
    LOG_MOUNT_I("Starting queued Alt/Az GOTO now after wait: ALT_deg=%.6f AZ_deg=%.6f", alt, az);
    queueAsyncAltAzSlew(alt, az);
  }
}

bool queueRelativeAltAzGoto(double altDelta, double azDelta, uint8_t source, const char* reason) {
  double alt = 0.0;
  double az = 0.0;
  unsigned long ageMs = 0;

  // Application-side nudges must not perform their own Z/E mount reads.
  // They use the same cached position API used by SkySafari/Stellarium polling.
  if (!mountPositionApiAltAz(alt, az, &ageMs)) {
    LOG_MOUNT_W("Nudge-to-GOTO rejected: mount-position API Alt/Az cache is not valid");
    return false;
  }

  double newAlt = clampAlt(alt + altDelta);
  double newAz = normalizeAz(az + azDelta);
  nudgeGotoQueueRequests++;

  LOG_MOUNT_I("Nudge-to-GOTO request source=%s age=%lu ms: Alt %.6f -> %.6f, Az %.6f -> %.6f",
              lx200SourceName(source),
              (unsigned long)ageMs,
              alt,
              newAlt,
              az,
              newAz);

  if (mountCommandPathBusy()) {
    return enqueueGotoAltAz(newAlt, newAz, source, reason ? reason : "nudge while mount busy");
  }

  queueAsyncAltAzSlew(newAlt, newAz);
  return true;
}

bool runLX200Nudge(const String &cmd, uint8_t source) {
  double altDelta = 0.0;
  double azDelta = 0.0;

  if (cmd == ":Mn") altDelta = lx200CurrentNudgeStepDeg;
  else if (cmd == ":Ms") altDelta = -lx200CurrentNudgeStepDeg;
  else if (cmd == ":Me") azDelta = lx200CurrentNudgeStepDeg;
  else if (cmd == ":Mw") azDelta = -lx200CurrentNudgeStepDeg;
  else return false;

  LOG_SKY_I("LX200 %s nudge converted to queued Alt/Az GOTO: %s -> Alt delta %.4f, Az delta %.4f",
       lx200SourceName(source), cmd.c_str(), altDelta, azDelta);

  return queueRelativeAltAzGoto(altDelta, azDelta, source, "LX200/SkySafari nudge while mount busy");
}

// Shared LX200-to-mount command core.
// All client transports, including WiFi SkySafari and Bluetooth SkySafari,
// must enter here through routeLX200SourceThroughCommonMountCore().
void handleLX200Command(const String &rawCmd, uint8_t source) {
  String cmd = rawCmd;
  if (cmd.length() == 0) return;

  const char* srcName = source == LX_SRC_BT ? "BT" : "WiFi";
  int colonPos = cmd.indexOf(':');
  if (colonPos > 0) {
    LOG_SKY_D("LX200 %s command had prefix noise, normalized %s -> %s",
              srcName, cmd.c_str(), cmd.substring(colonPos).c_str());
    cmd = cmd.substring(colonPos);
  }
  if (source == LX_SRC_BT) {
    lx200BtLastCommand = cmd;
    lx200BtLastCommandHandledMs = millis();
  }

  if ((uint8_t)cmd[0] == 0x06) {
    LOG_SKY_D("LX200 %s ACK query", srcName);
    lx200Send(source, "P");
    return;
  }

  bool lx200PollCommand = (cmd == ":GR" || cmd == ":GD" || cmd == ":GA" || cmd == ":GZ");
  if (lx200PollCommand) markPositionDemand(srcName);
  if (!lx200PollCommand) {
    LOG_SKY_I("LX200 %s RX: %s#", srcName, cmd.c_str());
  }

  if (cmd == ":GVP" || cmd == ":GVN" || cmd == ":GVD" || cmd == ":GVT") {
    lx200Send(source, "NexStarBridge#");
    return;
  }

  if (cmd == ":GW") {
    lx200Send(source, "n#");
    return;
  }

  if (cmd == ":D") {
    // LX200 distance-bars query. SkySafari uses this to decide whether the
    // Goto button should remain in Stop mode. Return distance bars only while
    // we are still physically waiting for the original NexStar final '@'.
    // As soon as '@' has arrived, even if post-GOTO E/Z verification is still
    // pending, return a bare # so SkySafari exits Stop mode.
    bool lx200ReportsSlewing = lx200GotoUiActive && gotoCompletionWatchActive;
    const char* reply = lx200ReportsSlewing ? "|#" : "#";
    LOG_SKY_D("LX200 %s :D# slew-status reply: %s  uiActive=%s watchActive=%s verifyPending=%s asyncSlewRunning=%s",
              srcName,
              reply,
              lx200GotoUiActive ? "true" : "false",
              gotoCompletionWatchActive ? "true" : "false",
              gotoCompletionVerifyPending ? "true" : "false",
              asyncSlewRunning ? "true" : "false");
    lx200Send(source, reply);
    return;
  }

  if (cmd == ":Gm" || cmd == ":Gd") {
    lx200Send(source, "00*00#");
    return;
  }

  if (cmd == ":U") return;

  if (cmd == ":RS" || cmd == ":RM" || cmd == ":RC" || cmd == ":RG") {
    setLX200NudgeRate(cmd);
    return;
  }

  if (cmd == ":Mn" || cmd == ":Ms" || cmd == ":Me" || cmd == ":Mw") {
    LOG_SKY_I("LX200 %s nudge command converted to queued Alt/Az GOTO: %s#", srcName, cmd.c_str());
    bool ok = runLX200Nudge(cmd, source);
    if (ok) markGotoQueueImmediateAck(srcName, "LX200 nudge accepted into Alt/Az GOTO queue path");
    if (!ok) LOG_SKY_W("LX200 %s nudge command failed: %s#", srcName, cmd.c_str());
    return;
  }

  if (cmd == ":Q" || cmd.startsWith(":Q")) {
    LOG_SKY_I("LX200 %s stop requested; original NexStar protocol has no abort command; clearing SkySafari UI/queued state only", srcName);
    handleLX200StopUiRequest(source);
    return;
  }

  if (cmd == ":GR" || cmd == ":GD") {
    // Application position polling uses the cached mount-position API only.
    // The real mount is refreshed by serviceMountPolling() on pollIntervalMs.
    // Do not perform E/Z serial transactions from SkySafari/Stellarium polling.
    double apiRa = 0.0;
    double apiDec = 0.0;
    unsigned long ageMs = 0;
    bool ok = mountPositionApiRaDec(apiRa, apiDec, &ageMs);
    if (!ok) {
      LOG_SKY_W("LX200 %s RA/Dec requested but mount-position API cache is not valid; returning safe defaults", srcName);
    }
    if (queuedGotoOrSlewActive()) {
      queuedGotoPositionCacheReplies++;
      LOG_SKY_T("LX200 %s RA/Dec poll answered from cache/API while queued/slewing; age=%lu ms ok=%d",
                srcName, (unsigned long)ageMs, ok ? 1 : 0);
    }

    lx200SuppressNextReplyLog = true;
    if (cmd == ":GR") lx200Send(source, lx200Ra(ok ? apiRa : 0.0));
    else lx200Send(source, lx200Dec(ok ? apiDec : 0.0));
    lx200SuppressNextReplyLog = false;

    return;
  }

  if (cmd == ":GA" || cmd == ":GZ") {
    // Application position polling uses the cached mount-position API only.
    // No direct Z command is sent to the mount from this client path.
    double apiAlt = 0.0;
    double apiAz = 0.0;
    unsigned long ageMs = 0;
    bool ok = mountPositionApiAltAz(apiAlt, apiAz, &ageMs);
    if (!ok) {
      LOG_SKY_W("LX200 %s Alt/Az requested but mount-position API cache is not valid; returning safe defaults", srcName);
    }
    if (queuedGotoOrSlewActive()) {
      queuedGotoPositionCacheReplies++;
      LOG_SKY_T("LX200 %s Alt/Az poll answered from cache/API while queued/slewing; age=%lu ms ok=%d",
                srcName, (unsigned long)ageMs, ok ? 1 : 0);
    }

    lx200SuppressNextReplyLog = true;
    if (cmd == ":GA") lx200Send(source, lx200Alt(ok ? apiAlt : 0.0));
    else lx200Send(source, lx200Az(ok ? apiAz : 0.0));
    lx200SuppressNextReplyLog = false;
    return;
  }

  if (cmd == ":Gt") {
    lx200Send(source, siteValid ? lx200SiteLat() : "+00*00#");
    return;
  }

  if (cmd == ":Gg") {
    lx200Send(source, siteValid ? lx200SiteLong() : "+000*00#");
    return;
  }

  if (cmd == ":GL") {
    lx200Send(source, lx200Time());
    return;
  }

  if (cmd == ":GC") {
    lx200Send(source, lx200Date());
    return;
  }

  if (cmd == ":GG") {
    lx200Send(source, lx200UtcOffset());
    return;
  }

  if (cmd.startsWith(":St")) {
    siteLatitudeDeg = parseSignedDegMin(cmd.substring(3));
    siteValid = true;
    markLocationSource("SkySafari");
    computeAltAzFromRaDec();
    savePersistentSettings();
    LOG_TIME_I("SkySafari/LX200 set latitude %.6f source=SkySafari", siteLatitudeDeg);
    lx200Send(source, "1");
    return;
  }

  if (cmd.startsWith(":Sg")) {
    // LX200/Meade longitudes are west-positive; convert to internal east-positive.
    siteLongitudeDeg = -parseSignedDegMin(cmd.substring(3));
    siteValid = true;
    markLocationSource("SkySafari");
    computeAltAzFromRaDec();
    savePersistentSettings();
    LOG_TIME_I("SkySafari/LX200 set longitude %.6f east-positive source=SkySafari", siteLongitudeDeg);
    lx200Send(source, "1");
    return;
  }

  if (cmd.startsWith(":SG")) {
    String raw = cmd.substring(3);
    // LX200 :SG is hours added to local time to get UTC.
    utcOffsetMinutes = -raw.toInt() * 60;
    utcOffsetSource = "SkySafari/LX200";
    timeValid = true;
    markTimeSource(TIME_SRC_SKYSAFARI);
    computeAltAzFromRaDec();
    savePersistentSettings();
    LOG_TIME_I("SkySafari/LX200 set UTC offset minutes %d source=SkySafari", utcOffsetMinutes);
    lx200Send(source, "1");
    return;
  }

  if (cmd.startsWith(":SL")) {
    String raw = cmd.substring(3);
    int c1 = raw.indexOf(':');
    int c2 = raw.indexOf(':', c1 + 1);
    if (c1 > 0 && c2 > c1) {
      localHour = raw.substring(0, c1).toInt();
      localMinute = raw.substring(c1 + 1, c2).toInt();
      localSecond = raw.substring(c2 + 1).toInt();
      timeSetMillis = millis();
      timeValid = true;
      markTimeSource(TIME_SRC_SKYSAFARI);
      computeAltAzFromRaDec();
      savePersistentSettings();
      LOG_TIME_I("SkySafari/LX200 set local time %02d:%02d:%02d source=SkySafari", localHour, localMinute, localSecond);
      lx200Send(source, "1");
    } else {
      lx200Send(source, "0");
    }
    return;
  }

  if (cmd.startsWith(":SC")) {
    String raw = cmd.substring(3);
    int s1 = raw.indexOf('/');
    int s2 = raw.indexOf('/', s1 + 1);
    if (s1 > 0 && s2 > s1) {
      localMonth = raw.substring(0, s1).toInt();
      localDay = raw.substring(s1 + 1, s2).toInt();
      int yy = raw.substring(s2 + 1).toInt();
      localYear = yy < 80 ? 2000 + yy : 1900 + yy;
      timeSetMillis = millis();
      timeValid = true;
      markTimeSource(TIME_SRC_SKYSAFARI);
      computeAltAzFromRaDec();
      savePersistentSettings();
      LOG_TIME_I("SkySafari/LX200 set date %04d-%02d-%02d source=SkySafari", localYear, localMonth, localDay);
      lx200Send(source, "1");
    } else {
      lx200Send(source, "0");
    }
    return;
  }

  if (cmd.startsWith(":Sr") || cmd.startsWith(":sr")) {
    if (source == LX_SRC_BT) lx200BtGotoStageCommands++;
    lx200TargetRA_deg = parseLX200RA(cmd);
    LOG_SKY_I("LX200 %s target RA staged %.6f deg", srcName, lx200TargetRA_deg);
    lx200Send(source, "1");
    return;
  }

  if (cmd.startsWith(":Sd") || cmd.startsWith(":sd")) {
    if (source == LX_SRC_BT) lx200BtGotoStageCommands++;
    lx200TargetDec_deg = parseLX200Dec(cmd);
    LOG_SKY_I("LX200 %s target Dec staged %.6f deg", srcName, lx200TargetDec_deg);
    lx200Send(source, "1");
    return;
  }

  if (isLX200GotoCommand(cmd)) {
    if (source == LX_SRC_BT) lx200BtGotoStageCommands++;
    LOG_SKY_I("LX200 %s slew command %s to RA=%.6f Dec=%.6f; reply 0 now; command will queue if mount is busy",
              srcName, cmd.c_str(), lx200TargetRA_deg, lx200TargetDec_deg);

    markLX200GotoUiStarted(source, "LX200 :MS#/:MA# accepted");

    if (mountCommandPathBusy()) {
      enqueueGotoRaDec(lx200TargetRA_deg, lx200TargetDec_deg, source, "LX200 GOTO while mount busy");
    } else {
      clearPendingReadRequestsForLX200Goto(srcName);
      queueAsyncSlew(lx200TargetRA_deg, lx200TargetDec_deg);
    }

    // LX200/SkySafari expects an immediate response. We accept the command here;
    // if the mount remains busy too long, serviceGotoQueue() drops it after timeout.
    lx200Send(source, "0");
    markGotoQueueImmediateAck(srcName, hasQueuedGoto() ? "LX200 GOTO accepted into queue" : "LX200 GOTO accepted for async start");
    return;
  }

  if (cmd == ":CM") {
    lx200Send(source, "Coordinates matched.#");
    return;
  }

  if (source == LX_SRC_BT) {
    lx200BtUnhandledCommands++;
    lx200BtLastUnhandled = cmd;
  } else {
    lx200WifiUnhandledCommands++;
  }
  LOG_SKY_W("Unsupported LX200 %s command ignored #%lu: %s#",
       srcName,
       (unsigned long)(source == LX_SRC_BT ? lx200BtUnhandledCommands : lx200WifiUnhandledCommands),
       cmd.c_str());
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

void handleStellariumServer() {
  if (!stellariumClient || !stellariumClient.connected()) {
    if (stellariumClientConnected) {
      LOG_STEL_I("Stellarium client disconnected");
      stellariumClientConnected = false;
    }

    WiFiClient newClient = stellariumServer.available();
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
    uint16_t len = readLE16(stellariumRxBuf);

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

void handleLX200Server() {
  if (!lx200Client || !lx200Client.connected()) {
    if (lx200WifiClientConnected) {
      LOGI("LX200/SkySafari WiFi client disconnected");
      lx200WifiClientConnected = false;
    }

    WiFiClient newClient = lx200Server.available();
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

  for (uint8_t i = 0; i < 8 && routeLX200SourceThroughCommonMountCore(LX_SRC_WIFI); i++) {
    yield();
  }
}

void handleBluetoothLX200() {
#if HAS_CLASSIC_BT
  if (!bluetoothRuntimeEnabled) return;
  bool nowConnected = SerialBT.hasClient();

  if (nowConnected && !lx200BtClientConnected) {
    LOG_BT_I("Bluetooth LX200/SkySafari client connected");
    LOG_BT_D("Bluetooth SkySafari connected: idle background mount polling remains enabled; SkySafari polls use latest cache");
    LOG_BT_I("Bluetooth client active; WiFi/Telnet will remain enabled unless wifi off is requested");
    btClientConnectedAtMs = millis();
    btAutoWifiOffDone = false;
    lx200BtLastRxMs = 0;
    lx200BtLastTxMs = 0;
    lx200BtLastCommand = "";
    lx200BtLastReply = "";
    lx200BtLastUnhandled = "";
    lx200BtGotoStageCommands = 0;
    lx200BtPollOnlyHintCount = 0;
    backgroundPollFailCount = 0;
    lastMountPollMs = 0;
  } else if (!nowConnected && lx200BtClientConnected) {
    LOG_BT_I("Bluetooth LX200/SkySafari client disconnected");
    btClientConnectedAtMs = 0;
  }

  lx200BtClientConnected = nowConnected;
  if (!nowConnected) return;

  // Match the stable Full WiFi LX200 behavior: process one complete command
  // per loop pass through the same shared LX200-to-mount command core used by WiFi.
  routeLX200SourceThroughCommonMountCore(LX_SRC_BT);
#endif
}

void serviceMountPolling() {
  // Top-priority independent mount poller:
  // - Runs from loop() before lower-priority network/UI/console services.
  // - Timing is controlled only by pollIntervalMs and a fixed next-due scheduler.
  // - Not gated by WiFi clients, Bluetooth clients, SkySafari recent traffic,
  //   status page refreshes, post-GOTO fast windows, or protocol activity.
  // - Still respects the original NexStar single-command rule by never polling
  //   while a mount transaction, slew, GOTO completion watch, or async command is active.
  unsigned long activePollMs = effectiveMountPollIntervalMs();
  if (activePollMs == 0) {
    nextMountPollDueMs = 0;
    return;
  }

  if (!timeValid) {
    nextMountPollDueMs = 0;
    return;
  }

  unsigned long nowMs = millis();
  if (nextMountPollDueMs == 0) {
    nextMountPollDueMs = nowMs + activePollMs;
    return;
  }

  if ((long)(nowMs - nextMountPollDueMs) < 0) return;

  if (mountBusy || asyncSlewRunning || asyncSlewPending || asyncAltAzSlewPending ||
      asyncNudgePending || asyncRaDecReadPending || asyncAltAzReadPending ||
      gotoCompletionWatchActive || gotoCompletionVerifyPending) {
    mountPollsDeferredBusy++;
    return;
  }

  lastPollSchedulerLatencyMs = nowMs - nextMountPollDueMs;
  if (lastPollSchedulerLatencyMs > maxPollSchedulerLatencyMs) maxPollSchedulerLatencyMs = lastPollSchedulerLatencyMs;
  if (lastPollSchedulerLatencyMs > activePollMs) mountPollsMissedDeadline++;
  mountPollsStarted++;

  // Background polling reads RA/Dec only; Alt/Az is computed locally from the
  // current site/time to reduce NexStar serial traffic.
  LOG_MOUNT_D("Top-priority mount poll due: interval=%lu ms latency=%lu ms; querying actual mount RA/Dec with E",
              activePollMs, lastPollSchedulerLatencyMs);

  double ra = 0.0;
  double dec = 0.0;

  bool gotRaDec = getRaDec(ra, dec, true);
  unsigned long finishMs = millis();
  lastMountPollMs = finishMs;
  if (gotRaDec) {
    markSuccessfulMountPoll(finishMs);
    computeAltAzFromRaDec();
  }

  // Keep the poll cadence tied to the fixed scheduler instead of drifting by
  // loop workload or mount command duration. If the code falls behind, skip to
  // the next future slot instead of doing a burst of back-to-back E/Z queries.
  nextMountPollDueMs += activePollMs;
  while ((long)(finishMs - nextMountPollDueMs) >= 0) {
    nextMountPollDueMs += activePollMs;
    mountPollsMissedDeadline++;
  }

  if (gotRaDec) {
    backgroundPollFailCount = 0;
  } else {
    backgroundPollFailCount++;
    if (backgroundPollFailCount < BACKGROUND_POLL_FAIL_LIMIT) {
      LOG_MOUNT_D("Top-priority mount poll failed count=%lu; polling remains enabled",
                  (unsigned long)backgroundPollFailCount);
    } else {
      LOG_MOUNT_W("Top-priority mount poll failed count=%lu; polling remains enabled",
                  (unsigned long)backgroundPollFailCount);
    }
  }
}

String jsonEscape(const String &s) {
  String out = "";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

String htmlEscape(const String &s) {
  String out = "";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') out += "&quot;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '&') out += "&amp;";
    else out += c;
  }
  return out;
}

bool webTraceEnabled() {
  return LOG_LEVEL >= LOG_TRACE && (LOG_SUBSYSTEM_MASK & LOG_CAT_WEB);
}

const char* currentRouteForLog() {
  if (currentWebRequestPath && currentWebRequestPath[0]) return currentWebRequestPath;
  return "";
}

bool isAlpacaPathForLogging() {
  String p = server.uri();
  return p.startsWith("/management") || p.startsWith("/api/");
}

void traceWebResponse(const char* type, int statusCode, unsigned int bodyLen) {
  if (!webTraceEnabled()) return;
  if (isAlpacaPathForLogging()) return; // prevent Alpaca debug/trace from crashing/flooding ESP8266

  LOG_WEB_T("HTTP response %s status=%d path=%s bytes=%u",
            type,
            statusCode,
            currentRouteForLog(),
            bodyLen);
}

void sendNoCacheHeaders() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
}

void sendJson(const String &body) {
  traceWebResponse("json", 200, (unsigned)body.length());
  sendNoCacheHeaders();
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", body);
}

void sendPlain(const String &body) {
  traceWebResponse("plain", 200, (unsigned)body.length());
  sendNoCacheHeaders();
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", body);
}

void sendAjaxOK(const String &message) {
  String body = "{\"ok\":true,\"message\":\"" + jsonEscape(message) + "\"}";
  if (webTraceEnabled()) LOG_WEB_T("AJAX OK path=%s message=%s", currentRouteForLog(), message.c_str());
  sendJson(body);
}

void sendAjaxRedirectOK(const String &message, const String &redirectUrl, unsigned long delayMs) {
  String body = "{\"ok\":true,\"message\":\"" + jsonEscape(message) + "\",\"redirectUrl\":\"" + jsonEscape(redirectUrl) + "\",\"redirectDelayMs\":" + String(delayMs) + "}";
  if (webTraceEnabled()) LOG_WEB_T("AJAX REDIRECT path=%s message=%s url=%s delay=%lu", currentRouteForLog(), message.c_str(), redirectUrl.c_str(), delayMs);
  sendJson(body);
}

void sendAjaxFail(const String &message) {
  String body = "{\"ok\":false,\"message\":\"" + jsonEscape(message) + "\"}";
  if (LOG_SUBSYSTEM_MASK & LOG_CAT_WEB) LOG_WEB_D("AJAX FAIL path=%s message=%s", currentRouteForLog(), message.c_str());
  sendJson(body);
}

bool wantsAjax() {
  return server.hasArg("ajax") && server.arg("ajax") == "1";
}

int clientTransactionID() {
  if (server.hasArg("ClientTransactionID")) return server.arg("ClientTransactionID").toInt();
  if (server.hasArg("ClientID")) return server.arg("ClientID").toInt();
  return 0;
}

String alpacaResponseValue(const String &valueJson, int errorNumber = 0, const String &errorMessage = "") {
  serverTransactionID++;
  String s = "{";
  s += "\"ClientTransactionID\":" + String(clientTransactionID()) + ",";
  s += "\"ServerTransactionID\":" + String(serverTransactionID) + ",";
  s += "\"ErrorNumber\":" + String(errorNumber) + ",";
  s += "\"ErrorMessage\":\"" + jsonEscape(errorMessage) + "\"";
  if (valueJson.length() > 0) s += ",\"Value\":" + valueJson;
  s += "}";
  return s;
}

void sendAlpacaValue(const String &valueJson) {
  sendJson(alpacaResponseValue(valueJson));
}

void sendAlpacaOK() {
  sendJson(alpacaResponseValue(""));
}

void sendAlpacaError(int code, const String &message) {
  LOG_ALP_W("Alpaca error %d: %s", code, message.c_str());
  sendJson(alpacaResponseValue("", code, message));
}

String argAny(const char* name) {
  if (server.hasArg(name)) return server.arg(name);
  return "";
}

String argAnyCI(const char* name) {
  String wanted = String(name);
  for (uint8_t i = 0; i < server.args(); i++) {
    String n = server.argName(i);
    if (n.equalsIgnoreCase(wanted)) return server.arg(i);
  }
  return "";
}

String alpacaPutValue(const char* primaryName) {
  String v = argAnyCI(primaryName);
  if (v.length() > 0) return v;
  v = argAnyCI("Value");
  if (v.length() > 0) return v;
  return "";
}

String currentUtcIsoString() {
  int y, mo, d, h, mi, se;
  currentUtcParts(y, mo, d, h, mi, se);
  char buf[28];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d", y, mo, d, h, mi, se);
  return String(buf);
}

bool safeFloatValue(const String &v, double &out) {
  if (v.length() == 0 || v.length() > 32) return false;
  out = v.toFloat();
  return !isnan(out) && !isinf(out);
}

void invalidateComputedAltAz() {
  if (altAzComputed) altAzCacheValid = false;
  altAzComputed = false;
}

String formatUptime() {
  unsigned long seconds = millis() / 1000;
  unsigned long days = seconds / 86400;
  seconds %= 86400;
  unsigned long hours = seconds / 3600;
  seconds %= 3600;
  unsigned long minutes = seconds / 60;
  seconds %= 60;

  String s = "";
  if (days) s += String(days) + "d ";
  s += String(hours) + "h ";
  s += String(minutes) + "m ";
  s += String(seconds) + "s";
  return s;
}

String resetReasonText() {
#if defined(ESP8266)
  return ESP.getResetReason();
#else
  esp_reset_reason_t r = esp_reset_reason();
  switch (r) {
    case ESP_RST_POWERON: return "Power on";
    case ESP_RST_EXT: return "External reset";
    case ESP_RST_SW: return "Software reset";
    case ESP_RST_PANIC: return "Panic/exception";
    case ESP_RST_INT_WDT: return "Interrupt watchdog";
    case ESP_RST_TASK_WDT: return "Task watchdog";
    case ESP_RST_WDT: return "Other watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
    case ESP_RST_BROWNOUT: return "Brownout";
    case ESP_RST_SDIO: return "SDIO reset";
    default: return "Unknown";
  }
#endif
}

String twoDigits(int v) {
  return (v < 10 ? "0" : "") + String(v);
}

String formatDurationHoursMinutes(unsigned long spanMs) {
  unsigned long totalMinutes = spanMs / 60000UL;
  unsigned long hours = totalMinutes / 60UL;
  unsigned long minutes = totalMinutes % 60UL;
  return String(hours) + ":" + twoDigits(minutes);
}

String formatLastSuccessfulMountPollTime() {
  if (lastSuccessfulMountPollMs == 0) return "none";
  String s = String(lastSuccessfulMountPollYear) + "-" + twoDigits(lastSuccessfulMountPollMonth) + "-" + twoDigits(lastSuccessfulMountPollDay);
  s += " " + twoDigits(lastSuccessfulMountPollHour) + ":" + twoDigits(lastSuccessfulMountPollMinute) + ":" + twoDigits(lastSuccessfulMountPollSecond);
  String tz = currentTimezoneAbbrev();
  if (tz.length()) s += " " + tz;
  return s;
}

String formatSuccessfulMountPollSpan() {
  if (firstSuccessfulMountPollMs == 0 || lastSuccessfulMountPollMs == 0) return "0:00";
  return formatDurationHoursMinutes(lastSuccessfulMountPollMs - firstSuccessfulMountPollMs);
}

String formatMountUptime() {
  return formatSuccessfulMountPollSpan();
}

void markSuccessfulMountPoll(unsigned long pollMs) {
  mountPollsSucceeded++;
  if (firstSuccessfulMountPollMs == 0) firstSuccessfulMountPollMs = pollMs;
  lastSuccessfulMountPollMs = pollMs;
  currentLocalParts(lastSuccessfulMountPollYear, lastSuccessfulMountPollMonth, lastSuccessfulMountPollDay,
                    lastSuccessfulMountPollHour, lastSuccessfulMountPollMinute, lastSuccessfulMountPollSecond);
}

String formatHmsFromHours(double hours, int decimals = 1) {
  while (hours < 0.0) hours += 24.0;
  while (hours >= 24.0) hours -= 24.0;

  int h = (int)floor(hours);
  double rem = (hours - h) * 60.0;
  int m = (int)floor(rem);
  double sec = (rem - m) * 60.0;

  if (sec >= 59.95 && decimals == 1) {
    sec = 0.0;
    m++;
  }
  if (m >= 60) {
    m = 0;
    h = (h + 1) % 24;
  }

  String s = twoDigits(h) + ":" + twoDigits(m) + ":";
  if (decimals <= 0) s += twoDigits((int)round(sec));
  else {
    if (sec < 10.0) s += "0";
    s += String(sec, decimals);
  }
  return s;
}

String observerTimeText() {
  String s;
  s += "TIME\n";
  s += "Offset: " + String(utcOffsetMinutes) + " min";
  String tz = currentTimezoneAbbrev();
  if (tz.length()) s += " " + tz;
  s += "\n";

  if (!timeValid) {
    s += "Local: not set\n";
    s += "UTC: not set\n";
    s += "Sidereal: unavailable\n";
    return s;
  }

  int ly, lmo, ld, lh, lmi, ls;
  int uy, umo, ud, uh, umi, us;
  currentLocalParts(ly, lmo, ld, lh, lmi, ls);
  currentUtcParts(uy, umo, ud, uh, umi, us);

  double lstH = normalizeRA(gmstDegrees() + siteLongitudeDeg) / 15.0;

  s += "Local: " + String(ly) + "-" + twoDigits(lmo) + "-" + twoDigits(ld) + " ";
  s += twoDigits(lh) + ":" + twoDigits(lmi) + ":" + twoDigits(ls);
  if (tz.length()) s += " " + tz;
  s += "\n";
  s += "UTC: " + String(uy) + "-" + twoDigits(umo) + "-" + twoDigits(ud) + "T";
  s += twoDigits(uh) + ":" + twoDigits(umi) + ":" + twoDigits(us) + "Z\n";
  s += "Sidereal: " + formatHmsFromHours(lstH, 1) + " LST\n";
  return s;
}

String basicStatusText() {
  String s;
  s += "MOUNT\n";
  s += "State: " + mountHealthLine() + "\n";
  s += "Operation: ";
  if (mountBusy || asyncSlewRunning || asyncSlewPending) s += "Slewing / waiting for completion";
  else if (hasQueuedGoto()) s += "GOTO queued";
  else s += "Idle";
  s += "\n";
  s += "Last response: " + String(lastMountResponseMs == 0 ? 0 : millis() - lastMountResponseMs) + " ms ago\n";
  s += "Mount uptime: " + formatMountUptime() + " h:mm\n";
  s += "Current polling: " + String(effectiveMountPollIntervalMs()) + " ms (" + String(positionDemandActive() ? "active" : "idle") + ")\n";
  s += "Idle poll interval: " + String(idlePollIntervalMs) + " ms\n";
  if (mountCommFault && lastMountFault.length()) s += "Fault: " + lastMountFault + "\n";
  s += "\nPOSITION\n";
  s += "RA / Dec: ";
  if (mountCurrentRaDecValid) s += String(mountCurrentRA_deg / 15.0, 6) + " h / " + String(mountCurrentDec_deg, 6) + " deg";
  else s += "unavailable";
  s += "\nAlt / Az: ";
  if (mountCurrentAltAzValid) s += String(mountCurrentAlt_deg, 6) + " deg / " + String(mountCurrentAz_deg, 6) + " deg";
  else s += "unavailable";
  s += "\nPosition source: " + String(altAzComputed ? "computed" : "mount/cache") + "\n";
  s += "\n";
  s += observerTimeText();
  s += "\nLOCATION\n";
  s += "Status/source: " + String(siteValid ? "valid / " : "not valid / ") + currentLocationSource + "\n";
  s += "Latitude: " + String(siteLatitudeDeg, 6) + " deg\n";
  s += "Longitude: " + String(siteLongitudeDeg, 6) + " deg east-positive\n";
  s += "Elevation: " + String(siteElevationMeters, 2) + " m\n";
  return s;
}

String taskStatsSectionText(bool basicMode);

String basicSystemHealthText() {
  String s;
  s += "SCHEDULER\n";
  s += "Active poll interval: " + String(pollIntervalMs) + " ms\n";
  s += "Idle poll interval: " + String(idlePollIntervalMs) + " ms\n";
  s += "Effective poll interval: " + String(effectiveMountPollIntervalMs()) + " ms\n";
  s += "Position demand: " + String(positionDemandActive() ? "active" : "idle") + "\n";
  s += "Poll latency current/max: " + String(lastPollSchedulerLatencyMs) + "/" + String(maxPollSchedulerLatencyMs) + " ms\n";
  s += "Missed poll deadlines: " + String(mountPollsMissedDeadline) + "\n";
  s += "Mount uptime: " + formatMountUptime() + " h:mm\n";
  s += "Loop latency current/max: " + String(lastLoopLatencyMs) + "/" + String(maxLoopLatencyMs) + " ms\n";
  s += "\nRELIABILITY\n";
  s += "Poll failures: " + String(backgroundPollFailCount) + "\n";
  s += "Polling auto-disabled: " + String(backgroundPollingAutoDisabled ? "yes" : "no") + "\n";
  s += "Free heap: " + String(ESP.getFreeHeap()) + " bytes\n";
  s += "Uptime: " + formatUptime() + "\n";
  s += "\n";
  s += taskStatsSectionText(true);
  s += "\nNETWORK\n";
  s += "Mode: " + wifiModeText + " / " + String(bridgeModeName()) + "\n";
  s += "STA: " + String(staConnected && WiFi.status() == WL_CONNECTED ? "connected" : "not connected") + "\n";
  return s;
}

String systemHealthText() {
  String s;
  s += "=== POLL SCHEDULER ===\n";
  s += "Policy: fixed interval, highest loop priority\n";
  s += "Interval: " + String(pollIntervalMs) + " ms\n";
  s += "Scheduler latency current/max: " + String(lastPollSchedulerLatencyMs) + "/" + String(maxPollSchedulerLatencyMs) + " ms\n";
  s += "Polls started: " + String(mountPollsStarted) + "\n";
  s += "Mount uptime: " + formatMountUptime() + " h:mm\n";
  s += "Deferred while command active: " + String(mountPollsDeferredBusy) + "\n";
  s += "Missed deadlines: " + String(mountPollsMissedDeadline) + "\n";
  s += "Background poll failures: " + String(backgroundPollFailCount) + "\n";
  s += "Polling auto-disabled: " + String(backgroundPollingAutoDisabled ? "yes" : "no") + "\n\n";
  s += "=== LOOP PERFORMANCE ===\n";
  s += "Loop latency current/max: " + String(lastLoopLatencyMs) + "/" + String(maxLoopLatencyMs) + " ms\n";
  s += "Loop execution current/max: " + String(lastLoopTimeMs) + "/" + String(maxLoopTimeMs) + " ms\n";
  s += "Uptime: " + formatUptime() + "\n\n";
  s += taskStatsSectionText(false);
  s += "\n";
  s += "=== MOUNT TRANSACTIONS ===\n";
  s += "Single-command discipline: enabled\n";
  s += "Position cache replies/misses: " + String(positionApiCacheReplies) + "/" + String(positionApiCacheMisses) + "\n";
  s += "Cached replies while queued/slewing: " + String(queuedGotoPositionCacheReplies) + "\n";
  s += "LX200 common-router commands: " + String(lx200CommonRouterCommands) + "\n";
  s += "LX200 GOTO started/completed/stop requests: " + String((unsigned long)lx200GotoUiStartedCount) + "/" + String((unsigned long)lx200GotoUiCompletedCount) + "/" + String((unsigned long)lx200GotoUiStopRequests) + "\n\n";
  s += "=== GOTO QUEUE ===\n";
  s += "Active/type: " + String(hasQueuedGoto() ? "yes / " : "no / ") + queuedGotoTypeName(queuedGotoType) + "\n";
  s += "Accepted/started/timed out/replaced: " + String(gotoQueueAccepted) + "/" + String(gotoQueueStarted) + "/" + String(gotoQueueTimedOut) + "/" + String(gotoQueueReplaced) + "\n";
  s += "Nudge queue requests: " + String(nudgeGotoQueueRequests) + "\n";
  s += "Immediate caller ACKs: " + String(gotoQueueImmediateAcks) + "\n";
  s += "Configured/effective timeout: " + String(gotoQueueTimeoutMs) + "/" + String(effectiveGotoQueueTimeoutMs()) + " ms\n\n";
  s += "=== MEMORY AND PLATFORM ===\n";
  s += "Board: " + String(BOARD_NAME) + "\n";
  s += "CPU: " + String(ESP.getCpuFreqMHz()) + " MHz\n";
  s += "Free heap: " + String(ESP.getFreeHeap()) + " bytes\n";
#if defined(ESP32)
  s += "Minimum free heap: " + String(ESP.getMinFreeHeap()) + " bytes\n";
#endif
  s += "Flash/sketch/free sketch: " + String(ESP.getFlashChipSize()) + "/" + String(ESP.getSketchSize()) + "/" + String(ESP.getFreeSketchSpace()) + " bytes\n";
  s += "Reset reason: " + resetReasonText() + "\n\n";
  s += "=== NETWORK ===\n";
  s += "WiFi/bridge mode: " + wifiModeText + " / " + String(bridgeModeName()) + "\n";
  s += "WiFi status: " + lastWifiStatus + "\n";
  s += "STA configured/connected: " + String(staConfigured ? "yes" : "no") + "/" + String(staConnected && WiFi.status() == WL_CONNECTED ? "yes" : "no") + "\n";
  s += "STA SSID/IP: " + staSsid + " / " + WiFi.localIP().toString() + "\n";
  s += "AP SSID/IP/clients: " + apSsid + " / " + WiFi.softAPIP().toString() + " / " + String(WiFi.softAPgetStationNum()) + "\n\n";
  s += "=== CLOCK AND PERSISTENCE ===\n";
  s += "NTP enabled/synced: " + String(ntpEnabled ? "yes" : "no") + "/" + String(ntpSyncValid ? "yes" : "no") + "\n";
  s += "NTP last-sync age: " + String(lastNtpSyncMs == 0 ? 0 : millis() - lastNtpSyncMs) + " ms\n";
  s += "Persistent saves: " + String(persistentSaveCount) + "\n";
  s += "Last save age: " + String(lastPersistentSaveMs == 0 ? 0 : millis() - lastPersistentSaveMs) + " ms\n";
  s += gpioStartupModeText();
  return s;
}

String mountHealthLine() {
  if (mountCommFault) return "Mount Not Responding";
  if (mountBusy || asyncSlewRunning || asyncSlewPending) return "Mount Busy / Slewing";
  if (lastMountResponseMs > 0) return "Mount Connected";
  return "Mount Unknown";
}

String currentStateText() {
  String s;
  s += "=== MOUNT AND MOTION ===\n";
  s += "Mount state: " + mountHealthLine() + "\n";
  s += "Last response age: " + String(mountLastResponseAge()) + " ms\n";
  if (mountCommFault) s += "Last fault: " + lastMountFault + "\n";
  s += "Command active: " + String(mountBusy ? "yes" : "no") + "\n";
  s += "Slew pending/running: " + String((asyncSlewPending || asyncAltAzSlewPending || asyncSlewRunning) ? "yes" : "no") + "\n";
  s += "GOTO queued: " + String(hasQueuedGoto() ? queuedGotoTypeName(queuedGotoType) : "no") + "\n";
  s += "SkySafari GOTO state: " + String(lx200GotoUiActive ? "active" : "idle") + "\n\n";
  s += "=== POSITION ===\n";
  s += "RA: " + String(cachedRA_deg / 15.0, 6) + " h (" + String(cachedRA_deg, 6) + " deg)\n";
  s += "Dec: " + String(cachedDec_deg, 6) + " deg\n";
  s += "Alt: " + String(cachedAlt_deg, 6) + " deg\n";
  s += "Az: " + String(cachedAz_deg, 6) + " deg\n";
  s += "RA/Dec cache valid: " + String(cacheValid ? "yes" : "no") + "\n";
  s += "Alt/Az cache valid: " + String(altAzCacheValid ? "yes" : "no") + "\n";
  s += "Alt/Az source: " + String(altAzComputed ? "computed" : "mount/cache") + "\n\n";
  s += "=== TIME AND SITE ===\n";
  s += "Time valid/source: " + String(timeValid ? "yes / " : "no / ") + String(timeSourceName(currentTimeSource)) + "\n";
  s += "Location valid/source: " + String(siteValid ? "yes / " : "no / ") + currentLocationSource + "\n";
  s += "Latitude: " + String(siteLatitudeDeg, 6) + " deg\n";
  s += "Longitude: " + String(siteLongitudeDeg, 6) + " deg east-positive\n";
  s += "Elevation: " + String(siteElevationMeters, 2) + " m\n";
  s += observerTimeText();
  s += "\n";
  s += "=== CLIENTS AND SERVICES ===\n";
  s += "Bridge mode: " + String(bridgeModeName()) + "\n";
  s += "Alpaca client active: " + String(alpacaConnected ? "yes" : "no") + "\n";
  s += "SkySafari/LX200 WiFi: " + String((lx200Client && lx200Client.connected()) ? "connected" : "idle") + "\n";
  s += "Stellarium: " + String((bridgeMode == BRIDGE_MODE_WIFI_FULL && stellariumClientConnected) ? "connected" : "idle") + "\n";
  s += "Bluetooth enabled: " + String(bluetoothRuntimeEnabled ? "yes" : "no") + "\n";
#if HAS_CLASSIC_BT
  s += "Bluetooth client: " + String((bluetoothRuntimeEnabled && SerialBT.hasClient()) ? "connected" : "idle") + "\n";
#endif
  s += "Web HTTP port: " + String(HTTP_WEB_PORT) + "\n";
  s += "Alpaca/LX200/Discovery ports: " + String(bridgeMode == BRIDGE_MODE_WIFI_FULL ? ALPACA_PORT : 0) + "/" + String(bridgeMode == BRIDGE_MODE_WIFI_FULL ? LX200_PORT : 0) + "/" + String(bridgeMode == BRIDGE_MODE_WIFI_FULL ? ALPACA_DISCOVERY_PORT : 0) + "\n\n";
  s += "=== CONTROL SETTINGS ===\n";
  s += "Active poll interval: " + String(pollIntervalMs) + " ms\n";
  s += "Idle poll interval: " + String(idlePollIntervalMs) + " ms\n";
  s += "Effective poll interval: " + String(effectiveMountPollIntervalMs()) + " ms (" + String(positionDemandActive() ? "active" : "idle") + ")\n";
  s += "Client throttle: " + String(minClientPollIntervalMs) + " ms\n";
  s += "Selected nudge rate: " + String(webSelectedRateIndex + 1) + " (" + String(nudgeRateDeg[webSelectedRateIndex], 3) + " deg)\n";
  s += "Log level: " + String(logLevelName(LOG_LEVEL)) + "\n";
  return s;
}

String getLogText() {
  String out;
  int count = logWrapped ? LOG_BUFFER_LINES : logWriteIndex;
  if (count <= 0) return "";

  // Newest first. Filtering happens at write time with lightweight subsystem masks.
  for (int i = 0; i < count; i++) {
    int idx = logWriteIndex - 1 - i;
    while (idx < 0) idx += LOG_BUFFER_LINES;
    out += logBuffer[idx];
    out += "\n";
    yield();
  }

  return out;
}



const char* timeSourceName(TimeSource src) {
  switch (src) {
    case TIME_SRC_MANUAL: return "Manual";
    case TIME_SRC_SKYSAFARI: return "SkySafari";
    case TIME_SRC_WEB: return "Web";
    case TIME_SRC_NTP: return "NTP";
    default: return "None";
  }
}

void markTimeSource(TimeSource src) {
  currentTimeSource = src;
}

void markLocationSource(const String &src) {
  currentLocationSource = src;
}

bool syncTimeFromNTP(bool forceLog = true) {
  if (forceLog) {
    LOG_TIME_I("NTP sync requested: enabled=%d staConnected=%d wifiStatus=%d server1=%s server2=%s tz=%s",
               ntpEnabled ? 1 : 0,
               (staConnected && WiFi.status() == WL_CONNECTED) ? 1 : 0,
               (int)WiFi.status(),
               ntpServer1,
               ntpServer2,
               tzRule);
  }

  if (!ntpEnabled) {
    if (forceLog) LOG_TIME_W("NTP sync skipped: NTP is disabled");
    return false;
  }

  if (!staConnected || WiFi.status() != WL_CONNECTED) {
    if (forceLog) LOG_TIME_W("NTP sync skipped: STA WiFi is not connected");
    return false;
  }

  if (forceLog) LOG_TIME_D("NTP configTzTime starting");
  configTzTime(tzRule, ntpServer1, ntpServer2);

  struct tm tminfo;
  if (forceLog) LOG_TIME_T("NTP getLocalTime waiting up to 10000 ms");
  bool ok = getLocalTime(&tminfo, 10000);
  if (!ok) {
    ntpSyncValid = false;
    if (forceLog) LOG_TIME_W("NTP sync failed: getLocalTime timed out or returned false");
    return false;
  }

  localYear = tminfo.tm_year + 1900;
  localMonth = tminfo.tm_mon + 1;
  localDay = tminfo.tm_mday;
  localHour = tminfo.tm_hour;
  localMinute = tminfo.tm_min;
  localSecond = tminfo.tm_sec;
  timeSetMillis = millis();
  timeValid = true;
  ntpSyncValid = true;
  lastNtpSyncMs = millis();
  markTimeSource(TIME_SRC_NTP);

  if (forceLog) {
    LOG_TIME_I("NTP sync successful: local=%04d-%02d-%02d %02d:%02d:%02d source=NTP",
               localYear, localMonth, localDay, localHour, localMinute, localSecond);
    LOG_TIME_D("NTP state updated: timeValid=%d ntpSyncValid=%d lastNtpSyncMs=%lu",
               timeValid ? 1 : 0, ntpSyncValid ? 1 : 0, lastNtpSyncMs);
  }

  return true;
}

void serviceNtpSync() {
  static unsigned long lastAttemptMs = 0;
  if (!ntpEnabled || !staConnected || WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();
  if (!timeValid) {
    if (now - lastAttemptMs > 5000) {
      lastAttemptMs = now;
      syncTimeFromNTP(false);
    }
    return;
  }

  if (now - lastNtpSyncMs > 6UL * 3600UL * 1000UL && now - lastAttemptMs > 5000) {
    lastAttemptMs = now;
    syncTimeFromNTP(false);
  }
}
String urlDecodeSimple(String s) {
  s.replace("+", " ");
  String out = "";
  char hex[3] = {0, 0, 0};
  for (unsigned int i = 0; i < s.length(); i++) {
    if (s[i] == '%' && i + 2 < s.length()) {
      hex[0] = s[i + 1];
      hex[1] = s[i + 2];
      out += (char)strtol(hex, NULL, 16);
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

bool loadWiFiConfig() {
  staSsid = "";
  staPass = "";
  staConfigured = false;

#if defined(ESP32)
  Preferences prefs;
  if (!prefs.begin("nexstar", true)) {
    LOG_WIFI_W("NVS WiFi config load failed: prefs.begin read");
    return false;
  }
  staSsid = prefs.getString("staSsid", "");
  staPass = prefs.getString("staPass", "");
  lastStaIp = prefs.getString("lastStaIp", "");
  prefs.end();

  staSsid.trim();
  staPass.trim();

  if (staSsid.length() == 0) {
    LOG_WIFI_I("No NVS WiFi STA config found");
    return false;
  }

  staConfigured = true;
  LOG_WIFI_I("Loaded NVS WiFi STA config for SSID '%s'", staSsid.c_str());
  return true;
#else
  if (!LittleFS.exists(WIFI_FILE)) {
    LOGI("No WiFi STA config file found");
    return false;
  }

  File f = LittleFS.open(WIFI_FILE, "r");
  if (!f) {
    LOGW("Could not open WiFi config file");
    return false;
  }

  staSsid = f.readStringUntil('\n');
  staPass = f.readStringUntil('\n');
  f.close();

  staSsid.trim();
  staPass.trim();

  if (staSsid.length() == 0) {
    LOG_WIFI_W("WiFi config exists but SSID is empty");
    return false;
  }

  staConfigured = true;
  LOG_WIFI_I("Loaded WiFi STA config for SSID '%s'", staSsid.c_str());
  return true;
#endif
}

bool saveWiFiConfig(const String &ssid, const String &pass) {
#if defined(ESP32)
  Preferences prefs;
  if (!prefs.begin("nexstar", false)) {
    LOG_WIFI_E("NVS WiFi config save failed: prefs.begin write");
    return false;
  }

  size_t ssidWritten = prefs.putString("staSsid", ssid);
  size_t passWritten = prefs.putString("staPass", pass);
  String verifySsid = prefs.getString("staSsid", "");
  prefs.end();

  if (ssidWritten == 0 || verifySsid != ssid) {
    LOG_WIFI_E("NVS WiFi config save verification failed: wrote=%u verify='%s' expected='%s'", (unsigned)ssidWritten, verifySsid.c_str(), ssid.c_str());
    return false;
  }

  staSsid = ssid;
  staPass = pass;
  staConfigured = staSsid.length() > 0;

  LOG_WIFI_I("Saved NVS WiFi STA config for SSID '%s' passLen=%u passWritten=%u", staSsid.c_str(), (unsigned)staPass.length(), (unsigned)passWritten);
  return true;
#else
  File f = LittleFS.open(WIFI_FILE, "w");
  if (!f) {
    LOGE("Could not open WiFi config file for write");
    return false;
  }

  f.println(ssid);
  f.println(pass);
  f.close();

  staSsid = ssid;
  staPass = pass;
  staConfigured = staSsid.length() > 0;

  LOG_WIFI_I("Saved WiFi STA config for SSID '%s'", staSsid.c_str());
  return true;
#endif
}

void saveLastStaIp(const String &ip) {
  if (ip.length() == 0 || ip == "0.0.0.0") return;
  lastStaIp = ip;
#if defined(ESP32)
  Preferences prefs;
  if (prefs.begin("nexstar", false)) {
    prefs.putString("lastStaIp", ip);
    prefs.end();
  }
#endif
}

void clearWiFiConfig() {
#if defined(ESP32)
  Preferences prefs;
  if (prefs.begin("nexstar", false)) {
    prefs.remove("staSsid");
    prefs.remove("staPass");
    prefs.end();
  }
#else
  if (LittleFS.exists(WIFI_FILE)) LittleFS.remove(WIFI_FILE);
#endif
  staSsid = "";
  staPass = "";
  staConfigured = false;
  staConnected = false;
  LOG_WIFI_I("WiFi STA config cleared");
}

bool saveBluetoothLiteStaModeFlag(bool disableSta) {
#if defined(ESP32)
  Preferences prefs;
  if (!prefs.begin("nexstar", false)) {
    LOG_WIFI_E("NVS STA mode flag save failed: prefs.begin write");
    return false;
  }
  prefs.putBool("valid", true);
  prefs.putBool("staRunDis", disableSta);
  prefs.end();
  LOG_WIFI_I("Saved BT STA mode flag: staRuntimeDisabled=%d", disableSta ? 1 : 0);
  return true;
#else
  staRuntimeDisabled = disableSta;
  return savePersistentSettings();
#endif
}

bool saveBluetoothLiteApOnlySettings() {
#if defined(ESP32)
  Preferences prefs;
  if (!prefs.begin("nexstar", false)) {
    LOG_WIFI_E("NVS AP settings save failed: prefs.begin write");
    return false;
  }
  prefs.putBool("valid", true);
  prefs.putString("apSsid", apSsid);
  prefs.putString("apPass", apPass);
  prefs.putString("apIp", apIp);
  prefs.putBool("staRunDis", true);
  prefs.end();
  LOG_WIFI_I("Saved BT AP-only settings: apSsid=%s apIp=%s", apSsid.c_str(), apIp.c_str());
  return true;
#else
  staRuntimeDisabled = true;
  return savePersistentSettings();
#endif
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
#if HAS_CLASSIC_BT
    if (bluetoothRuntimeEnabled) {
      SerialBT.disconnect();
      delay(100);
      SerialBT.end();
      delay(250);
    }
#endif
    delay(100);
    ESP.restart();
  }
}

void runtimeWifiOff() {
#if defined(ESP32)
  tinyWebServerRuntimeEnabled = false;
  wifiRuntimeEnabled = false;

  if (telnetClient) telnetClient.stop();
  telnetClientConnected = false;
  telnetAuthenticated = false;
  telnetLine = "";
  telnetLastWasCR = false;

  // Drop clients and radio state. This is runtime-only and does not erase saved STA/AP settings.
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
  tinyWebServerRuntimeEnabled = btLiteBootWebEnabled;

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

bool readStartupModePinActive(int pin) {
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
    tinyWebServerRuntimeEnabled = true;
    startupModeSource = "GPIO force Full WiFi";
    startupModePinUsed = GPIO_STARTUP_FULL_WIFI_PIN;
    Serial.printf("[BOOT] GPIO startup override: GPIO%d active -> Full WiFi web mode\n", startupModePinUsed);
    return;
  }

  if (readStartupModePinActive(GPIO_STARTUP_BT_WEB_PIN)) {
    bridgeMode = BRIDGE_MODE_BT_MIN_WEB;
    btLiteBootWebEnabled = true;
    tinyWebServerRuntimeEnabled = true;
    startupModeSource = "GPIO force BT + web";
    startupModePinUsed = GPIO_STARTUP_BT_WEB_PIN;
    Serial.printf("[BOOT] GPIO startup override: GPIO%d active -> BT + tiny web\n", startupModePinUsed);
    return;
  }

  if (readStartupModePinActive(GPIO_STARTUP_BT_TELNET_PIN)) {
    bridgeMode = BRIDGE_MODE_BT_MIN_WEB;
    btLiteBootWebEnabled = false;
    tinyWebServerRuntimeEnabled = false;
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

bool loadBridgeMode() {
#if defined(ESP32)
  Preferences prefs;
  if (!prefs.begin("nexstar", true)) return false;
  uint8_t m = prefs.getUChar("bridgeMode", BRIDGE_MODE_WIFI_FULL);
  prefs.end();
  bridgeMode = (m == BRIDGE_MODE_BT_MIN_WEB) ? BRIDGE_MODE_BT_MIN_WEB : BRIDGE_MODE_WIFI_FULL;
  return true;
#else
  bridgeMode = BRIDGE_MODE_WIFI_FULL;
  return true;
#endif
}

bool saveBridgeMode(uint8_t mode) {
  bridgeMode = (mode == BRIDGE_MODE_BT_MIN_WEB) ? BRIDGE_MODE_BT_MIN_WEB : BRIDGE_MODE_WIFI_FULL;
#if defined(ESP32)
  Preferences prefs;
  if (!prefs.begin("nexstar", false)) return false;
  prefs.putUChar("bridgeMode", bridgeMode);
  prefs.end();
#endif
  return true;
}

bool saveBluetoothLiteWebBootEnabled(bool enabled) {
  btLiteBootWebEnabled = enabled;
  tinyWebServerRuntimeEnabled = enabled;
#if defined(ESP32)
  Preferences prefs;
  if (!prefs.begin("nexstar", false)) {
    LOG_SET_E("NVS save FAILED: btWebBoot prefs.begin write");
    return false;
  }
  prefs.putBool("valid", true);
  prefs.putBool("btWebBoot", btLiteBootWebEnabled);
  prefs.end();
  LOG_SET_I("BT boot web server saved: %s", btLiteBootWebEnabled ? "enabled" : "disabled");
#endif
  return true;
}

bool saveBluetoothLitePollInterval(unsigned long ms) {
#if defined(ESP32)
  Preferences prefs;
  if (!prefs.begin("nexstar", false)) {
    LOG_SET_E("NVS save FAILED: btPollMs prefs.begin write");
    return false;
  }
  prefs.putUInt("btPollMs", ms);
  prefs.end();
  LOG_SET_I("BT mount poll interval saved separately: %lu ms", ms);
  return true;
#else
  // ESP8266/non-NVS fallback: runtime only, do not alter the Full WiFi poll setting file.
  return true;
#endif
}

bool loadBluetoothLitePollInterval() {
#if defined(ESP32)
  Preferences prefs;
  if (!prefs.begin("nexstar", true)) {
    LOG_SET_W("NVS load skipped: btPollMs prefs.begin read failed");
    return false;
  }
  bool exists = prefs.isKey("btPollMs");
  unsigned long v = prefs.getUInt("btPollMs", pollIntervalMs);
  prefs.end();

  if (!exists) {
    LOG_SET_I("No separate BT mount poll interval saved; using current default %lu ms", pollIntervalMs);
    return false;
  }

  if (v > 60000) v = 60000;
  pollIntervalMs = v;
  nextMountPollDueMs = 0;
  LOG_SET_I("BT mount poll interval loaded separately: %lu ms", pollIntervalMs);
  return true;
#else
  return false;
#endif
}

void handleSetModePage() {
  String m = webServer->arg("mode");
  m.toLowerCase();
  String ret = requestedReturnUrl();
  if (ret.length() == 0 || !ret.startsWith("http://")) ret = currentRequestBaseUrl();

  if (m == "wifi") {
    saveBridgeMode(BRIDGE_MODE_WIFI_FULL);
    String page;
    page.reserve(900);
    page += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
    page += F("<meta http-equiv='refresh' content='7;url=");
    page += ret;
    page += F("'><script>setTimeout(function(){location.replace('");
    page += ret;
    page += F("')},7000)</script></head><body><h3>Switched to Full WiFi web mode</h3><p>Rebooting...</p><p><a href='");
    page += ret;
    page += F("'>Open previous page</a></p></body></html>");
    webServer->sendHeader("Cache-Control", "no-store");
    webServer->send(200, "text/html", page);
    delay(900);
    ESP.restart();
    return;
  }

  if (m == "bt") {
    saveBridgeMode(BRIDGE_MODE_BT_MIN_WEB);
    String page;
    page.reserve(900);
    page += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
    page += F("<meta http-equiv='refresh' content='7;url=");
    page += ret;
    page += F("'><script>setTimeout(function(){location.replace('");
    page += ret;
    page += F("')},7000)</script></head><body><h3>Switched to BT mode</h3><p>Rebooting...</p><p>Returning to the same host that called this page.</p><p><a href='");
    page += ret;
    page += F("'>Open previous page</a></p></body></html>");
    webServer->sendHeader("Cache-Control", "no-store");
    webServer->send(200, "text/html", page);
    delay(900);
    ESP.restart();
    return;
  }

  webServer->send(400, "text/plain", "Unknown mode");
}

void tinySetupSendHtml(WiFiClient &c, const String &body) {
  c.print("HTTP/1.1 200 OK\r\n");
  c.print("Content-Type: text/html; charset=utf-8\r\n");
  c.print("Cache-Control: no-store\r\n");
  c.print("Connection: close\r\n\r\n");
  c.print(body);
}

void tinySetupSendJson(WiFiClient &c, const String &body) {
  c.print("HTTP/1.1 200 OK\r\n");
  c.print("Content-Type: application/json\r\n");
  c.print("Cache-Control: no-store\r\n");
  c.print("Connection: close\r\n\r\n");
  c.print(body);
}

void tinySetupSendRedirect(WiFiClient &c, const String &where) {
  c.print("HTTP/1.1 303 See Other\r\n");
  c.print("Location: ");
  c.print(where);
  c.print("\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
}


String tinyUrlDecode(String s) {
  String out;
  out.reserve(s.length());
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < (int)s.length()) {
      char h1 = s[i + 1];
      char h2 = s[i + 2];
      auto hexVal = [](char h)->int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
      };
      int v1 = hexVal(h1);
      int v2 = hexVal(h2);
      if (v1 >= 0 && v2 >= 0) {
        out += char((v1 << 4) | v2);
        i += 2;
      } else {
        out += c;
      }
    } else {
      out += c;
    }
  }
  return out;
}

String tinyQueryParam(const String &req, const String &name) {
  int q = req.indexOf('?');
  if (q < 0) return "";
  int sp = req.indexOf(' ', q);
  String qs = (sp > q) ? req.substring(q + 1, sp) : req.substring(q + 1);
  String key = name + "=";
  int start = 0;
  while (start < (int)qs.length()) {
    int amp = qs.indexOf('&', start);
    if (amp < 0) amp = qs.length();
    String part = qs.substring(start, amp);
    if (part.startsWith(key)) return tinyUrlDecode(part.substring(key.length()));
    start = amp + 1;
  }
  return "";
}

String tinyEsc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += F("&amp;");
    else if (c == '<') o += F("&lt;");
    else if (c == '>') o += F("&gt;");
    else if (c == '"') o += F("&quot;");
    else if (c == '\'') o += F("&#39;");
    else o += c;
  }
  return o;
}

String sampleWebCpuLoadText();

String tinyRequestHost(const String &req) {
  int h = req.indexOf("\nHost:");
  if (h < 0) h = req.indexOf("\nhost:");
  if (h < 0) return "";
  int p = req.indexOf(':', h + 1);
  if (p < 0) return "";
  int e = req.indexOf('\n', p + 1);
  if (e < 0) e = req.length();
  String host = req.substring(p + 1, e);
  host.trim();
  return host;
}

String tinyDefaultReturnUrl(const String &req) {
  String host = tinyRequestHost(req);
  if (host.length() > 0) return String("http://") + host + "/";
  if (WiFi.status() == WL_CONNECTED) return String("http://") + WiFi.localIP().toString() + "/";
  if (apRunning) return String("http://") + WiFi.softAPIP().toString() + "/";
  return "/";
}

String tinyReturnUrlFromRequest(const String &req) {
  String ret = tinyQueryParam(req, "return");
  ret.trim();
  if (ret.startsWith("http://")) return ret;
  return tinyDefaultReturnUrl(req);
}

String tinyRebootRedirectPage(const String &title, const String &ret) {
  String page;
  page.reserve(900);
  page += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<meta http-equiv='refresh' content='8;url=");
  page += ret;
  page += F("'><script>setTimeout(function(){location.replace('");
  page += ret;
  page += F("')},8000)</script>");
  page += F("<style>body{font-family:Arial,Helvetica,sans-serif;background:#0b1020;color:#edf2ff;margin:0;padding:20px}.card{max-width:560px;margin:25px auto;background:#151c2f;border:1px solid #2b3654;border-radius:12px;padding:18px}a{color:#8cc7ff}</style>");
  page += F("</head><body><div class='card'><h2>");
  page += title;
  page += F("</h2><p>Rebooting. The browser should return to the root menu automatically.</p><p><a href='");
  page += ret;
  page += F("'>Open root menu</a></p></div></body></html>");
  return page;
}

String tinyBtStatusJson() {
  bool btConnected = false;
#if HAS_CLASSIC_BT
  btConnected = bluetoothRuntimeEnabled && SerialBT.hasClient();
#endif
  String json;
  json.reserve(1300);
  json += "{";
  json += "\"bridgeMode\":\"" + jsonEscape(String(bridgeModeName())) + "\",";
  json += "\"btName\":\"" + jsonEscape(runtimeBtName()) + "\",";
  json += "\"apIp\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"staIp\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"staConnected\":" + String((staConnected && WiFi.status() == WL_CONNECTED) ? "true" : "false") + ",";
  json += "\"bluetoothEnabled\":" + String(bluetoothRuntimeEnabled ? "true" : "false") + ",";
  json += "\"bluetoothConnected\":" + String(btConnected ? "true" : "false") + ",";
  json += "\"lx200BtRxCommands\":" + String(lx200BtRxCommands) + ",";
  json += "\"lx200BtTxReplies\":" + String(lx200BtTxReplies) + ",";
  json += "\"lx200BtLastCommand\":\"" + jsonEscape(lx200BtLastCommand) + "\",";
  json += "\"lx200BtLastReply\":\"" + jsonEscape(lx200BtLastReply) + "\",";
  json += "\"lx200BtLastUnhandled\":\"" + jsonEscape(lx200BtLastUnhandled) + "\",";
  json += "\"mountCommFault\":" + String(mountCommFault ? "true" : "false") + ",";
  json += "\"mountBusy\":" + String(mountBusy ? "true" : "false") + ",";
  json += "\"mountUptime\":\"" + jsonEscape(formatMountUptime()) + "\",";
  json += "\"cpuLoad\":\"" + jsonEscape(sampleWebCpuLoadText()) + "\",";
  json += "\"freeHeap\":" + String(ESP.getFreeHeap());
  json += "}";
  return json;
}

void appendTinyBanner(String &page) {
  int y, mo, d, h, mi, se;
  currentLocalParts(y, mo, d, h, mi, se);
  page += F("<div class='card wide'><div class='v'>Time ");
  page += String(y) + "-" + twoDigits(mo) + "-" + twoDigits(d) + " " + twoDigits(h) + ":" + twoDigits(mi) + ":" + twoDigits(se);
  page += F("<br>Lat ");
  page += String(siteLatitudeDeg, 6);
  page += F(" / Lon ");
  page += String(siteLongitudeDeg, 6);
  page += F("<br>");
  page += sampleWebCpuLoadText();
  page += F("</div></div>");
}


String tinyStatusPage() {
  String page;
  page.reserve(9000);

  bool btConnected = false;
#if HAS_CLASSIC_BT
  btConnected = bluetoothRuntimeEnabled && SerialBT.hasClient();
#endif

  page += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>");
  page += FW_NAME;
  page += F(" ");
  page += FW_VERSION;
  page += F(" BT Status</title>");
  page += F("<style>");
  page += F(":root{--bg:#0b1020;--panel:#151c2f;--panel2:#101729;--text:#edf2ff;--muted:#a9b4cf;--line:#2b3654;--accent:#4aa3ff;--bad:#ff8d8d;--ok:#8dffba}");
  page += F("*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font-family:Arial,Helvetica,sans-serif;font-size:16px}");
  page += F(".top{position:sticky;top:0;z-index:2;background:#080d1a;border-bottom:1px solid var(--line);padding:10px 14px;display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap}.topActions{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.brand{white-space:nowrap}");
  page += F(".brand{font-size:18px;font-weight:700}.brand small{display:block;font-size:12px;color:var(--muted);font-weight:400;margin-top:2px}.pill{border:1px solid var(--line);border-radius:999px;padding:5px 10px;color:var(--muted);background:var(--panel2);font-size:13px}");
  page += F(".wrap{max-width:980px;margin:0 auto;padding:14px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:14px}.card{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:14px;box-shadow:0 1px 3px #0006}.wide{grid-column:1/-1}");
  page += F("h2{font-size:18px;margin:0 0 12px 0}p{color:var(--muted);line-height:1.35}.kv{display:grid;grid-template-columns:190px 1fr;gap:7px 12px;align-items:center}.k{color:var(--muted)}.v{font-family:Consolas,monospace;word-break:break-word}");
  page += F(".actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:12px}.btn,button{display:inline-flex;align-items:center;justify-content:center;border:1px solid var(--line);background:#1b2742;color:var(--text);border-radius:9px;padding:10px 12px;text-decoration:none;font:inherit;font-weight:700;cursor:pointer;line-height:1.2}.topActions .btn,.topActions button{min-width:76px;min-height:42px}.btn.primary,button.primary{background:#174b80;border-color:#2b72b9}");
  page += F("pre{white-space:pre-wrap;word-break:break-word;margin:0;background:#0a1122;border:1px solid var(--line);border-radius:9px;padding:12px;color:var(--text);font-family:Consolas,monospace;font-size:13px;line-height:1.35}");
  page += F("@media(max-width:760px){.grid{grid-template-columns:1fr}.kv{grid-template-columns:1fr}.top{align-items:flex-start}.brand{font-size:16px}}");
  page += F("body.themeLight{--bg:#f3f6fb;--panel:#ffffff;--panel2:#e9eef7;--text:#172033;--muted:#56627a;--line:#b9c5d8;--accent:#1769aa;--bad:#a00000;--ok:#08752f}.themeLight .top{background:#e9eef7}.themeLight .btn,.themeLight button{background:#e4eaf4;color:#172033}.themeLight .btn.primary,.themeLight button.primary{background:#d4e7f7;border-color:#1769aa}.themeLight pre,.themeLight input{background:#f7f9fc;color:#172033}.themeNight{--bg:#050000;--panel:#110000;--panel2:#190000;--text:#ff4a4a;--muted:#d42d2d;--line:#650000;--accent:#ff3030;--bad:#ff6060;--ok:#ff4040}.themeNight .top{background:#080000}.themeNight .card,.themeNight pre,.themeNight input{background:#100000!important;color:#ff4a4a!important}.themeNight .btn,.themeNight button{background:#260000!important;color:#ff4a4a!important;border-color:#750000!important}.themeNight .btn.primary,.themeNight button.primary{background:#520000!important;border-color:#a00000!important}.themeNight .btn.warn{background:#3a0000!important;border-color:#8a0000!important}.nightBtn.active{background:#7a0000!important;color:#ffb0b0!important;border-color:#ff3030!important}.statusModeBtn.active{border-color:var(--accent)!important}");
  page += F(".themeNight input[type=checkbox]{accent-color:#ff2020!important}.themeLight .themeDark select{color-scheme:dark!important}.themeDark select option{background:#0a1122!important;color:#edf2ff!important}.themeDark select option:disabled{background:#101729!important;color:#7f899f!important}.themeDark select option:checked{background:#174b80!important;color:#fff!important}input[type=checkbox]{accent-color:#1769aa}.themeDark input[type=checkbox]{accent-color:#4aa3ff}</style><script>let baseTheme=localStorage.getItem('baseTheme')||'dark',nightMode=localStorage.getItem('nightMode')==='1',statusAdvanced=localStorage.getItem('statusAdvanced')==='1';function applyTinyTheme(){document.body.classList.remove('themeLight','themeDark','themeNight');document.body.classList.add(nightMode?'themeNight':(baseTheme==='light'?'themeLight':'themeDark'));let b=document.getElementById('nightModeBtn');if(b){b.classList.toggle('active',nightMode);b.textContent='Red'}let lb=document.getElementById('themeLightBtn'),db=document.getElementById('themeDarkBtn');if(lb)lb.classList.toggle('active',baseTheme==='light');if(db)db.classList.toggle('active',baseTheme==='dark')}function setBaseTheme(t){baseTheme=(t==='light')?'light':'dark';nightMode=false;localStorage.setItem('baseTheme',baseTheme);localStorage.setItem('nightMode','0');applyTinyTheme()}function toggleTinyNight(){nightMode=!nightMode;localStorage.setItem('nightMode',nightMode?'1':'0');applyTinyTheme()}function applyTinyStatusMode(){let b=document.getElementById('tinyBasic'),a=document.getElementById('tinyAdvanced'),x=document.getElementById('tinyStatusModeBtn');if(b)b.style.display=statusAdvanced?'none':'block';if(a)a.style.display=statusAdvanced?'block':'none';if(x){x.textContent=statusAdvanced?'Basic':'Advanced';x.classList.toggle('active',statusAdvanced)}}function toggleTinyStatusMode(){statusAdvanced=!statusAdvanced;localStorage.setItem('statusAdvanced',statusAdvanced?'1':'0');applyTinyStatusMode()}document.addEventListener('DOMContentLoaded',function(){applyTinyTheme();applyTinyStatusMode()});</script></head><body>");

  page += F("<div class='top'><div class='brand'>");
  page += FW_NAME;
  page += F(" ");
  page += FW_VERSION;
  page += F("</div><div class='topActions'>");
  page += F("<button id='nightModeBtn' class='btn warn nightBtn' type='button' onclick='toggleTinyNight()'>Red</button><button id='themeLightBtn' class='btn' type='button' onclick=\"setBaseTheme('light')\">Light</button><button id='themeDarkBtn' class='btn' type='button' onclick=\"setBaseTheme('dark')\">Dark</button>");
  page += F("<a class='btn warn' href='#' onclick=\"location.href='/reboot?return='+encodeURIComponent('/status');return false\">Reboot</a>");
  page += F("<a class='btn primary' href='/mode?radio=wifi&return=/status'>WiFi</a>");
  page += F("</div></div>");

  page += F("<div class='wrap'>");
  page += F("<div class='grid'>");
  appendTinyBanner(page);

  page += F("<!-- BT_MINI_STATUS_PAGE_v5.15 -->");
  page += F("<div class='actions wide' style='justify-content:space-between;align-items:center;margin-bottom:0'><button id='tinyStatusModeBtn' class='statusModeBtn' type='button' onclick='toggleTinyStatusMode()'>Advanced</button><a class='btn primary' href='/?page=status&refresh=1'>Refresh</a></div>");
  page += F("<div id='tinyBasic' class='wide'><div class='grid'>");
  page += F("<div class='card'><h2>Observer Status</h2><pre>");
  page += tinyEsc(basicStatusText());
  page += F("</pre></div><div class='card'><h2>System Health</h2><pre>");
  page += tinyEsc(basicSystemHealthText());
  page += F("</pre></div></div></div>");
  page += F("<div id='tinyAdvanced' class='wide' style='display:none'><div class='grid'>");
  page += F("<div class='card'><h2>Observer Status - Advanced</h2><pre>");
  page += tinyEsc(currentStateText());
  page += F("</pre></div><div class='card'><h2>System Health - Advanced</h2><pre>");
  page += tinyEsc(systemHealthText());
  page += F("</pre></div></div></div>");

  page += F("</div></div></body></html>");
  return page;
}

String tinySetupPage() {
  String page;
  page.reserve(9000);

  page += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>");
  page += FW_NAME;
  page += F(" ");
  page += FW_VERSION;
  page += F(" BT</title>");
  page += F("<style>");
  page += F(":root{--bg:#0b1020;--panel:#151c2f;--panel2:#101729;--text:#edf2ff;--muted:#a9b4cf;--line:#2b3654;--accent:#4aa3ff;--bad:#ff8d8d;--ok:#8dffba}");
  page += F("*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font-family:Arial,Helvetica,sans-serif;font-size:16px}");
  page += F(".top{position:sticky;top:0;z-index:2;background:#080d1a;border-bottom:1px solid var(--line);padding:10px 14px;display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap}.topActions{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.brand{white-space:nowrap}");
  page += F(".brand{font-size:18px;font-weight:700}.brand small{display:block;font-size:12px;color:var(--muted);font-weight:400;margin-top:2px}.pill{border:1px solid var(--line);border-radius:999px;padding:5px 10px;color:var(--muted);background:var(--panel2);font-size:13px}");
  page += F(".wrap{max-width:920px;margin:0 auto;padding:14px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:14px}.card{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:14px;box-shadow:0 1px 3px #0006}.wide{grid-column:1/-1}");
  page += F("h2{font-size:18px;margin:0 0 12px 0}p{color:var(--muted);line-height:1.35}.kv{display:grid;grid-template-columns:150px 1fr;gap:7px 12px;align-items:center}.k{color:var(--muted)}.v{font-family:Consolas,monospace;word-break:break-word}pre{white-space:pre-wrap;word-break:break-word;background:#090f1d;border:1px solid var(--line);border-radius:9px;padding:10px;margin:0;font-family:Consolas,monospace;font-size:13px;line-height:1.35}");
  page += F("form{margin:0}.row{display:grid;grid-template-columns:150px minmax(0,1fr);gap:10px 12px;align-items:center;margin:9px 0}label{color:var(--muted);font-weight:600;text-align:left}");
  page += F("input{width:100%;min-width:0;border:1px solid var(--line);background:#0a1122;color:var(--text);border-radius:8px;padding:9px 10px;font-size:15px}input:focus{outline:2px solid var(--accent);border-color:var(--accent)}");
  page += F(".hint{font-size:13px;color:var(--muted);margin:8px 0 0 162px}.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:12px}.btn,button{display:inline-flex;align-items:center;justify-content:center;border:1px solid var(--line);background:#1b2742;color:var(--text);border-radius:9px;padding:10px 12px;text-decoration:none;font:inherit;font-weight:700;cursor:pointer;line-height:1.2}.topActions .btn,.topActions button{min-width:76px;min-height:42px}.btn.primary,button.primary{background:#174b80;border-color:#2b72b9}.btn.warn{background:#4b2630;border-color:#88485a}");
  page += F("@media(max-width:760px){.grid{grid-template-columns:1fr}.row,.kv{grid-template-columns:1fr}label{text-align:left}.hint{margin-left:0}.top{align-items:flex-start}.brand{font-size:16px}}");
  page += F("body.themeLight{--bg:#f3f6fb;--panel:#ffffff;--panel2:#e9eef7;--text:#172033;--muted:#56627a;--line:#b9c5d8;--accent:#1769aa;--bad:#a00000;--ok:#08752f}.themeLight .top{background:#e9eef7}.themeLight .btn,.themeLight button{background:#e4eaf4;color:#172033}.themeLight .btn.primary,.themeLight button.primary{background:#d4e7f7;border-color:#1769aa}.themeLight pre,.themeLight input{background:#f7f9fc;color:#172033}.themeNight{--bg:#050000;--panel:#110000;--panel2:#190000;--text:#ff4a4a;--muted:#d42d2d;--line:#650000;--accent:#ff3030;--bad:#ff6060;--ok:#ff4040}.themeNight .top{background:#080000}.themeNight .card,.themeNight pre,.themeNight input{background:#100000!important;color:#ff4a4a!important}.themeNight .btn,.themeNight button{background:#260000!important;color:#ff4a4a!important;border-color:#750000!important}.themeNight .btn.primary,.themeNight button.primary{background:#520000!important;border-color:#a00000!important}.themeNight .btn.warn{background:#3a0000!important;border-color:#8a0000!important}.nightBtn.active{background:#7a0000!important;color:#ffb0b0!important;border-color:#ff3030!important}.statusModeBtn.active{border-color:var(--accent)!important}");
  page += F(".themeNight input[type=checkbox]{accent-color:#ff2020!important}.themeLight input[type=checkbox]{accent-color:#1769aa}.themeDark input[type=checkbox]{accent-color:#4aa3ff}</style><script>let baseTheme=localStorage.getItem('baseTheme')||'dark',nightMode=localStorage.getItem('nightMode')==='1',statusAdvanced=localStorage.getItem('statusAdvanced')==='1';function applyTinyTheme(){document.body.classList.remove('themeLight','themeDark','themeNight');document.body.classList.add(nightMode?'themeNight':(baseTheme==='light'?'themeLight':'themeDark'));let b=document.getElementById('nightModeBtn');if(b){b.classList.toggle('active',nightMode);b.textContent='Red'}let lb=document.getElementById('themeLightBtn'),db=document.getElementById('themeDarkBtn');if(lb)lb.classList.toggle('active',baseTheme==='light');if(db)db.classList.toggle('active',baseTheme==='dark')}function setBaseTheme(t){baseTheme=(t==='light')?'light':'dark';nightMode=false;localStorage.setItem('baseTheme',baseTheme);localStorage.setItem('nightMode','0');applyTinyTheme()}function toggleTinyNight(){nightMode=!nightMode;localStorage.setItem('nightMode',nightMode?'1':'0');applyTinyTheme()}function applyTinyStatusMode(){let b=document.getElementById('tinyBasic'),a=document.getElementById('tinyAdvanced'),x=document.getElementById('tinyStatusModeBtn');if(b)b.style.display=statusAdvanced?'none':'block';if(a)a.style.display=statusAdvanced?'block':'none';if(x){x.textContent=statusAdvanced?'Basic':'Advanced';x.classList.toggle('active',statusAdvanced)}}function toggleTinyStatusMode(){statusAdvanced=!statusAdvanced;localStorage.setItem('statusAdvanced',statusAdvanced?'1':'0');applyTinyStatusMode()}document.addEventListener('DOMContentLoaded',function(){applyTinyTheme();applyTinyStatusMode()});</script></head><body>");

  page += F("<div class='top'><div class='brand'>");
  page += FW_NAME;
  page += F(" ");
  page += FW_VERSION;
  page += F("</div><div class='topActions'>");
  page += F("<button id='nightModeBtn' class='btn warn nightBtn' type='button' onclick='toggleTinyNight()'>Red</button><button id='themeLightBtn' class='btn' type='button' onclick=\"setBaseTheme('light')\">Light</button><button id='themeDarkBtn' class='btn' type='button' onclick=\"setBaseTheme('dark')\">Dark</button>");
  page += F("<a class='btn warn' href='#' onclick=\"location.href='/reboot?return='+encodeURIComponent(location.href);return false\">Reboot</a>");
  page += F("<a class='btn primary' href='/mode?radio=wifi'>WiFi</a>");
  page += F("</div></div>");

  page += F("<div class='wrap'>");
  page += F("<div class='grid'>");
  appendTinyBanner(page);

  page += F("<!-- BT_LITE_MAIN_PAGE_v4.80 -->");
  page += F("<div class='actions wide' style='justify-content:space-between;align-items:center;margin-bottom:0'><button id='tinyStatusModeBtn' class='statusModeBtn' type='button' onclick='toggleTinyStatusMode()'>Advanced</button><a class='btn' href='#' onclick='location.reload();return false'>Refresh</a></div>");
  page += F("<div id='tinyBasic' class='wide'><div class='grid'><div class='card'><h2>Observer Status</h2><pre>");
  page += tinyEsc(basicStatusText());
  page += F("</pre></div><div class='card'><h2>System Health</h2><pre>");
  page += tinyEsc(basicSystemHealthText());
  page += F("</pre></div></div></div>");
  page += F("<div id='tinyAdvanced' class='wide' style='display:none'><div class='grid'><div class='card'><h2>Observer Status - Advanced</h2><pre>");
  page += tinyEsc(currentStateText());
  page += F("</pre></div><div class='card'><h2>System Health - Advanced</h2><pre>");
  page += tinyEsc(systemHealthText());
  page += F("</pre></div></div></div>");

  page += F("</div></div></body></html>");
  return page;
}

void serviceTinySetupServer() {
#if defined(ESP32)
  if (bridgeMode != BRIDGE_MODE_BT_MIN_WEB) return;
  if (!wifiRuntimeEnabled) return;
  if (!tinyWebServerRuntimeEnabled) return;
  WiFiClient c = tinySetupServer.available();
  if (!c) return;

  c.setTimeout(700);
  String req = c.readStringUntil('\r');
  c.readStringUntil('\n');

  while (c.connected()) {
    String h = c.readStringUntil('\n');
    if (h == "\r" || h.length() == 0) break;
  }

  String tinyPath = "/";
  int tinySp1 = req.indexOf(' ');
  int tinySp2 = tinySp1 >= 0 ? req.indexOf(' ', tinySp1 + 1) : -1;
  if (tinySp1 >= 0 && tinySp2 > tinySp1) {
    tinyPath = req.substring(tinySp1 + 1, tinySp2);
  }

  // Some clients/proxies can send an absolute URI in the request line.
  // Convert "http://host/status?x=1" to "/status?x=1" before routing.
  if (tinyPath.startsWith("http://") || tinyPath.startsWith("https://")) {
    int scheme = tinyPath.indexOf("://");
    int slash = scheme >= 0 ? tinyPath.indexOf('/', scheme + 3) : -1;
    tinyPath = slash >= 0 ? tinyPath.substring(slash) : String("/");
  }

  int tinyQ = tinyPath.indexOf('?');
  String tinyPathOnly = tinyQ >= 0 ? tinyPath.substring(0, tinyQ) : tinyPath;
  tinyPathOnly.trim();

  LOG_WEB_D("BT tiny request route: req='%s' path='%s' pathOnly='%s'", req.c_str(), tinyPath.c_str(), tinyPathOnly.c_str());

  String tinyPageMode = tinyQueryParam(req, "page");
  tinyPageMode.toLowerCase();
  if (tinyPageMode == "status") {
    tinySetupSendHtml(c, tinyStatusPage());
    c.stop();
    return;
  }

  if (tinyPathOnly == "/status") {
    tinySetupSendRedirect(c, "/?page=status");
    c.stop();
    return;
  }

  if (tinyPathOnly == "/btstatus") {
    tinySetupSendJson(c, tinyBtStatusJson());
    c.stop();
    return;
  }

  if (tinyPathOnly == "/btpoll") {
    int p = tinyQueryParam(req, "poll").toInt();
    if (p < 0) p = 0;
    if (p > 60000) p = 60000;
    pollIntervalMs = (unsigned long)p;
    nextMountPollDueMs = 0;
    backgroundPollingAutoDisabled = false;
    bool saved = saveBluetoothLitePollInterval(pollIntervalMs);

    tinySetupSendRedirect(c, "/");
    c.stop();
    return;
  }

  if ((tinyPathOnly == "/mode" && tinyPath.indexOf("radio=wifi") >= 0) ||
      (tinyPathOnly == "/setmode" && tinyPath.indexOf("mode=wifi") >= 0)) {
    saveBridgeMode(BRIDGE_MODE_WIFI_FULL);
    String ret = tinyQueryParam(req, "return");
    if (!ret.startsWith("http://")) ret = String("http://") + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "/";
    String page;
    page.reserve(900);
    page += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><meta http-equiv='refresh' content='7;url=");
    page += ret;
    page += F("'><script>setTimeout(function(){location.replace('");
    page += ret;
    page += F("')},7000)</script></head><body><h2>Full WiFi mode saved</h2><p>Rebooting into full WiFi mode...</p><p>The page should return to the same host automatically.</p><p><a href='");
    page += ret;
    page += F("'>Open main UI</a></p></body></html>");
    tinySetupSendHtml(c, page);
    c.stop();
    delay(900);
    ESP.restart();
    return;
  }

  if (tinyPathOnly == "/mode" && tinyPath.indexOf("radio=bt") >= 0) {
    saveBridgeMode(BRIDGE_MODE_BT_MIN_WEB);
    tinySetupSendRedirect(c, "/");
    c.stop();
    return;
  }

  if (tinyPathOnly == "/wifi_save") {
    loadWiFiConfig();
    String target = tinyQueryParam(req, "target");
    String ssid = tinyQueryParam(req, "ssid");
    String pass = tinyQueryParam(req, "pass");

    ssid.trim();

    bool ok = true;
    String msg;
    if (target == "sta") {
      staSsid = ssid;
      staPass = pass;
      staRuntimeDisabled = false;  // STA Setup means boot BT in STA mode.
      staConnected = false;
      msg = "STA settings saved; STA enabled for STA boot";
    } else if (target == "ap") {
      if (ssid.length() == 0) {
        ok = false;
        msg = "AP SSID cannot be blank";
      } else if (pass.length() > 0 && pass.length() < 8) {
        ok = false;
        msg = "AP password must be blank/open or at least 8 characters";
      } else {
        String ip = tinyQueryParam(req, "ip");
        ip.trim();
        IPAddress testIp;
        if (ip.length() == 0) ip = apIp;
        if (!parseIpAddress(ip, testIp)) {
          ok = false;
          msg = "AP IP is invalid";
        } else {
          String suffix = deviceMacSuffix();
          if (suffix.length() > 0 && ssid.endsWith("-" + suffix)) {
            ssid = ssid.substring(0, ssid.length() - suffix.length() - 1);
          }
          apSsid = ssid;
          apPass = pass;
          apIp = ip;
          staRuntimeDisabled = true;   // AP Setup means boot BT in AP-only mode.
          staConnected = false;
          msg = "AP settings saved; STA disabled for AP-only boot";
        }
      }
    } else {
      ok = false;
      msg = "Unknown WiFi settings target";
    }

    bool saveOk = false;
    if (ok) {
      LOG_WIFI_I("BT WiFi save target=%s apSsid=%s apIp=%s staRuntimeDisabled=%d", target.c_str(), apSsid.c_str(), apIp.c_str(), staRuntimeDisabled ? 1 : 0);
      if (target == "sta") {
        saveOk = saveWiFiConfig(staSsid, staPass) && saveBluetoothLiteStaModeFlag(false);
      } else if (target == "ap") {
        saveOk = saveBluetoothLiteApOnlySettings();
      }

      if (!saveOk) {
        String page = F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head><body><h2>Save failed</h2><p>Could not write WiFi settings to storage.</p><p><a href='/'>Back</a></p></body></html>");
        tinySetupSendHtml(c, page);
        c.stop();
        return;
      }

      String page = F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head><body><h2>");
      page += msg;
      if (target == "ap") {
        page += F("</h2><p>Rebooting into BT AP-only mode. Reconnect to the AP SSID shown on the setup page after reboot.</p>");
      } else if (target == "sta") {
        page += F("</h2><p>Rebooting into BT STA mode. Use the router-assigned STA IP after reboot, or fall back to AP if STA cannot connect.</p>");
      } else {
        page += F("</h2><p>Rebooting so BT uses the new saved WiFi configuration...</p>");
      }
      page += F("</body></html>");
      tinySetupSendHtml(c, page);
      c.stop();
      delay(900);
      ESP.restart();
      return;
    } else {
      String page = F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head><body><h2>Save failed</h2><p>");
      page += tinyEsc(msg);
      page += F("</p><p><a href='/'>Back</a></p></body></html>");
      tinySetupSendHtml(c, page);
      c.stop();
      return;
    }
  }

  if (tinyPathOnly == "/reboot") {
    String ret = tinyReturnUrlFromRequest(req);
    tinySetupSendHtml(c, tinyRebootRedirectPage("Rebooting", ret));
    c.stop();
    delay(900);
    ESP.restart();
    return;
  }

  tinySetupSendHtml(c, tinySetupPage());
  c.stop();
#endif
}



void handleSetBtPollPage() {
  logHttpRequest("SET_BT_POLL");
  int p = server.hasArg("poll") ? server.arg("poll").toInt() : (int)pollIntervalMs;
  if (p < 0) p = 0;
  if (p > 60000) p = 60000;
  pollIntervalMs = (unsigned long)p;
    nextMountPollDueMs = 0;
  backgroundPollingAutoDisabled = false;
  bool saved = savePersistentSettings();

  String msg = String("Mount poll interval set to ") + String(pollIntervalMs) + " ms";
  if (!saved) msg += " (save failed)";

  if (wantsAjax()) {
    if (saved) sendAjaxOK(msg);
    else sendAjaxFail(msg);
  } else {
    server.sendHeader("Location", "/");
    server.send(303, "text/plain", msg);
  }
}

void startFallbackAP() {
  if (!apRunning) {
#if defined(ESP32)
    // Make AP the recovery path. Even if STA is later used, AP must remain up.
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
  WiFi.disconnect(false, true);   // reset STA state, keep saved ESP32 SDK config untouched
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

      // v5.09: Internet-backed STA operation should use NTP by default.
      // A previously saved disabled value must not leave time synchronization
      // silently off after a successful STA connection.
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

  // IMPORTANT STABILITY RULE:
  // Never run AP and STA at the same time.
  // If STA is configured and not disabled, try STA-only first.
  // If STA is missing or fails, switch completely to AP-only fallback.
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

  // AP-only fallback. STA is fully off here.
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


// Bright Star Catalog data is stored separately.
#include "bsc5_catalog_data.h"

void handleBsc5DataPage() {
  logHttpRequest("BSC5 data");
  sendNoCacheHeaders();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain", "");
  for (uint16_t i = 0; i < BSC5_CHUNK_COUNT; i++) {
    const char* p = (const char*)pgm_read_ptr(&BSC5_CHUNKS[i]);
#if defined(ESP8266)
    server.sendContent_P(p);
#else
    server.sendContent_P(p);
#endif
    delay(1);
    yield();
  }
  server.sendContent("");
}

void sendWebPage() {
  // Stream the page in chunks instead of building one huge String.
  // This avoids ESP8266 heap fragmentation/crashes when the UI grows.
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  server.sendContent(F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><meta http-equiv='Cache-Control' content='no-store, no-cache, must-revalidate, max-age=0'><meta http-equiv='Pragma' content='no-cache'><meta http-equiv='Expires' content='0'><title>NexStar Converter</title>"));
  server.sendContent(F("<style>body{font-family:Arial,Helvetica,sans-serif;background:#0b1020;color:#edf2ff;margin:0;padding:10px;font-size:var(--ui-scale,16px)}.topbar{position:sticky;top:0;z-index:2;background:#080d1a;border:1px solid #2b3654;border-radius:12px;padding:10px 14px;display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:10px;box-shadow:0 1px 3px #0006}.scaleCtl{margin-left:auto;display:flex;align-items:center;gap:6px;background:#101729;border:1px solid #2b3654;border-radius:10px;padding:4px 6px;white-space:nowrap;flex-wrap:nowrap}.scaleCtl button{min-width:2em;padding:.25em .5em}.scaleCtl span{display:inline-block;min-width:2.7em;text-align:center;font-size:.95em}h2{margin:4px 0 10px}.topbar h2{font-size:18px;margin:0;font-weight:700}h3{margin:4px 0 8px}button,input,select{font-size:1em;margin:.2em;padding:.48em .55em;max-width:100%;box-sizing:border-box}input{width:120px;max-width:95%;border:1px solid #2b3654;background:#0a1122;color:#edf2ff;border-radius:8px}select{border:1px solid #2b3654;background:#0a1122;color:#edf2ff;border-radius:8px;color-scheme:dark;-webkit-appearance:auto;appearance:auto}select option{background:#0a1122;color:#edf2ff}select option:disabled{color:#7f899f!important;background:#101729!important}select option:checked{background:#174b80;color:#fff}input:focus,select:focus{outline:2px solid #4aa3ff;border-color:#4aa3ff}button{border-radius:9px;border:1px solid #2b3654;background:#1b2742;color:#edf2ff;font-weight:700;cursor:pointer}.quick{font-size:.9em;background:#151c2f;border:1px solid #2b3654;border-radius:12px;padding:8px;margin:8px 0;box-shadow:0 1px 3px #0006}.tabs{display:flex;gap:6px;overflow-x:auto;margin:8px 0 12px}.tabs button{white-space:nowrap;padding:8px 11px;background:#1b2742;border-color:#2b3654}.tabs button.active{background:#174b80;color:#edf2ff;font-weight:bold;border-color:#2b72b9}.tab{display:none}.tab.active{display:block}.grid{display:block}.tab .card{margin-bottom:10px}.card{background:#151c2f;padding:14px;border-radius:12px;border:1px solid #2b3654;min-width:0;box-shadow:0 1px 3px #0006}.row{display:flex;flex-wrap:wrap;align-items:center;gap:4px}.formrow{display:grid;grid-template-columns:150px minmax(0,1fr);gap:10px 12px;align-items:center;margin:9px 0}.formrow label{color:#a9b4cf;font-weight:bold;text-align:left}.formrow input,.formrow select{width:100%;max-width:100%;margin:0}.hint{font-size:13px;color:#a9b4cf;margin:8px 0 0 162px;line-height:1.35}.actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}.pad{text-align:center}.pad button{min-width:5.5em;min-height:3.25em;font-size:1.15em}pre{background:#080d1a;color:#8dffba;border:1px solid #2b3654;padding:8px;overflow:auto;white-space:pre-wrap;border-radius:8px;min-height:260px}.small{font-size:13px;color:#a9b4cf}.msg{padding:6px;border-radius:8px;background:#101729;color:#edf2ff;border:1px solid #2b3654;margin:5px 0;min-height:18px}.ok{color:#8dffba}.bad{color:#ff8d8d}.health{font-size:18px;font-weight:bold;padding:8px;border-radius:8px;margin-bottom:6px;background:#101729;border:1px solid #2b3654}.alive{border-color:#8dffba!important}.busy{border-color:#ffcc66!important}.idle{border-color:#2b3654!important}.fault{border-color:#ff0000!important;animation:flash .7s infinite}@keyframes flash{0%{border-color:#ff0000}50%{border-color:#550000}100%{border-color:#ff0000}}@media (max-width:699px){.topbar{flex-wrap:wrap}.scaleCtl{margin-left:0;width:100%;max-width:100%;overflow-x:auto;box-sizing:border-box}.tabs{max-width:100%;box-sizing:border-box}.grid{grid-template-columns:1fr}.card{width:100%;box-sizing:border-box}.row{display:block}.formrow{grid-template-columns:1fr}.formrow label{text-align:left}.hint{margin-left:0}.row button,.row select{display:block;width:100%;margin:6px 0}.pad button{min-width:72px}.msg,pre{max-width:100%;overflow:auto}}@media (min-width:700px){.grid{display:block}.tab.active{display:grid;grid-template-columns:1fr 1fr;gap:10px}.status{grid-column:auto}.logs{grid-column:auto}.tab .card{margin-bottom:0}}@media (min-width:1100px){.tab.active{grid-template-columns:1fr 1fr 1fr}}.logAlert{font-weight:bold;color:#ffcc00;animation:blinkAlert 1s step-start infinite;cursor:pointer}.logAlert.off{display:none}@keyframes blinkAlert{50%{opacity:0}}.catBtns button{margin:4px}.catInfo{font-size:.9em;background:#101729;padding:6px;border-radius:8px;margin:6px 0;display:block;width:100%;box-sizing:border-box;border:1px solid #2b3654;color:#edf2ff;line-height:1.35}.dark .catInfo{background:#101729}.logs pre{height:360px;overflow-y:auto;scroll-behavior:auto}.loggrid{display:grid;grid-template-columns:repeat(3,minmax(150px,1fr));gap:8px 18px;align-items:center;margin:8px 0}.loggrid label{display:grid;grid-template-columns:22px 1fr;align-items:center;column-gap:6px;margin:0;white-space:nowrap}.loggrid input[type=checkbox]{margin:0;width:18px;height:18px;justify-self:start}@media(max-width:520px){.loggrid{grid-template-columns:repeat(2,minmax(130px,1fr))}}.themeLight{background:#f3f5f8!important;color:#172033!important}.themeLight .topbar{background:#fff!important;border-color:#bcc5d2!important;box-shadow:0 1px 4px #0002}.themeLight .scaleCtl,.themeLight .quick,.themeLight .card,.themeLight .msg,.themeLight .health,.themeLight .catInfo{background:#fff!important;color:#172033!important;border-color:#bcc5d2!important;box-shadow:0 1px 3px #0002}.themeLight input,.themeLight select,.themeLight pre{background:#f8fafc!important;color:#172033!important;border-color:#aab4c2!important}.themeLight select{color-scheme:light!important}.themeLight select option{background:#f8fafc!important;color:#172033!important}.themeLight select option:disabled{background:#eef1f5!important;color:#8993a3!important}.themeLight select option:checked{background:#1769aa!important;color:#fff!important}.themeLight button,.themeLight .tabs button,.themeLight .badge{background:#e5eaf1!important;color:#172033!important;border-color:#aab4c2!important}.themeLight .tabs button.active{background:#1769aa!important;color:#fff!important;border-color:#12568c!important}.themeLight .formrow label,.themeLight .hint,.themeLight .small{color:#4b5668!important}.themeLight pre{color:#163b22!important}.themeNight{background:#050000!important;color:#ff3a3a!important}.themeNight *{text-shadow:none!important}.themeNight .topbar{background:#100000!important;border-color:#5a0000!important;box-shadow:none!important}.themeNight .scaleCtl,.themeNight .quick,.themeNight .card,.themeNight .msg,.themeNight .health,.themeNight .catInfo{background:#100000!important;color:#ff3a3a!important;border-color:#5a0000!important;box-shadow:none!important}.themeNight input,.themeNight select,.themeNight pre{background:#050000!important;color:#ff3a3a!important;border-color:#650000!important}.themeNight select{color-scheme:dark!important}.themeNight select option{background:#050000!important;color:#ff3a3a!important}.themeNight select option:disabled{background:#100000!important;color:#7f2020!important}.themeNight select option:checked{background:#5a0000!important;color:#ff7070!important}.themeNight button,.themeNight .tabs button,.themeNight .badge{background:#260000!important;color:#ff4a4a!important;border-color:#750000!important}.themeNight button:hover{background:#3a0000!important}.themeNight .tabs button.active{background:#5a0000!important;color:#ff7070!important;border-color:#a00000!important}.themeNight .formrow label,.themeNight .hint,.themeNight .small,.themeNight .ok,.themeNight .bad{color:#ff3a3a!important}.themeNight .alive,.themeNight .busy,.themeNight .idle,.themeNight .fault{border-color:#8a0000!important}.themeNight .logAlert{color:#ff3a3a!important}.nightBtn.active{background:#7a0000!important;color:#ffb0b0!important;border-color:#ff3030!important}.statusModeBtn.active{border-color:var(--accent)!important}.themeChoice{display:flex;gap:8px;flex-wrap:wrap}.themeChoice button.active{outline:2px solid #4aa3ff}.themeNight .themeChoice button.active{outline-color:#ff3030}.themeLight .themeChoice button.active{outline-color:#1769aa}.statusPair{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;grid-column:1/-1}.statusPair>.card{margin-bottom:0}@media(max-width:760px){.statusPair{grid-template-columns:1fr}}input[type=checkbox]{accent-color:#1769aa}.themeDark input[type=checkbox]{accent-color:#4aa3ff}.themeLight input[type=checkbox]{accent-color:#1769aa}.themeNight input[type=checkbox]{accent-color:#d40000!important;filter:brightness(.9) saturate(1.4)}.themeNight input[type=checkbox]:checked{accent-color:#ff2020!important}</style>"));
  server.sendContent(F("<script>"));
  server.sendContent(F("const FW_VERSION='"));
  server.sendContent(FW_VERSION);
  server.sendContent(F("';let busy=false,uiScale=parseFloat(localStorage.getItem('uiScale')||'1'),baseTheme=localStorage.getItem('baseTheme')||'dark',nightMode=localStorage.getItem('nightMode')==='1',statusAdvanced=localStorage.getItem('statusAdvanced')==='1';function id(x){return document.getElementById(x)}function enc(v){return encodeURIComponent(v)}function cb(u){return u+(u.includes('?')?'&':'?')+'fw='+enc(FW_VERSION)+'&t='+Date.now()}function setMsg(n,t,c){let e=id(n);if(e){e.textContent=t;e.className='msg '+(c||'')}}function applyScale(){document.documentElement.style.setProperty('--ui-scale',(16*uiScale)+'px');let e=id('scalePct');if(e)e.textContent=Math.round(uiScale*100)+'%';localStorage.setItem('uiScale',uiScale.toFixed(2))}function scaleUi(d){uiScale=Math.max(0.7,Math.min(1.6,uiScale+d));applyScale()}function resetScale(){uiScale=1;applyScale()}function applyTheme(){document.body.classList.remove('themeLight','themeDark','themeNight');document.body.classList.add(nightMode?'themeNight':(baseTheme==='light'?'themeLight':'themeDark'));let cs=(!nightMode&&baseTheme==='light')?'light':'dark';document.documentElement.style.colorScheme=cs;document.querySelectorAll('select').forEach(e=>{e.style.colorScheme=cs});let nb=id('nightModeBtn');if(nb){nb.classList.toggle('active',nightMode);nb.textContent='Red'}let lb=id('themeLightBtn'),db=id('themeDarkBtn');if(lb)lb.classList.toggle('active',baseTheme==='light');if(db)db.classList.toggle('active',baseTheme==='dark')}function setBaseTheme(t){baseTheme=(t==='light')?'light':'dark';nightMode=false;localStorage.setItem('baseTheme',baseTheme);localStorage.setItem('nightMode','0');applyTheme()}function toggleNightMode(){nightMode=!nightMode;localStorage.setItem('nightMode',nightMode?'1':'0');applyTheme()}function applyStatusMode(){let a=id('statusAdvancedWrap'),b=id('statusBasicWrap'),btn=id('statusModeBtn');if(a)a.style.display=statusAdvanced?'grid':'none';if(b)b.style.display=statusAdvanced?'none':'grid';if(btn){btn.textContent=statusAdvanced?'Basic':'Advanced';btn.classList.toggle('active',statusAdvanced)}}function toggleStatusMode(){statusAdvanced=!statusAdvanced;localStorage.setItem('statusAdvanced',statusAdvanced?'1':'0');applyStatusMode()}function showTab(n){document.querySelectorAll('.tab').forEach(e=>e.classList.remove('active'));document.querySelectorAll('.tabs button').forEach(e=>e.classList.remove('active'));let t=id('tab_'+n);if(t)t.classList.add('active');let b=id('btn_'+n);if(b)b.classList.add('active');localStorage.setItem('activeTab',n)}function restoreTab(){showTab(localStorage.getItem('activeTab')||'control')}async function getText(u){let r=await fetch(cb(u),{cache:'no-store'});return await r.text()}async function getJson(u){let r=await fetch(cb(u),{cache:'no-store'});let t=await r.text();try{return JSON.parse(t)}catch(e){return {ok:r.ok,message:t||'OK'}}}async function jsonAction(url,msg,onOk,jsName){if(busy)return;busy=true;let el=id(msg);if(el){el.textContent='Working...';el.className='msg'}try{let full=url+(url.includes('?')?'&':'?')+'ajax=1';if(jsName)full+='&js='+enc(jsName);let j=await getJson(full);if(j.ok&&url.startsWith('/set_manual_site_time'))clearSiteDirty();if(j.ok&&url.startsWith('/setrates'))clearRateDirty();if(j.ok&&onOk)onOk();if(el){el.textContent=j.message||'OK';el.className='msg '+(j.ok?'ok':'bad')}if(j.ok&&j.redirectUrl){let d=Number(j.redirectDelayMs||1200);setTimeout(()=>{window.location.href=j.redirectUrl},d);busy=false;return}}catch(e){if(el){el.textContent='Request failed';el.className='msg bad'}}busy=false;setTimeout(updateNow,500)}"));
  server.sendContent(F("function setLog(l){setVal('logLevelSel',l);applyLogSettings()}function clearLogs(){let lb=id('logBox');if(lb)lb.textContent='';jsonAction('/clearlogs','logsMsg',function(){updateNow()},'clearLogs')}function clearLogAlert(){jsonAction('/clearlogalert','logsMsg',function(){updateNow()},'clearLogAlert')}function setRate(){jsonAction('/setwebrate?rate='+enc(id('rateSel').value),'mainMsg',null,'setRate')}function nudge(d){jsonAction('/webnudge?dir='+enc(d),'mainMsg',null,'nudge')}function readRaDec(){jsonAction('/getradecweb','gotoMsg',function(){setTimeout(updateNow,700);setTimeout(function(){updateNow();loadCurrentRaDecTarget()},1600)},'readRaDec')}function readAltAz(){jsonAction('/getaltazweb','altazMsg',function(){setTimeout(updateNow,700);setTimeout(function(){updateNow();loadCurrentAltAzTarget()},1600)},'readAltAz')}function startupReadAltAz(){fetch('/getaltazweb').then(()=>{setTimeout(updateNow,900);setTimeout(function(){if(!targetsInitialized&&lastStatus){setTargetRaDec(lastStatus.raHours,lastStatus.decDeg);setTargetAltAz(lastStatus.altDeg,lastStatus.azDeg);targetsInitialized=true}},1800)}).catch(()=>{})}"));
  server.sendContent(F("function julianFromStatus(){if(!lastStatus)return NaN;let ms=Date.UTC(lastStatus.localYear,lastStatus.localMonth-1,lastStatus.localDay,lastStatus.localHour,lastStatus.localMinute,lastStatus.localSecond)-Number(lastStatus.utcOffsetMinutes||0)*60000;return ms/86400000+2440587.5}function lstHours(){let jd=julianFromStatus();if(!isFinite(jd)||!lastStatus)return NaN;let T=(jd-2451545.0)/36525.0;let gmst=280.46061837+360.98564736629*(jd-2451545.0)+0.000387933*T*T-T*T*T/38710000.0;let lst=(gmst+Number(lastStatus.longitude))/15.0;lst=lst%24;if(lst<0)lst+=24;return lst}function radecToAltDeg(raHours,decDeg){if(!lastStatus||!lastStatus.siteValid||!lastStatus.timeValid)return NaN;let lat=Number(lastStatus.latitude)*Math.PI/180,dec=Number(decDeg)*Math.PI/180,ha=(lstHours()-Number(raHours))*15*Math.PI/180;let s=Math.sin(dec)*Math.sin(lat)+Math.cos(dec)*Math.cos(lat)*Math.cos(ha);return Math.asin(Math.max(-1,Math.min(1,s)))*180/Math.PI}function confirmAboveHorizonRaDec(ra,dec,msg){let alt=radecToAltDeg(ra,dec);if(!isFinite(alt)){setMsg(msg,'Cannot verify horizon: valid site/time required','bad');return false}if(alt<0){setMsg(msg,'Blocked by horizon safety: target Alt '+alt.toFixed(2)+' deg','bad');return false}let cb=id('alt_limit_enable');if(cb&&cb.checked){let mn=Number(id('alt_min').value),mx=Number(id('alt_max').value);if(isFinite(mn)&&isFinite(mx)&&mn<mx&&(alt<mn||alt>mx)){setMsg(msg,'Blocked by Alt limit: '+alt.toFixed(2)+' deg allowed '+mn.toFixed(2)+' to '+mx.toFixed(2),'bad');return false}}return true}function confirmAboveHorizonAltAz(alt,msg){let a=Number(alt);if(!isFinite(a)){setMsg(msg,'Invalid target altitude','bad');return false}if(a<0){setMsg(msg,'Blocked by horizon safety: target Alt '+a.toFixed(2)+' deg','bad');return false}let enabled=false,mn=0,mx=90;let cb=id('alt_limit_enable'),mi=id('alt_min'),ma=id('alt_max');if(cb){enabled=cb.checked;mn=Number(mi?mi.value:0);mx=Number(ma?ma.value:90)}else if(lastStatus){enabled=!!lastStatus.safeAltLimitEnabled;mn=Number(lastStatus.safeAltMinDeg);mx=Number(lastStatus.safeAltMaxDeg)}if(enabled){if(!isFinite(mn)||!isFinite(mx)||mn>=mx){setMsg(msg,'Invalid Alt safety limits','bad');return false}if(a<mn||a>mx){setMsg(msg,'Blocked by Alt limit: '+a.toFixed(2)+' deg allowed '+mn.toFixed(2)+' to '+mx.toFixed(2),'bad');return false}}return true}function confirmSafeDec(dec,msg){return true}function gotoRaDec(){let ra=Number(id('ra_hours').value),dec=Number(id('dec_deg').value);if(!isFinite(ra)||ra<0||ra>=24){setMsg('gotoMsg','RA must be from 0 up to, but not including, 24 hours','bad');return}if(!isFinite(dec)||dec<-90||dec>90){setMsg('gotoMsg','Declination must be from -90 to +90 degrees','bad');return}if(!confirmAboveHorizonRaDec(ra,dec,'gotoMsg'))return;jsonAction('/webgoto_radec?ra_hours='+enc(ra)+'&dec_deg='+enc(dec),'gotoMsg',function(){setTimeout(updateNow,1200)},'gotoRaDec')}function gotoAltAz(){let alt=Number(id('alt_deg').value),az=Number(id('az_deg').value);if(!isFinite(alt)||alt<-90||alt>90){setMsg('altazMsg','Altitude must be from -90 to +90 degrees','bad');return}if(!isFinite(az)||az<0||az>=360){setMsg('altazMsg','Azimuth must be from 0 up to, but not including, 360 degrees','bad');return}if(!confirmAboveHorizonAltAz(alt,'altazMsg'))return;jsonAction('/webgoto_altaz?alt_deg='+enc(alt)+'&az_deg='+enc(az),'altazMsg',function(){setTimeout(updateNow,1200)},'gotoAltAz')}"));
  server.sendContent(F("function saveAltLimits(){let en=id('alt_limit_enable').checked?1:0;jsonAction('/setaltlimits?enabled='+en+'&min='+enc(id('alt_min').value)+'&max='+enc(id('alt_max').value),'altLimitMsg',function(){clearFieldsDirty(['alt_limit_enable','alt_min','alt_max']);setTimeout(updateNow,300)},'saveAltLimits')}function saveDecLimits(){let en=id('dec_limit_enable').checked?1:0;jsonAction('/setdeclimits?enabled='+en+'&min='+enc(id('dec_min').value)+'&max='+enc(id('dec_max').value),'decLimitMsg',function(){clearFieldsDirty(['dec_limit_enable','dec_min','dec_max']);setTimeout(updateNow,300)},'saveDecLimits')}function saveRates(){jsonAction('/setrates?r1='+enc(id('r1').value)+'&r2='+enc(id('r2').value)+'&r3='+enc(id('r3').value)+'&r4='+enc(id('r4').value),'settingsMsg',null,'saveRates')}function resetRates(){jsonAction('/resetrates','settingsMsg',null,'resetRates')}"));
  server.sendContent(F("function saveManualSiteTime(){jsonAction('/set_manual_site_time?lat='+enc(id('lat').value)+'&lon='+enc(id('lon').value)+'&offset='+enc(id('offset').value)+'&year='+enc(id('year').value)+'&month='+enc(id('month').value)+'&day='+enc(id('day').value)+'&hour='+enc(id('hour').value)+'&minute='+enc(id('minute').value)+'&second='+enc(id('second').value)+'&poll='+enc(id('poll').value)+'&idlepoll='+enc(id('idlepoll').value)+'&throttle='+enc(id('throttle').value),'siteMsg',null,'saveManualSiteTime')}function startHttpsTimeLocation(){setMsg('siteMsg','Starting GPS Sync HTTPS server...','ok');let ret=location.href;fetch(cb('/start_https?json=1&return='+enc(ret)),{cache:'no-store'}).then(r=>r.text()).then(t=>{let j=null;try{j=JSON.parse(t)}catch(e){throw new Error('bad GPS Sync response: '+t.slice(0,80))}if(j.ok&&j.url){setMsg('siteMsg','Opening GPS Sync HTTPS page. Accept the certificate warning if shown.','ok');setTimeout(()=>{location.href=j.url},700)}else setMsg('siteMsg',j.error||'GPS Sync start failed','bad')}).catch(e=>setMsg('siteMsg','GPS Sync start failed: '+e,'bad'))}"));
  server.sendContent(F("function clearSavedSite(){jsonAction('/clearsettings','siteMsg',function(){clearFieldsDirty(['lat','lon','offset','year','month','day','hour','minute','second','poll','idlepoll','throttle']);setTimeout(updateNow,500)},'clearSavedSite')}function saveWifi(){jsonAction('/setwifi?ssid='+enc(id('wifi_ssid').value)+'&pass='+enc(id('wifi_pass').value),'wifiMsg',function(){clearFieldsDirty(['wifi_ssid','wifi_pass']);setTimeout(updateNow,500)},'saveWifi')}function clearWifi(){jsonAction('/clearwifi','wifiMsg',function(){clearFieldsDirty(['wifi_ssid','wifi_pass']);setTimeout(updateNow,500)},'clearWifi')}function saveAp(){jsonAction('/setap?ap_ssid='+enc(id('ap_ssid').value)+'&ap_pass='+enc(id('ap_pass').value)+'&ap_ip='+enc(id('ap_ip').value)+'&lx200_port='+enc(id('lx200_port').value)+'&stellarium_port='+enc(id('stellarium_port').value),'apMsg',function(){clearFieldsDirty(['ap_ssid','ap_pass','ap_ip','lx200_port','stellarium_port']);setTimeout(updateNow,500)},'saveAp')}function saveConnCfg(){jsonAction('/setap?ap_ssid='+enc(id('ap_ssid').value)+'&ap_pass='+enc(id('ap_pass').value)+'&ap_ip='+enc(id('ap_ip').value)+'&lx200_port='+enc(id('lx200_port').value)+'&stellarium_port='+enc(id('stellarium_port').value)+'&telnet_port='+enc(id('telnet_port').value)+'&telnet_pass='+enc(id('telnet_pass').value)+'&return='+enc(location.href),'connCfgMsg',function(){clearFieldsDirty(['lx200_port','stellarium_port','telnet_port','telnet_pass']);setTimeout(updateNow,500)},'saveConnCfg')}function defaultConnCfg(){jsonAction('/setup_action?name=Connection%20Defaults','connCfgMsg',null,'defaultConnCfg');setVal('web_port',80);setVal('alpaca_port',11111);setVal('alpaca_discovery_port',32227);setVal('lx200_port',4030);setVal('stellarium_port',10001);setVal('telnet_port',23);setVal('telnet_pass','nexstar');setMsg('connCfgMsg','Default server port values loaded. Click Save Server Config to apply.','ok')}function saveNtp(){let en=id('ntp_enable').checked?1:0;jsonAction('/setntp?enabled='+en+'&server1='+enc(id('ntp1').value)+'&server2='+enc(id('ntp2').value)+'&tz='+enc(id('tzrule').value),'ntpMsg',function(){clearFieldsDirty(['ntp_enable','ntp1','ntp2','tzrule']);setTimeout(updateNow,300)},'saveNtp')}function syncNtp(){jsonAction('/syncntp','ntpMsg',null,'syncNtp')}function fetchIpLoc(){jsonAction('/fetch_ip_location','ipLocMsg',function(){setTimeout(updateNow,500)},'fetchIpLoc')}function useIpLoc(){jsonAction('/use_ip_location','ipLocMsg',function(){setTimeout(updateNow,500)},'useIpLoc')}"));
  server.sendContent(F("const catDSO=[{id:'M1',n:'Crab Nebula',ra:5.57556,dec:22.0145},{id:'M2',n:'Globular Cluster Aquarius',ra:21.5575,dec:-0.8233},{id:'M3',n:'Globular Cluster Canes Venatici',ra:13.7032,dec:28.3773},{id:'M5',n:'Globular Cluster Serpens',ra:15.3092,dec:2.2810},{id:'M8',n:'Lagoon Nebula',ra:18.0602,dec:-24.3867},{id:'M11',n:'Wild Duck Cluster',ra:18.8512,dec:-6.2700},{id:'M13',n:'Hercules Globular Cluster',ra:16.6949,dec:36.4613},{id:'M15',n:'Pegasus Globular Cluster',ra:21.5006,dec:12.2870},{id:'M16',n:'Eagle Nebula',ra:18.3122,dec:-13.8067},{id:'M17',n:'Omega/Swan Nebula',ra:18.3464,dec:-16.1767},{id:'M20',n:'Trifid Nebula',ra:18.0383,dec:-23.0300},{id:'M22',n:'Sagittarius Cluster',ra:18.6067,dec:-23.9047},{id:'M27',n:'Dumbbell Nebula',ra:19.9934,dec:22.7210},{id:'M31',n:'Andromeda Galaxy',ra:0.7123,dec:41.2692},{id:'M32',n:'Andromeda Companion',ra:0.7116,dec:40.8653},{id:'M33',n:'Triangulum Galaxy',ra:1.5641,dec:30.6602},{id:'M35',n:'Gemini Open Cluster',ra:6.1514,dec:24.3333},{id:'M36',n:'Auriga Open Cluster',ra:5.6025,dec:34.1408},{id:'M37',n:'Auriga Open Cluster',ra:5.8717,dec:32.5533},{id:'M38',n:'Auriga Open Cluster',ra:5.4786,dec:35.8550},{id:'M41',n:'Canis Major Open Cluster',ra:6.7667,dec:-20.7567},{id:'M42',n:'Orion Nebula',ra:5.5881,dec:-5.3911},{id:'M43',n:'De Mairan Nebula',ra:5.5933,dec:-5.2778},{id:'M44',n:'Beehive Cluster',ra:8.6665,dec:19.6667},{id:'M45',n:'Pleiades',ra:3.7900,dec:24.1167},{id:'M46',n:'Puppis Open Cluster',ra:7.6962,dec:-14.8100},{id:'M47',n:'Puppis Open Cluster',ra:7.6097,dec:-14.5000},{id:'M51',n:'Whirlpool Galaxy',ra:13.4979,dec:47.1952},{id:'M53',n:'Coma Globular Cluster',ra:13.2154,dec:18.1682},{id:'M57',n:'Ring Nebula',ra:18.8931,dec:33.0292},{id:'M64',n:'Black Eye Galaxy',ra:12.9455,dec:21.6827},{id:'M65',n:'Leo Triplet Galaxy',ra:11.3155,dec:13.0922},{id:'M66',n:'Leo Triplet Galaxy',ra:11.3375,dec:12.9915},{id:'M67',n:'Cancer Open Cluster',ra:8.8500,dec:11.8167},{id:'M78',n:'Orion Reflection Nebula',ra:5.7794,dec:0.0792},{id:'M81',n:'Bode Galaxy',ra:9.9259,dec:69.0653},{id:'M82',n:'Cigar Galaxy',ra:9.9312,dec:69.6797},{id:'M92',n:'Hercules Globular Cluster',ra:17.2854,dec:43.1365},{id:'M97',n:'Owl Nebula',ra:11.2466,dec:55.0190},{id:'M101',n:'Pinwheel Galaxy',ra:14.0535,dec:54.3489},{id:'M104',n:'Sombrero Galaxy',ra:12.6665,dec:-11.6231},{id:'M106',n:'Canes Venatici Galaxy',ra:12.3160,dec:47.3037},{id:'M108',n:'Surfboard Galaxy',ra:11.1919,dec:55.6741},{id:'M109',n:'Barred Spiral Galaxy',ra:11.9598,dec:53.3745},{id:'NGC457',n:'Owl/ET Cluster',ra:1.3208,dec:58.2900},{id:'NGC869',n:'Double Cluster h Persei',ra:2.3167,dec:57.1333},{id:'NGC884',n:'Double Cluster chi Persei',ra:2.3667,dec:57.1500},{id:'NGC7000',n:'North America Nebula',ra:20.9800,dec:44.3333},{id:'NGC6960',n:'Veil Nebula West',ra:20.7600,dec:30.7200},{id:'NGC6992',n:'Veil Nebula East',ra:20.9400,dec:31.7200},{id:'M4',n:'Cat Eye Globular Cluster',ra:16.3931,dec:-26.5258},{id:'M6',n:'Butterfly Cluster',ra:17.6725,dec:-32.2533},{id:'M7',n:'Ptolemy Cluster',ra:17.8975,dec:-34.7928},{id:'M10',n:'Globular Cluster Ophiuchus',ra:16.9525,dec:-4.1003},{id:'M12',n:'Globular Cluster Ophiuchus',ra:16.7873,dec:-1.9485},{id:'M34',n:'Perseus Open Cluster',ra:2.7000,dec:42.7833},{id:'M50',n:'Heart-Shaped Cluster',ra:7.0533,dec:-8.3378},{id:'M52',n:'Cassiopeia Open Cluster',ra:23.4058,dec:61.5933},{id:'M63',n:'Sunflower Galaxy',ra:13.2637,dec:42.0294},{id:'M76',n:'Little Dumbbell Nebula',ra:1.7050,dec:51.5756},{id:'M94',n:'Croc Eye Galaxy',ra:12.8481,dec:41.1203},{id:'M110',n:'Andromeda Companion',ra:0.6728,dec:41.6853},{id:'NGC2237',n:'Rosette Nebula',ra:6.5333,dec:5.0500},{id:'NGC2264',n:'Christmas Tree Cluster/Cone Nebula',ra:6.6833,dec:9.8833},{id:'NGC2392',n:'Eskimo Nebula',ra:7.4852,dec:20.9118},{id:'NGC3242',n:'Ghost of Jupiter',ra:10.4127,dec:-18.6411},{id:'NGC4565',n:'Needle Galaxy',ra:12.6060,dec:25.9877},{id:'NGC4631',n:'Whale Galaxy',ra:12.7042,dec:32.5415},{id:'NGC5907',n:'Splinter Galaxy',ra:15.2745,dec:56.3289},{id:'NGC6826',n:'Blinking Planetary',ra:19.7485,dec:50.5259},{id:'NGC6888',n:'Crescent Nebula',ra:20.2010,dec:38.3550},{id:'IC434',n:'Horsehead Nebula',ra:5.6833,dec:-2.4667},{id:'IC1805',n:'Heart Nebula',ra:2.5500,dec:61.4500},{id:'IC1848',n:'Soul Nebula',ra:2.8500,dec:60.4333}];const catStars=[{id:'Polaris',n:'Polaris',ra:2.5303,dec:89.2641},{id:'Sirius',n:'Sirius',ra:6.7525,dec:-16.7161},{id:'Canopus',n:'Canopus',ra:6.3992,dec:-52.6957},{id:'Arcturus',n:'Arcturus',ra:14.2610,dec:19.1825},{id:'Vega',n:'Vega',ra:18.6156,dec:38.7837},{id:'Capella',n:'Capella',ra:5.2782,dec:45.9980},{id:'Rigel',n:'Rigel',ra:5.2423,dec:-8.2016},{id:'Procyon',n:'Procyon',ra:7.6550,dec:5.2250},{id:'Betelgeuse',n:'Betelgeuse',ra:5.9195,dec:7.4071},{id:'Altair',n:'Altair',ra:19.8464,dec:8.8683},{id:'Aldebaran',n:'Aldebaran',ra:4.5987,dec:16.5093},{id:'Spica',n:'Spica',ra:13.4199,dec:-11.1614},{id:'Antares',n:'Antares',ra:16.4901,dec:-26.4320},{id:'Pollux',n:'Pollux',ra:7.7553,dec:28.0262},{id:'Fomalhaut',n:'Fomalhaut',ra:22.9608,dec:-29.6222},{id:'Deneb',n:'Deneb',ra:20.6905,dec:45.2803},{id:'Regulus',n:'Regulus',ra:10.1395,dec:11.9672},{id:'Castor',n:'Castor',ra:7.5767,dec:31.8883},{id:'Bellatrix',n:'Bellatrix',ra:5.4189,dec:6.3497},{id:'Elnath',n:'Elnath',ra:5.4382,dec:28.6075},{id:'Alnitak',n:'Alnitak',ra:5.6793,dec:-1.9426},{id:'Alnilam',n:'Alnilam',ra:5.6036,dec:-1.2019},{id:'Mintaka',n:'Mintaka',ra:5.5334,dec:-0.2991},{id:'Dubhe',n:'Dubhe',ra:11.0621,dec:61.7510},{id:'Mizar',n:'Mizar',ra:13.3988,dec:54.9254},{id:'Achernar',n:'Achernar',ra:1.6286,dec:-57.2368},{id:'Hadar',n:'Beta Centauri',ra:14.0637,dec:-60.3730},{id:'Acrux',n:'Alpha Crucis',ra:12.4433,dec:-63.0991},{id:'Gacrux',n:'Gamma Crucis',ra:12.5194,dec:-57.1132},{id:'Menkent',n:'Theta Centauri',ra:14.1114,dec:-36.3700},{id:'Rasalhague',n:'Alpha Ophiuchi',ra:17.5822,dec:12.5600},{id:'Kochab',n:'Beta Ursae Minoris',ra:14.8451,dec:74.1555},{id:'Alkaid',n:'Eta Ursae Majoris',ra:13.7923,dec:49.3133},{id:'Alioth',n:'Epsilon Ursae Majoris',ra:12.9005,dec:55.9598},{id:'Alcor',n:'Alcor',ra:13.4204,dec:54.9879},{id:'Alphecca',n:'Alpha Coronae Borealis',ra:15.5781,dec:26.7147},{id:'Rasalgethi',n:'Alpha Herculis',ra:17.2441,dec:14.3903},{id:'Enif',n:'Epsilon Pegasi',ra:21.7364,dec:9.8750},{id:'Scheat',n:'Beta Pegasi',ra:23.0629,dec:28.0828},{id:'Markab',n:'Alpha Pegasi',ra:23.0793,dec:15.2053},{id:'Mirach',n:'Beta Andromedae',ra:1.1622,dec:35.6206},{id:'Almach',n:'Gamma Andromedae',ra:2.0649,dec:42.3297},{id:'Hamal',n:'Alpha Arietis',ra:2.1195,dec:23.4624},{id:'Mirfak',n:'Alpha Persei',ra:3.4054,dec:49.8612},{id:'Algol',n:'Beta Persei',ra:3.1361,dec:40.9556}];const catSolar=['Sun','Moon','Mercury','Venus','Mars','Jupiter','Saturn','Uranus','Neptune'];const catExtraCompact=`NGC869|Double Cluster h Persei|23300|571330|open^NGC884|Double Cluster chi Persei|23667|571500|open^NGC457|Owl Cluster|13167|582830|open^NGC663|Cassiopeia Open Cluster|17667|612170|open^NGC752|Andromeda Open Cluster|19583|377830|open^NGC7789|Caroline Rose Cluster|239583|567170|open^NGC7000|North America Nebula|209667|443330|nebula^IC5070|Pelican Nebula|208333|443670|nebula^NGC6960|Western Veil Nebula|207667|307000|nebula^NGC6992|Eastern Veil Nebula|209333|317170|nebula^NGC7635|Bubble Nebula|233333|612000|nebula^NGC281|Pacman Nebula|8833|566330|nebula^NGC1499|California Nebula|40000|366330|nebula^NGC2024|Flame Nebula|57000|-18500|nebula^NGC1977|Running Man Nebula|55917|-48500|nebula^NGC2174|Monkey Head Nebula|61500|206500|nebula^NGC2359|Thor's Helmet|73167|-132170|nebula^NGC3372|Carina Nebula|107500|-598670|nebula^NGC3603|Southern Nebula Cluster|112500|-612670|nebula^NGC6188|Fighting Dragons Nebula|166667|-487670|nebula^NGC6334|Cat's Paw Nebula|173333|-361170|nebula^NGC6357|Lobster Nebula|174000|-342000|nebula^M6|Butterfly Cluster|176683|-322170|open^M7|Ptolemy Cluster|178975|-348170|open^M10|Globular Cluster Ophiuchus|169517|-41000|globular^M12|Globular Cluster Ophiuchus|167875|-19500|globular^M14|Globular Cluster Ophiuchus|176267|-32450|globular^M19|Globular Cluster Ophiuchus|170438|-262670|globular^M21|Sagittarius Open Cluster|180700|-225000|open^M23|Sagittarius Open Cluster|179500|-190170|open^M24|Sagittarius Star Cloud|182667|-184830|open^M25|Sagittarius Open Cluster|185300|-191170|open^M26|Scutum Open Cluster|187517|-93830|open^M28|Globular Cluster Sagittarius|184100|-248670|globular^M29|Cygnus Open Cluster|203983|385330|open^M30|Globular Cluster Capricornus|216728|-231800|globular^M34|Perseus Open Cluster|27000|427830|open^M39|Cygnus Open Cluster|215333|484330|open^M40|Winnecke 4 Double Star|123700|580830|star^M48|Hydra Open Cluster|82283|-57500|open^M50|Monoceros Open Cluster|70500|-83330|open^M52|Cassiopeia Open Cluster|234000|616000|open^M54|Globular Cluster Sagittarius|189175|-304830|globular^M55|Globular Cluster Sagittarius|196667|-309670|globular^M56|Globular Cluster Lyra|192767|301830|globular^M69|Globular Cluster Sagittarius|185231|-323480|globular^M70|Globular Cluster Sagittarius|187202|-322930|globular^M71|Sagitta Globular Cluster|198960|187790|globular^M72|Globular Cluster Aquarius|208910|-125370|globular^M73|Aquarius Asterism|209833|-126330|open^M75|Globular Cluster Sagittarius|201013|-219220|globular^M79|Globular Cluster Lepus|54029|-245240|globular^M80|Globular Cluster Scorpius|162830|-229760|globular^M107|Globular Cluster Ophiuchus|165422|-130540|globular^M108|Surfboard Galaxy|111919|556740|galaxy^M109|Barred Spiral Galaxy|119599|533750|galaxy^NGC253|Sculptor Galaxy|7925|-252880|galaxy^NGC288|Sculptor Globular Cluster|8783|-265830|globular^NGC891|Edge-on Galaxy Andromeda|23758|423500|galaxy^NGC1023|Perseus Lenticular Galaxy|26767|390630|galaxy^NGC1232|Eridanus Spiral Galaxy|31620|-205790|galaxy^NGC1300|Barred Spiral Galaxy|33319|-194110|galaxy^NGC1316|Fornax A Galaxy|33783|-372080|galaxy^NGC1365|Great Barred Spiral Galaxy|35600|-361400|galaxy^NGC2403|Camelopardalis Galaxy|76150|656020|galaxy^NGC2683|UFO Galaxy|88775|334210|galaxy^NGC2903|Leo Galaxy|95333|215000|galaxy^NGC3115|Spindle Galaxy|100872|-77180|galaxy^NGC3521|Bubble Galaxy|110950|-350|galaxy^NGC3628|Hamburger Galaxy|113380|135890|galaxy^NGC4038|Antennae Galaxies|120314|-188670|galaxy^NGC4244|Silver Needle Galaxy|122939|378070|galaxy^NGC4449|Canes Venatici Galaxy|124698|440940|galaxy^NGC4725|One-armed Spiral Galaxy|128400|255010|galaxy^NGC5128|Centaurus A|134240|-430190|galaxy^NGC5139|Omega Centauri|134467|-474790|globular^NGC5195|M51 Companion|134990|472660|galaxy^NGC5236|Southern Pinwheel M83|136167|-298660|galaxy^NGC5986|Lupus Globular Cluster|157700|-377860|globular^NGC6144|Scorpius Globular Cluster|164500|-260230|globular^NGC6231|Northern Jewel Box|169000|-418000|open^NGC6543|Cat's Eye Nebula|179710|666330|planetary_nebula^NGC6572|Blue Racquetball Nebula|182010|68530|planetary_nebula^NGC6720|Ring Nebula M57|188931|330290|planetary_nebula^NGC6752|Pavo Globular Cluster|191767|-599840|globular^NGC7662|Blue Snowball Nebula|234310|425350|planetary_nebula^IC342|Hidden Galaxy|37800|680960|galaxy^IC1396|Elephant Trunk Nebula|216500|575000|nebula^IC5146|Cocoon Nebula|218917|472670|nebula^B33|Horsehead Dark Nebula|56833|-24670|nebula^Barnard33|Horsehead Nebula|56833|-24670|nebula^Caldwell14|Double Cluster|23500|571330|open^Caldwell20|North America Nebula|209667|443330|nebula^Caldwell33|Eastern Veil Nebula|209333|317170|nebula^Caldwell34|Western Veil Nebula|207667|307000|nebula^Caldwell49|Rosette Nebula|65333|50500|nebula^Caldwell63|Helix Nebula|224939|-208370|planetary_nebula^Caldwell76|Centaurus A|134240|-430190|galaxy^Caldwell80|Omega Centauri|134467|-474790|globular^Caldwell92|Carina Nebula|107500|-598670|nebula^Caldwell93|Jewel Box Cluster|128833|-603670|open`;function catDecodeExtra(s){return s.split('^').filter(Boolean).map(r=>{let p=r.split('|');return{id:p[0],n:p[1],ra:Number(p[2])/10000,dec:Number(p[3])/10000,k:p[4]||'other'}})}const catExtra=catDecodeExtra(catExtraCompact);/* BSC5_GENERATED_BEGIN */const catBSC=[];let catBSCLoaded=false,catBSCLoading=false;function catBscCount(){return catBSC.length}function catDecodeBscText(s){return s.split('^').filter(Boolean).map(r=>{let p=r.split('|');let hr=p[0],m=Number(p[4])/100;return{id:'HR'+hr,n:(p[1]||('HR'+hr))+' mag '+m.toFixed(2),ra:Number(p[2])/10000,dec:Number(p[3])/10000,mag:m,k:'star'}})}async function loadBSC5(){if(catBSCLoaded)return true;if(catBSCLoading)return false;catBSCLoading=true;setText('bscCount','loading...');try{let t=await getText('/bsc5_data');let a=catDecodeBscText(t);catBSC.splice(0,catBSC.length,...a);catBSCLoaded=true;setText('bscCount',String(catBSC.length));return true}catch(e){setText('bscCount','load failed');return false}finally{catBSCLoading=false}}/* BSC5_GENERATED_END */function catBscCount(){return typeof catBSC!=='undefined'?catBSC.length:0}function degNorm(x){x=x%360;if(x<0)x+=360;return x}function rad(d){return d*Math.PI/180}function deg(r){return r*180/Math.PI}function jdNow(){let d=new Date();return d.getTime()/86400000+2440587.5}function sunRaDec(){let jd=jdNow(),n=jd-2451545.0,L=degNorm(280.460+0.9856474*n),g=degNorm(357.528+0.9856003*n),lam=degNorm(L+1.915*Math.sin(rad(g))+0.020*Math.sin(rad(2*g))),eps=23.439-0.0000004*n;let ra=deg(Math.atan2(Math.cos(rad(eps))*Math.sin(rad(lam)),Math.cos(rad(lam))))/15;if(ra<0)ra+=24;let dec=deg(Math.asin(Math.sin(rad(eps))*Math.sin(rad(lam))));return{ra:ra,dec:dec,n:'Sun',id:'Sun',approx:true}}function moonRaDec(){let jd=jdNow(),d=jd-2451543.5,N=degNorm(125.1228-0.0529538083*d),i=5.1454,w=degNorm(318.0634+0.1643573223*d),a=60.2666,e=0.054900,M=degNorm(115.3654+13.0649929509*d);let E=rad(M)+e*Math.sin(rad(M))*(1+e*Math.cos(rad(M)));let xv=a*(Math.cos(E)-e),yv=a*(Math.sqrt(1-e*e)*Math.sin(E));let v=deg(Math.atan2(yv,xv)),r=Math.sqrt(xv*xv+yv*yv);let xh=r*(Math.cos(rad(N))*Math.cos(rad(v+w))-Math.sin(rad(N))*Math.sin(rad(v+w))*Math.cos(rad(i)));let yh=r*(Math.sin(rad(N))*Math.cos(rad(v+w))+Math.cos(rad(N))*Math.sin(rad(v+w))*Math.cos(rad(i)));let zh=r*(Math.sin(rad(v+w))*Math.sin(rad(i)));let lon=degNorm(deg(Math.atan2(yh,xh))),lat=deg(Math.atan2(zh,Math.sqrt(xh*xh+yh*yh)));let n=jd-2451545.0,eps=23.439-0.0000004*n;let xe=Math.cos(rad(lon))*Math.cos(rad(lat));let ye=Math.sin(rad(lon))*Math.cos(rad(lat))*Math.cos(rad(eps))-Math.sin(rad(lat))*Math.sin(rad(eps));let ze=Math.sin(rad(lon))*Math.cos(rad(lat))*Math.sin(rad(eps))+Math.sin(rad(lat))*Math.cos(rad(eps));let ra=deg(Math.atan2(ye,xe))/15;if(ra<0)ra+=24;let dec=deg(Math.atan2(ze,Math.sqrt(xe*xe+ye*ye)));return{ra:ra,dec:dec,n:'Moon',id:'Moon',approx:true}}function simpleSolar(name){if(name==='Sun')return sunRaDec();if(name==='Moon')return moonRaDec();let base={Mercury:[4.092,252.3,7],Venus:[1.602,181.9,3.4],Mars:[0.524,355.4,1.85],Jupiter:[0.0831,34.4,1.3],Saturn:[0.0335,50.1,2.5],Uranus:[0.0117,314.0,0.8],Neptune:[0.006,304.0,1.8]};let b=base[name];if(!b)return null;let jd=jdNow(),n=jd-2451545.0,lon=degNorm(b[1]+b[0]*n),eps=23.439-0.0000004*n;let ra=deg(Math.atan2(Math.cos(rad(eps))*Math.sin(rad(lon)),Math.cos(rad(lon))))/15;if(ra<0)ra+=24;let dec=deg(Math.asin(Math.sin(rad(eps))*Math.sin(rad(lon))));return{ra:ra,dec:dec,n:name,id:name,approx:true}}function catKind(o){if(o&&o.k)return o.k;let n=((o.n||'')+' '+(o.id||'')).toLowerCase();if(n.includes('globular'))return'globular';if(n.includes('open cluster')||n.includes('cluster'))return'open';if(n.includes('planetary')||n.includes('dumbbell')||n.includes('ring nebula')||n.includes('eskimo')||n.includes('ghost of jupiter')||n.includes('blinking'))return'planetary_nebula';if(n.includes('galaxy')||n.includes('andromeda')||n.includes('whirlpool')||n.includes('sombrero')||n.includes('pinwheel')||n.includes('needle')||n.includes('whale'))return'galaxy';if(n.includes('nebula')||n.includes('horsehead')||n.includes('rosette')||n.includes('heart')||n.includes('soul')||n.includes('veil'))return'nebula';return'other'}function catAllRows(){let a=[];catSolar.forEach(x=>a.push({id:x,n:x,src:'solar'}));catDSO.forEach(x=>a.push(Object.assign({src:'dso'},x)));catExtra.forEach(x=>a.push(Object.assign({src:'extra'},x)));catStars.forEach(x=>a.push(Object.assign({src:'star'},x)));catBSC.forEach(x=>a.push(Object.assign({src:'bsc'},x)));return a}function mountRaDecNow(){if(!lastStatus||!lastStatus.cacheValid)return null;let raH=Number(lastStatus.raHours),dec=Number(lastStatus.decDeg);if(!isFinite(raH)||!isFinite(dec))return null;return{raH:((raH%24)+24)%24,dec:dec}}function catAngularDistanceDeg(x){let m=mountRaDecNow();if(!m||!x||!isFinite(x.ra)||!isFinite(x.dec))return 9999;let ra1=m.raH*Math.PI/12,ra2=Number(x.ra)*Math.PI/12,d1=m.dec*Math.PI/180,d2=Number(x.dec)*Math.PI/180;let c=Math.sin(d1)*Math.sin(d2)+Math.cos(d1)*Math.cos(d2)*Math.cos(ra1-ra2);return Math.acos(Math.max(-1,Math.min(1,c)))*180/Math.PI}function catIdentifyPool(){let a=catSolar.map(x=>simpleSolar(x)).filter(x=>x&&isFinite(x.ra)&&isFinite(x.dec)).map(x=>Object.assign({src:'solar'},x)).concat(catDSO.map(x=>Object.assign({src:'dso'},x))).concat(catExtra.map(x=>Object.assign({src:'extra'},x))).concat(catStars.map(x=>Object.assign({src:'star'},x)));if(catBSCLoaded)a=a.concat(catBSC.map(x=>Object.assign({src:'bsc'},x)));return a}let lastPointingIdKey='';function autoUpdatePointingId(){if(!lastStatus||!lastStatus.cacheValid)return;let k=Number(lastStatus.raHours).toFixed(4)+','+Number(lastStatus.decDeg).toFixed(3);if(k===lastPointingIdKey)return;lastPointingIdKey=k;if(typeof identifyPointingNow==='function')identifyPointingNow()}function sameCatalogObject(a,b){if(!a||!b)return false;return (a.src||'dso')===(b.src||'dso')&&a.id===b.id}function catalogShortLabel(x){return x?x.id+(x.n&&x.n!==x.id?' - '+x.n:''):'?'}function identifyPointingNow(){let m=mountRaDecNow(),e=id('nearestInfo'),box=id('pointingIdBox'),s=id('nearestSelect');if(s)s.innerHTML='';let nc=id('nearCount');if(nc)nc.textContent='0';if(!m){if(box)box.value='waiting for valid mount RA/Dec cache';if(e)e.innerHTML='Pointing ID: waiting for valid mount RA/Dec cache';return}if(!catBSCLoaded&&!catBSCLoading){loadBSC5().then(()=>{lastPointingIdKey='';identifyPointingNow();catPopulate()})}let a=catIdentifyPool().map(x=>Object.assign({},x,{dist:catAngularDistanceDeg(x)})).filter(x=>isFinite(x.dist)&&x.dist<9999).sort((x,y)=>x.dist-y.dist);let best=a[0];if(!best){if(box)box.value='no catalog match yet';if(e)e.innerHTML='Pointing ID: no catalog match yet';return}if(box)box.value=catOptionText(best);let nr=id('nearRadius'),rad=nr?Number(nr.value):10;if(!isFinite(rad)||rad<=0)rad=10;let near=a.filter(x=>!sameCatalogObject(x,best)&&isFinite(x.dist)&&x.dist<=rad);if(s){let nc=id('nearCount');if(nc)nc.textContent=String(near.length);let ph=document.createElement('option');ph.value='';ph.textContent=near.length?'Select a nearby object':'No nearby objects inside radius';ph.selected=true;s.appendChild(ph);near.forEach((x,i)=>{let op=document.createElement('option');op.value=(x.src||'dso')+':'+x.id;op.textContent=(i===0?'BEST: ':'')+catNearestOptionText(x);s.appendChild(op)})}if(e)e.innerHTML='<b>Pointing ID:</b> '+catalogShortLabel(best)+'<br><b>Mount RA/Dec used:</b> '+m.raH.toFixed(6)+' h / '+m.dec.toFixed(6)+' deg<br><b>Distance:</b> '+best.dist.toFixed(2)+' deg'}function catSortAndLimit(a){let sm=id('cat_sort'),sort=sm?sm.value:'brightness';if(sort==='name')a.sort((x,y)=>(x.n||x.id||'').localeCompare(y.n||y.id||''));else if(sort==='visibility')a.sort((x,y)=>{let ax=catAltAzFromRaDec(x.ra,x.dec),ay=catAltAzFromRaDec(y.ra,y.dec);let va=((ax&&isFinite(ax.alt))?ax.alt:-999),vb=((ay&&isFinite(ay.alt))?ay.alt:-999);return vb-va});else a.sort((x,y)=>(isFinite(x.mag)?x.mag:99)-(isFinite(y.mag)?y.mag:99));window.catTotalBeforeLimit=a.length;return a}function catBaseArrayForGroup(v){let a=[];if(v==='solar')a=catSolar.map(x=>({id:x,n:x,src:'solar'}));else if(v==='stars')a=catStars.map(x=>Object.assign({src:'star'},x));else if(v==='bsc'){if(!catBSCLoaded&&!catBSCLoading)loadBSC5().then(()=>{identifyPointingNow();catPopulate()});a=catBSC.map(x=>Object.assign({src:'bsc'},x));}else{a=catDSO.map(x=>Object.assign({src:'dso'},x)).concat(catExtra.map(x=>Object.assign({src:'extra'},x)));if(v==='galaxy'||v==='nebula'||v==='planetary_nebula'||v==='globular'||v==='open')a=a.filter(x=>catKind(x)===v);}return a}function catArray(){let g=id('cat_group'),v=g?g.value:'messier',q=(id('cat_search')?id('cat_search').value:'').toLowerCase();let a=[];if(q){a=catAllRows().filter(x=>(x.id+' '+(x.n||'')).toLowerCase().includes(q));}else a=catBaseArrayForGroup(v);return catSortAndLimit(a)}window.catVisibleOnly=false;function catVisibleNow(c){let aa=catAltAzFromRaDec(c.ra,c.dec);if(!aa||aa.alt<0)return false;if(!lastStatus||!lastStatus.siteValid||!lastStatus.timeValid)return false;let lat=Number(lastStatus.latitude)*Math.PI/180,dec=Number(c.dec)*Math.PI/180,cLat=Math.cos(lat),cDec=Math.cos(dec);if(!isFinite(lat)||!isFinite(dec)||Math.abs(cLat)<1e-6||Math.abs(cDec)<1e-6)return false;let cosH=(0-Math.sin(lat)*Math.sin(dec))/(cLat*cDec);if(cosH<-1)return true;if(cosH>1)return false;let H=Math.acos(Math.max(-1,Math.min(1,cosH)))*12/Math.PI,lst=lstHours();if(!isFinite(lst))return false;let hoursUntilSet=(((Number(c.ra)+H-lst)%24)+24)%24/1.00273790935;return hoursUntilSet>0.02}function catToggleVisible(){}function localTzAbbr(){if(!lastStatus)return'';let off=Number(lastStatus.utcOffsetMinutes);if(!isFinite(off))return'';if(off===-420)return'MST';if(off===-360)return'MDT';let sg=off>=0?'+':'-',a=Math.abs(off),h=Math.floor(a/60),m=a%60;return'UTC'+sg+String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')}function eventBaseMs(dayOff){if(!lastStatus)return NaN;let off=Number(lastStatus.utcOffsetMinutes)||0;return Date.UTC(Number(lastStatus.localYear),Number(lastStatus.localMonth)-1,Number(lastStatus.localDay)+dayOff)-off*60000}function eventFmt(ms,base){let off=Number(lastStatus.utcOffsetMinutes)||0,d=new Date(ms+off*60000),b=new Date(base+off*60000),h=d.getUTCHours(),m=d.getUTCMinutes(),ap=h>=12?'PM':'AM',hh=h%12;if(!hh)hh=12;let suf=d.getUTCDate()!==b.getUTCDate()?' +1':'';return hh+':'+String(m).padStart(2,'0')+' '+ap+suf}function sunMoonEvents(kind){if(!lastStatus||!lastStatus.siteValid||!lastStatus.timeValid)return null;let lat=Number(lastStatus.latitude),lon=Number(lastStatus.longitude);if(!isFinite(lat)||!isFinite(lon))return null;let r=Math.PI/180,dayMs=864e5,J1970=2440588,J2000=2451545,toJ=t=>t/dayMs-0.5+J1970,fromJ=j=>(j+0.5-J1970)*dayMs,toD=t=>toJ(t)-J2000,lw=-lon*r,phi=lat*r,ob=23.4397*r;function ra(l,b){return Math.atan2(Math.sin(l)*Math.cos(ob)-Math.tan(b)*Math.sin(ob),Math.cos(l))}function dec(l,b){return Math.asin(Math.sin(b)*Math.cos(ob)+Math.cos(b)*Math.sin(ob)*Math.sin(l))}function sid(d){return r*(280.16+360.9856235*d)-lw}function alt(H,d){return Math.asin(Math.sin(phi)*Math.sin(d)+Math.cos(phi)*Math.cos(d)*Math.cos(H))}function refr(h){if(h<0)h=0;return 0.0002967/Math.tan(h+0.00312536/(h+0.08901179))}function sunC(d){let M=r*(357.5291+0.98560028*d),C=r*(1.9148*Math.sin(M)+0.02*Math.sin(2*M)+0.0003*Math.sin(3*M)),P=r*102.9372,L=M+C+P+Math.PI;return{M:M,L:L,dec:dec(L,0),ra:ra(L,0)}}function moonC(d){let L=r*(218.316+13.176396*d),M=r*(134.963+13.064993*d),F=r*(93.272+13.22935*d),l=L+r*6.289*Math.sin(M),b=r*5.128*Math.sin(F);return{ra:ra(l,b),dec:dec(l,b)}}function moonAlt(t){let d=toD(t),c=moonC(d),h=alt(sid(d)-c.ra,c.dec);return h+refr(h)}function solar(base){let d=toD(base),n=Math.round(d-0.0009-lw/(2*Math.PI)),ds=0.0009+lw/(2*Math.PI)+n,c=sunC(ds),Jnoon=J2000+ds+0.0053*Math.sin(c.M)-0.0069*Math.sin(2*c.L),w=Math.acos((Math.sin(-0.833*r)-Math.sin(phi)*Math.sin(c.dec))/(Math.cos(phi)*Math.cos(c.dec))),a=w/(2*Math.PI),Jset=J2000+ds+a+0.0053*Math.sin(c.M)-0.0069*Math.sin(2*c.L),Jrise=Jnoon-(Jset-Jnoon);return{rise:fromJ(Jrise),set:fromJ(Jset)}}function moonDay(base){let hc=0.133*r,h0=moonAlt(base)-hc,rise=null,set=null;for(let i=1;i<=24;i+=2){let h1=moonAlt(base+i*3600e3)-hc,h2=moonAlt(base+(i+1)*3600e3)-hc,a=(h0+h2)/2-h1,b=(h2-h0)/2,xe=-b/(2*a),ye=(a*xe+b)*xe+h1,D=b*b-4*a*h1,n=0,x1=0,x2=0;if(D>=0){let dx=Math.sqrt(D)/(Math.abs(a)*2);x1=xe-dx;x2=xe+dx;if(Math.abs(x1)<=1)n++;if(Math.abs(x2)<=1)n++;if(x1<-1)x1=x2}if(n===1){if(h0<0)rise=base+(i+x1)*3600e3;else set=base+(i+x1)*3600e3}else if(n===2){rise=base+(i+(ye<0?x2:x1))*3600e3;set=base+(i+(ye<0?x1:x2))*3600e3}if(rise&&set)break;h0=h2}return{rise:rise,set:set}}let base=eventBaseMs(0);if(kind==='Sun'){let e=solar(base);return{short:'R '+eventFmt(e.rise,base)+' S '+eventFmt(e.set,base),detail:'Rise/Set: sunrise '+eventFmt(e.rise,base)+' / sunset '+eventFmt(e.set,base)}}if(kind==='Moon'){let e=moonDay(base);if(!e.rise)e.rise=moonDay(eventBaseMs(1)).rise;if(!e.set)e.set=moonDay(eventBaseMs(1)).set;return{short:'R '+(e.rise?eventFmt(e.rise,base):'none')+' S '+(e.set?eventFmt(e.set,base):'none'),detail:'Rise/Set: moonrise '+(e.rise?eventFmt(e.rise,base):'none')+' / moonset '+(e.set?eventFmt(e.set,base):'none')}}return null}function catRiseSet(c,decArg){if(!lastStatus||!lastStatus.siteValid||!lastStatus.timeValid)return null;if(c&&typeof c==='object'&&(c.id==='Sun'||c.id==='Moon'))return sunMoonEvents(c.id);let raHours=typeof c==='object'?c.ra:c,decDeg=typeof c==='object'?c.dec:decArg;let tz=localTzAbbr(),z=tz?' '+tz:'',lat=Number(lastStatus.latitude)*Math.PI/180,dec=Number(decDeg)*Math.PI/180,cLat=Math.cos(lat),cDec=Math.cos(dec);if(!isFinite(lat)||!isFinite(dec)||Math.abs(cLat)<1e-6||Math.abs(cDec)<1e-6)return null;let cosH=(0-Math.sin(lat)*Math.sin(dec))/(cLat*cDec);if(cosH>1)return{short:'never rises',detail:'Rise/Set: never rises at current site'};if(cosH<-1)return{short:'circumpolar',detail:'Rise/Set: circumpolar at current site'};let H=Math.acos(Math.max(-1,Math.min(1,cosH)))*12/Math.PI,lst=lstHours();if(!isFinite(lst))return null;function p(n){return String(n).padStart(2,'0')}function at(targetLst){let dh=(((targetLst-lst)%24)+24)%24/1.00273790935,now=Number(lastStatus.localHour)*3600+Number(lastStatus.localMinute)*60+Number(lastStatus.localSecond),sec=Math.round((now+dh*3600)%86400);if(sec<0)sec+=86400;return p(Math.floor(sec/3600))+':'+p(Math.floor((sec%3600)/60))}let rr=at(Number(raHours)-H),ss=at(Number(raHours)+H);return{short:'R '+rr+z+' S '+ss+z,detail:'Rise/Set: next rise '+rr+z+' / next set '+ss+z}}function catAltAzShort(c){let aa=c?catAltAzFromRaDec(c.ra,c.dec):null;if(aa&&isFinite(aa.alt)&&isFinite(aa.az))return'Alt '+Number(aa.alt).toFixed(1)+' Az '+Number(aa.az).toFixed(1);return'Alt ? Az ?'}function catOptionText(x){let c=x.src==='solar'?simpleSolar(x.id):x;let t=x.id+(x.n&&x.n!==x.id?' - '+x.n:'');if(c)t+=' | '+catAltAzShort(c);let rs=c?catRiseSet(c):null;if(rs)t+=' | '+rs.short;return t}function catNearestOptionText(x){let t=catOptionText(x);if(isFinite(x.dist)&&x.dist<9999)t+=' | '+x.dist.toFixed(2)+' deg away';return t}function catRefreshLabels(){let o=id('cat_obj');if(!o)return;let arr=catArray(),m={};arr.forEach(x=>{m[(x.src||'dso')+':'+x.id]=catOptionText(x)});for(let i=0;i<o.options.length;i++){let v=o.options[i].value;if(m[v])o.options[i].textContent=m[v]}}function catPopulate(){setText('bscCount',String(catBscCount()));let o=id('cat_obj');if(!o)return;let arr=catArray();let old=o.value;o.innerHTML='';arr.forEach(x=>{let op=document.createElement('option');let c=x.src==='solar'?simpleSolar(x.id):x,aa=c?catAltAzFromRaDec(c.ra,c.dec):null;op.value=(x.src||'dso')+':'+x.id;op.textContent=catOptionText(x);if(aa&&isFinite(aa.alt)&&aa.alt<0){op.style.color='#777';op.style.backgroundColor='#222'}o.appendChild(op)});setText('catShown',String(arr.length));setText('catTotal',String(window.catTotalBeforeLimit||arr.length));if(old){for(let i=0;i<o.options.length;i++){if(o.options[i].value===old){o.selectedIndex=i;break}}}if(o.selectedIndex<0&&o.options.length>0)o.selectedIndex=0;catUpdate()}function catSelected(){let o=id('cat_obj');if(!o||!o.value)return null;let p=o.value.split(':'),src=p[0],v=p.slice(1).join(':');if(src==='solar')return simpleSolar(v);if(src==='star')return catStars.find(x=>x.id===v);if(src==='bsc')return catBSC.find(x=>x.id===v);if(src==='extra')return catExtra.find(x=>x.id===v);return catDSO.find(x=>x.id===v)}function catSetInfo(s){let e=document.getElementById('cat_info');if(!e)return;e.innerHTML=s}function hms(ra){ra=((Number(ra)%24)+24)%24;let h=Math.floor(ra),m=Math.floor((ra-h)*60),s=(((ra-h)*60-m)*60);return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+s.toFixed(1).padStart(4,'0')}function dms(dec){let d0=Number(dec),sg=d0<0?'-':'+';d0=Math.abs(d0);let d=Math.floor(d0),m=Math.floor((d0-d)*60),s=(((d0-d)*60-m)*60);return sg+String(d).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+s.toFixed(0).padStart(2,'0')}function catAltAzFromRaDec(raHours,decDeg){if(!lastStatus)return null;let lat=Number(lastStatus.latitude),lon=Number(lastStatus.longitude),yr=Number(lastStatus.localYear),mo=Number(lastStatus.localMonth),dy=Number(lastStatus.localDay),hr=Number(lastStatus.localHour),mi=Number(lastStatus.localMinute),se=Number(lastStatus.localSecond);if(!isFinite(lat)||!isFinite(lon)||!isFinite(yr)||!isFinite(mo)||!isFinite(dy)||!isFinite(hr)||!isFinite(mi)||!isFinite(se))return null;function d2r(d){return d*Math.PI/180}function r2d(r){return r*180/Math.PI}let off=Number(lastStatus.utcOffsetMinutes||0),utcMs=Date.UTC(yr,mo-1,dy,hr,mi,se)-off*60000,jd=utcMs/86400000+2440587.5,T=(jd-2451545.0)/36525.0,gmst=280.46061837+360.98564736629*(jd-2451545.0)+0.000387933*T*T-(T*T*T)/38710000.0;gmst=((gmst%360)+360)%360;let lst=((gmst+lon)%360+360)%360,raDeg=((Number(raHours)*15)%360+360)%360,ha=((lst-raDeg)%360+360)%360;if(ha>180)ha-=360;let haR=d2r(ha),decR=d2r(Number(decDeg)),latR=d2r(lat),sinAlt=Math.sin(decR)*Math.sin(latR)+Math.cos(decR)*Math.cos(latR)*Math.cos(haR);sinAlt=Math.max(-1,Math.min(1,sinAlt));let alt=r2d(Math.asin(sinAlt)),cosAz=(Math.sin(decR)-Math.sin(d2r(alt))*Math.sin(latR))/(Math.cos(d2r(alt))*Math.cos(latR));cosAz=Math.max(-1,Math.min(1,cosAz));let az=r2d(Math.acos(cosAz));if(Math.sin(haR)>0)az=360-az;az=((az%360)+360)%360;return{alt:alt,az:az}}function catUpdate(){let c=catSelected();if(!c){catSetInfo('RA ? / Dec ?<br>Sky Alt ? / Az ?<br>Mount Alt ? / Az ?');return}let aa=catAltAzFromRaDec(c.ra,c.dec);let coord='RA '+hms(c.ra)+' h / Dec '+dms(c.dec)+' deg<br>Sky Alt '+(aa?Number(aa.alt).toFixed(2):'?')+' deg / Az '+(aa?Number(aa.az).toFixed(2):'?')+' deg';coord+='<br>Mount Alt '+(lastStatus&&lastStatus.altAzCacheValid?Number(lastStatus.altDeg).toFixed(2):'?')+' deg / Az '+(lastStatus&&lastStatus.altAzCacheValid?Number(lastStatus.azDeg).toFixed(2):'?')+' deg';if(c.approx)coord+=' APPROX ephemeris';catSetInfo(coord)}function groupForCatalogItem(c){let src=c?(c.src||'dso'):'dso';if(src==='bsc')return'bsc';if(src==='star')return'stars';if(src==='solar')return'solar';let k=catKind(c);if(k==='galaxy'||k==='nebula'||k==='planetary_nebula'||k==='globular'||k==='open')return k;return'messier'}function forceObjectPulldownTo(c){if(!c)return false;let o=id('cat_obj');if(!o)return false;let val=(c.src||'dso')+':'+c.id;for(let i=0;i<o.options.length;i++){if(o.options[i].value===val){o.selectedIndex=i;catUpdate();return true}}return false}function selectNearestCandidate(){let s=id('nearestSelect');if(!s||!s.value)return;let p=s.value.split(':'),src=p[0],objId=p.slice(1).join(':'),a=catIdentifyPool();let c=a.find(x=>(x.src||'dso')===src&&x.id===objId);if(!c)return;let g=id('cat_group');if(g){let gv=groupForCatalogItem(c);if(g.value!==gv)g.value=gv}let search=id('cat_search');if(search)search.value='';catPopulate();if(!forceObjectPulldownTo(c)){let search2=id('cat_search');if(search2)search2.value=c.id;catPopulate();forceObjectPulldownTo(c)}}function setRaDecRaw(ra,dec){let r=document.getElementById('ra_hours'),d=document.getElementById('dec_deg');gotoRaDecDirty=false;if(r){r.dataset.dirty='0';r.value=Number(ra).toFixed(6);r.dispatchEvent(new Event('input',{bubbles:true}))}if(d){d.dataset.dirty='0';d.value=Number(dec).toFixed(6);d.dispatchEvent(new Event('input',{bubbles:true}))}setTimeout(()=>{if(r){r.dataset.dirty='0';r.value=Number(ra).toFixed(6)}if(d){d.dataset.dirty='0';d.value=Number(dec).toFixed(6)}},50)}function catLoad(){setMsg('catMsg','Use Slew to Object','ok')}function catSlew(){let c=catSelected();if(!c){setMsg('catMsg','No catalog object selected','bad');return}if(!confirmAboveHorizonRaDec(c.ra,c.dec,'catMsg'))return;jsonAction('/webgoto_radec?ra_hours='+enc(Number(c.ra).toFixed(6))+'&dec_deg='+enc(Number(c.dec).toFixed(6)),'catMsg',function(){setTimeout(updateNow,1200)},'catalogSlew')}"));
  server.sendContent(F("let lastStatus=null,targetsInitialized=false;function targetFieldsLocked(){return false}function lockTargetFields(ms){}function setText(n,v){let e=id(n);if(e)e.textContent=v}function setVal(n,v,fix){let e=id(n);if(!e||v===undefined||v===null||Number.isNaN(v))return;if(e.dataset.dirty==='1'||document.activeElement===e)return;if(fix!==undefined&&typeof v==='number')e.value=v.toFixed(fix);else e.value=v}function setChecked(n,v){let e=id(n);if(!e)return;if(e.dataset.dirty==='1'||document.activeElement===e)return;e.checked=!!v}function markDirty(){this.dataset.dirty='1'}function markRaDecDirty(){gotoRaDecDirty=true;markDirty.call(this)}function markAltAzDirty(){gotoAltAzDirty=true;markDirty.call(this)}function clearFieldsDirty(list){list.forEach(n=>{let e=id(n);if(e)e.dataset.dirty='0'})}function clearSiteDirty(){clearFieldsDirty(['lat','lon','offset','year','month','day','hour','minute','second','poll','idlepoll','throttle'])}function clearRateDirty(){clearFieldsDirty(['r1','r2','r3','r4','rateSel'])}function attachDirty(){['lat','lon','offset','year','month','day','hour','minute','second','poll','idlepoll','throttle','r1','r2','r3','r4','rateSel','wifi_ssid','wifi_pass','ap_ssid','ap_pass','ap_ip','lx200_port','stellarium_port','ntp1','ntp2','tzrule','nearRadius','ntp_enable','dec_limit_enable','alt_limit_enable','dec_min','dec_max','alt_min','alt_max'].forEach(n=>{let e=id(n);if(e&&!e.dataset.bound){e.addEventListener('input',markDirty);e.addEventListener('change',markDirty);e.dataset.bound='1'}})}function setTargetRaDec(ra,dec){let r=id('ra_hours'),d=id('dec_deg');if(r)r.value=Number(ra).toFixed(6);if(d)d.value=Number(dec).toFixed(6)}function setTargetAltAz(alt,az){let a=id('alt_deg'),z=id('az_deg');if(a)a.value=Number(alt).toFixed(6);if(z)z.value=Number(az).toFixed(6)}function loadCurrentRaDecTarget(){if(!lastStatus)return;setTargetRaDec(lastStatus.raHours,lastStatus.decDeg);setMsg('gotoMsg','Loaded current RA/Dec into target fields','ok')}function loadCurrentAltAzTarget(){if(!lastStatus)return;setTargetAltAz(lastStatus.altDeg,lastStatus.azDeg);setMsg('altazMsg','Loaded current Alt/Az into target fields','ok')}function fillInputs(j){lastStatus=j;attachDirty();autoUpdatePointingId();setText('curRaHours',j.cacheValid?j.raHours.toFixed(6):'?');setText('curDecDeg',j.cacheValid?j.decDeg.toFixed(6):'?');setText('curAltDeg',j.altAzCacheValid?j.altDeg.toFixed(6):'?');setText('curAzDeg',j.altAzCacheValid?j.azDeg.toFixed(6):'?');let tzBanner=(typeof localTzAbbr==='function'&&j.timeValid)?localTzAbbr():'';setText('topDateTime',j.localYear+'-'+String(j.localMonth).padStart(2,'0')+'-'+String(j.localDay).padStart(2,'0')+' '+String(j.localHour).padStart(2,'0')+':'+String(j.localMinute).padStart(2,'0')+':'+String(j.localSecond).padStart(2,'0')+(tzBanner?' '+tzBanner:''));setText('topLat',Number(j.latitude).toFixed(6));setText('topLon',Number(j.longitude).toFixed(6));setText('topCpuLoad',j.cpuLoad||'CPU Load --%');if(!targetsInitialized){setTargetRaDec(j.raHours,j.decDeg);setTargetAltAz(j.altDeg,j.azDeg);targetsInitialized=true}setVal('lat',j.latitude,6);setVal('lon',j.longitude,6);setVal('offset',j.utcOffsetMinutes);setVal('poll',j.pollIntervalMs);setVal('throttle',j.clientThrottleMs);setVal('r1',j.nudgeRate1,3);setVal('r2',j.nudgeRate2,3);setVal('r3',j.nudgeRate3,3);setVal('r4',j.nudgeRate4,3);setVal('dec_min',j.safeDecMinDeg,2);setVal('dec_max',j.safeDecMaxDeg,2);setChecked('dec_limit_enable',j.safeDecLimitEnabled);setVal('alt_min',j.safeAltMinDeg,2);setVal('alt_max',j.safeAltMaxDeg,2);setChecked('alt_limit_enable',j.safeAltLimitEnabled);setVal('rateSel',j.webSelectedRate);setVal('year',j.localYear);setVal('month',j.localMonth);setVal('day',j.localDay);setVal('hour',j.localHour);setVal('minute',j.localMinute);setVal('second',j.localSecond);setVal('wifi_ssid',j.staSsid);setVal('ap_ssid',j.apSsid);setVal('ap_pass',j.apPass);setVal('ap_ip',j.apIpConfig);setVal('web_port',j.webPort);setVal('alpaca_port',j.alpacaPort);setVal('alpaca_discovery_port',j.alpacaDiscoveryPort);setVal('lx200_port',j.lx200Port);setVal('stellarium_port',j.stellariumPort);setVal('telnet_port',j.telnetPort);setVal('ntp1',j.ntpServer1);setVal('ntp2',j.ntpServer2);setVal('tzrule',j.tzRule);setVal('logLevelSel',j.logLevelName||'info');setChecked('ntp_enable',j.ntpEnabled);setText('ipLocStatus',j.approxIpLocationStatus);setText('ipLocLatLon',j.approxIpLocationValid?(Number(j.approxIpLat).toFixed(6)+', '+Number(j.approxIpLon).toFixed(6)):'none');setText('ipLocText',j.approxIpLocationText||'');if(typeof catRefreshLabels==='function')catRefreshLabels();if(typeof catUpdate==='function')catUpdate();if(typeof identifyPointingNow==='function'){}}"));
  server.sendContent(F("const _fillInputsBase=fillInputs;fillInputs=function(j){_fillInputsBase(j);setVal('idlepoll',j.idlePollIntervalMs);let e=id('idlepoll');if(e&&!e.dataset.bound){e.addEventListener('input',markDirty);e.addEventListener('change',markDirty);e.dataset.bound='1'}};"));
  server.sendContent(F("async function updateNow(){try{let j=await getJson('/status');let sc=id('statusCard');sc.classList.remove('alive','busy','idle','fault');if(j.mountFault){sc.classList.add('fault');setText('healthLine','MOUNT NOT RESPONDING');setText('healthLine2','MOUNT NOT RESPONDING')}else if(j.mountBusy){sc.classList.add('busy');setText('healthLine','MOUNT BUSY / SLEWING');setText('healthLine2','MOUNT BUSY / SLEWING')}else if(j.mountAlive){sc.classList.add('alive');setText('healthLine','MOUNT CONNECTED - '+j.lastResponseAgeMs+' ms');setText('healthLine2','MOUNT CONNECTED - '+j.lastResponseAgeMs+' ms')}else{sc.classList.add('idle');setText('healthLine','MOUNT UNKNOWN');setText('healthLine2','MOUNT UNKNOWN')}id('statusBox').textContent=j.statusText;id('sysBox').textContent=j.systemText;let bs=id('statusBasicBox'),bh=id('sysBasicBox');if(bs)bs.textContent=j.basicStatusText||'Status unavailable';if(bh)bh.textContent=j.basicSystemText||'System health unavailable';let la=id('logAlert');if(la){if(j.mountFault){la.classList.remove('off');la.textContent='MOUNT DISCONNECTED: '+(j.lastMountFault||'not responding')}else{la.classList.add('off');la.textContent=''}}fillInputs(j)}catch(e){}}let logsLive=true;function selectedLogCats(){let a=[];document.querySelectorAll('.logcat').forEach(c=>{if(c.checked)a.push(c.value)});return a.length?a.join(','):'none'}function applyLogSettings(){jsonAction('/setlog?level='+enc(id('logLevelSel').value)+'&cats='+enc(selectedLogCats()),'logsMsg',function(){refreshLogs()},'applyLogSettings')}function toggleLogsLive(){logsLive=!logsLive;let b=id('logsLiveBtn');if(b)b.textContent=logsLive?'Pause Logs':'Start Logs';if(logsLive)refreshLogs()}async function refreshLogs(){if(!logsLive)return;try{let b=id('logBox');b.textContent=await getText('/logs');b.scrollTop=0}catch(e){}}async function initialPageLoad(){applyScale();applyTheme();applyStatusMode();restoreTab();catPopulate();updateNow();refreshLogs();if(false){setTimeout(function(){startupReadAltAz()},900)}setTimeout(updateNow,2500);setTimeout(function(){updateNow();if(typeof catRefreshLabels==='function')catRefreshLabels();if(typeof catUpdate==='function')catUpdate()},4500)}setInterval(updateNow,8000);setInterval(refreshLogs,5000);window.addEventListener('load',initialPageLoad);"));
  server.sendContent(F("function badge(ok,warn){return ok?'ok':(warn?'bad':'warn')}function age(ms){return ms?Math.round(ms/1000)+'s ago':'never'}function dashSet(n,t,c){let e=id(n);if(e){e.textContent=t;e.className='badge '+(c||'warn')}}function updateProtocolDash(j){let btRx=j.lx200BtRxCommands||0,btTx=j.lx200BtTxReplies||0,eff=j.effectivePollIntervalMs||0,dem=j.positionDemandActive?'active':'idle';dashSet('bMount',j.mountFault?'FAULT':(j.mountBusy?'BUSY':(j.mountAlive?'OK':'?')),j.mountAlive&&!j.mountFault,j.mountFault);dashSet('bTime',j.timeValid?'TIME OK':'TIME ?',j.timeValid,!j.timeValid);dashSet('bLoc',j.siteValid?'LOC OK':'LOC ?',j.siteValid,!j.siteValid);dashSet('bAlpaca',j.alpacaConnected?'ALPACA OK':'ALPACA ?',j.alpacaConnected,false);dashSet('bSky',j.bluetoothConnected?'BT SKY ON':(btRx?'BT SKY RX':(j.lx200WifiConnected?'WIFI SKY ON':(j.lx200WifiRxCommands?'WIFI SKY RX':'SKY WAIT'))),j.bluetoothConnected||btRx||j.lx200WifiConnected||j.lx200WifiRxCommands,false);dashSet('bStel',j.stellariumConnected?'STEL ON':(j.stellariumRxPackets?'STEL RX':'STEL WAIT'),j.stellariumConnected||j.stellariumRxPackets,false);let dm=id('dashMount');if(dm)dm.style.whiteSpace='pre-line';setText('dashMount','current poll '+eff+' ms '+dem+'\\nmount uptime '+(j.mountUptime||'0:00')+' h:mm\\npoll age '+j.mountCurrentRaDecAgeMs+' ms');setText('dashTime','source '+j.timeSource+' / location '+j.locationSource);setText('dashPorts','web '+j.webPort+' / alpaca '+j.alpacaPort+' / SkySafari WiFi '+j.lx200Port+' / Stellarium '+j.stellariumPort+' / BT '+(j.bluetoothEnabled?'on':'off'));setText('dashProto','Alpaca req '+(j.alpacaHttpRequests||0)+' / Sky WiFi RX '+j.lx200WifiRxCommands+' TX '+j.lx200WifiTxReplies+' / Sky BT RX '+btRx+' TX '+btTx+' / Stell RX '+j.stellariumRxPackets+' TX '+j.stellariumTxPackets);setText('dashFault',j.logAlert?j.logAlertText:(j.lastMountFault||'none'));let tl=id('timeConfidence');if(tl){let cls=j.timeValid&&j.siteValid?'ok':'bad';tl.className='msg '+cls;tl.textContent=(j.timeValid&&j.siteValid?'Time/location valid: ':'Time/location warning: ')+'time='+j.timeSource+', location='+j.locationSource+', offset='+j.utcOffsetMinutes+' minutes'}}async function refreshProtocolDash(){try{updateProtocolDash(await getJson('/status'))}catch(e){}}setInterval(refreshProtocolDash,8000);"));
  server.sendContent(F("let csOpen=null;function csClose(){if(csOpen){csOpen.classList.remove('open');csOpen=null}}function csPosition(h){let b=h.querySelector('.csBtn'),m=h.querySelector('.csMenu'),r=b.getBoundingClientRect(),vh=window.innerHeight||document.documentElement.clientHeight,spaceBelow=vh-r.bottom,spaceAbove=r.top,w=Math.max(r.width,180),mh=Math.min(360,Math.max(130,(spaceBelow>=180?spaceBelow-8:spaceAbove-8)));m.style.width=w+'px';m.style.maxHeight=mh+'px';m.style.left=Math.max(4,Math.min(r.left,(window.innerWidth||document.documentElement.clientWidth)-w-4))+'px';if(spaceBelow>=180||spaceBelow>=spaceAbove)m.style.top=(r.bottom+2)+'px';else m.style.top=Math.max(4,r.top-mh-2)+'px'}function csSync(sel){let h=sel&&sel._csHost;if(!h)return;let b=h.querySelector('.csBtn'),m=h.querySelector('.csMenu'),opts=Array.from(sel.options);b.textContent=sel.selectedIndex>=0?opts[sel.selectedIndex].textContent:'Select';b.disabled=sel.disabled;m.innerHTML='';opts.forEach((o,i)=>{let x=document.createElement('button');x.type='button';x.className='csOpt'+(o.disabled?' disabled':'')+(o.classList.contains('belowHorizon')?' belowHorizon':'')+(i===sel.selectedIndex?' sel':'');x.textContent=o.textContent;x.disabled=o.disabled;x.dataset.index=i;x.onclick=function(ev){ev.preventDefault();ev.stopPropagation();if(o.disabled)return;sel.selectedIndex=i;sel.dispatchEvent(new Event('change',{bubbles:true}));csSync(sel);csClose()};m.appendChild(x)})}function csEnhance(sel){if(!sel||sel._csHost)return;let h=document.createElement('div');h.className='csHost';sel.parentNode.insertBefore(h,sel);h.appendChild(sel);let b=document.createElement('button');b.type='button';b.className='csBtn';b.setAttribute('aria-haspopup','listbox');let m=document.createElement('div');m.className='csMenu';m.setAttribute('role','listbox');h.appendChild(b);h.appendChild(m);sel._csHost=h;b.onclick=function(ev){ev.preventDefault();ev.stopPropagation();if(csOpen&&csOpen!==h)csClose();let opening=!h.classList.contains('open');h.classList.toggle('open',opening);csOpen=opening?h:null;if(opening){csSync(sel);csPosition(h)}};new MutationObserver(function(){csSync(sel)}).observe(sel,{childList:true,subtree:true,attributes:true,characterData:true});sel.addEventListener('change',function(){csSync(sel)});csSync(sel)}function csInit(root){(root||document).querySelectorAll('select').forEach(csEnhance)}document.addEventListener('click',csClose);window.addEventListener('resize',function(){if(csOpen)csPosition(csOpen)});window.addEventListener('scroll',function(){if(csOpen)csPosition(csOpen)},true);document.addEventListener('DOMContentLoaded',function(){csInit(document);setInterval(function(){document.querySelectorAll('select').forEach(function(s){if(!s._csHost)csEnhance(s);else csSync(s)})},1000)});"));
  server.sendContent(F("</script><style>.csHost{position:relative;display:inline-block;width:100%;max-width:100%;margin:0}.csHost>select{position:absolute!important;left:-10000px!important;width:1px!important;height:1px!important;opacity:0!important;pointer-events:none!important}.csBtn{width:100%;margin:0!important;text-align:left;display:flex;align-items:center;justify-content:space-between;gap:.6em;min-height:2.45em;background:#0a1122!important;color:#edf2ff!important;border:1px solid #2b3654!important;border-radius:8px!important;font-weight:400!important}.csBtn:after{content:'';display:inline-block;flex:0 0 auto;width:.52em;height:.52em;border-right:2px solid currentColor;border-bottom:2px solid currentColor;transform:rotate(45deg);margin:0 .2em .25em .5em;opacity:.85}.csHost.open .csBtn:after{transform:rotate(225deg);margin:.25em .2em 0 .5em}.csMenu{display:none;position:fixed;z-index:10000;max-height:min(55vh,360px);overflow:auto;background:#0a1122;color:#edf2ff;border:1px solid #2b3654;border-radius:9px;box-shadow:0 8px 26px #000b;padding:4px;box-sizing:border-box;-webkit-overflow-scrolling:touch}.csHost.open .csMenu{display:block}.csOpt{display:block;width:100%;text-align:left;margin:0!important;padding:.62em .7em;border:0!important;border-radius:6px!important;background:#0a1122!important;color:#edf2ff!important;font-weight:400!important}.csOpt:hover,.csOpt:focus,.csOpt.sel{background:#174b80!important;color:#fff!important}.csOpt:disabled,.csOpt.disabled{background:#101729!important;color:#7f899f!important;opacity:1}.csOpt.belowHorizon{color:#7f899f!important}.themeLight .csBtn,.themeLight .csMenu,.themeLight .csOpt{background:#f8fafc!important;color:#172033!important;border-color:#aab4c2!important}.themeLight .csOpt:hover,.themeLight .csOpt:focus,.themeLight .csOpt.sel{background:#1769aa!important;color:#fff!important}.themeLight .csOpt:disabled,.themeLight .csOpt.disabled,.themeLight .csOpt.belowHorizon{background:#eef1f5!important;color:#8993a3!important}.themeNight .csBtn,.themeNight .csMenu,.themeNight .csOpt{background:#050000!important;color:#ff3a3a!important;border-color:#650000!important}.themeNight .csOpt:hover,.themeNight .csOpt:focus,.themeNight .csOpt.sel{background:#5a0000!important;color:#ff7070!important}.themeNight .csOpt:disabled,.themeNight .csOpt.disabled,.themeNight .csOpt.belowHorizon{background:#100000!important;color:#7f2020!important}.row .csHost{width:auto;min-width:10em}@media(max-width:699px){.row .csHost{display:block;width:100%;margin:6px 0}.csMenu{max-height:60vh}}.dash{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:8px;margin:8px 0}.dash .card{border-width:1px;padding:8px}.badge{display:inline-block;font-weight:bold;border-radius:7px;padding:3px 7px;margin:2px 4px 4px 0;background:#1b2742;border:1px solid #2b3654}.badge.ok{background:#123d2b;color:#8dffba}.badge.warn{background:#4b4210;color:#ffe58a}.badge.bad{background:#4b2630;color:#ff8d8d}.connBtns button{min-width:9.5em}.tight{min-height:0}</style></head><body><div class='topbar'><h2>"));
  server.sendContent(FW_NAME);
  server.sendContent(F(" "));
  server.sendContent(FW_VERSION);
  server.sendContent(F("</h2><div class='scaleCtl'><button id='nightModeBtn' class='nightBtn' onclick='toggleNightMode()'>Red</button><button id='themeLightBtn' onclick=\"setBaseTheme('light')\">Light</button><button id='themeDarkBtn' onclick=\"setBaseTheme('dark')\">Dark</button><button onclick=\"location.href='/reboot?return='+encodeURIComponent(location.href)\">Reboot</button><button onclick=\"location.href='/setmode?mode=bt&return='+encodeURIComponent(location.href)\">BT</button><button onclick='scaleUi(-0.1)'>-</button><span id='scalePct'>100%</span><button onclick='scaleUi(0.1)'>+</button></div></div><div class='quick'><span id='healthLine'>Mount Unknown</span><br>Current RA <span id='curRaHours'>?</span> h / Dec <span id='curDecDeg'>?</span> deg<br>Current Alt <span id='curAltDeg'>?</span> deg / Az <span id='curAzDeg'>?</span> deg<br>Time <span id='topDateTime'>?</span><br>Lat <span id='topLat'>?</span> / Lon <span id='topLon'>?</span><br><span id='topCpuLoad'>CPU Load --%</span><br><span id='logAlert' class='logAlert off'></span></div><div id='timeConfidence' class='msg'>Checking time/location confidence...</div><div class='dash'><div class='card tight'><h3>Mount</h3><span id='bMount' class='badge warn'>?</span><div id='dashMount' class='small'>Loading...</div></div><div class='card tight'><h3>Time / Location</h3><span id='bTime' class='badge warn'>?</span><span id='bLoc' class='badge warn'>?</span><div id='dashTime' class='small'>Loading...</div></div><div class='card tight'><h3>Protocols</h3><span id='bAlpaca' class='badge warn'>?</span><span id='bSky' class='badge warn'>?</span><span id='bStel' class='badge warn'>?</span><div id='dashProto' class='small'>Loading...</div></div><div class='card tight'><h3>Ports / Fault</h3><div id='dashPorts' class='small'>Loading...</div><div id='dashFault' class='small'>none</div></div></div><div class='tabs'><button id='btn_control' onclick='showTab(&quot;control&quot;)'>Control</button><button id='btn_status' onclick='showTab(&quot;status&quot;)'>Status</button><button id='btn_setup' onclick='showTab(&quot;setup&quot;)'>Setup</button><button id='btn_logs' onclick='showTab(&quot;logs&quot;)'>Logs</button><button onclick='startHttpsTimeLocation()'>GPS Sync</button></div><div class='grid'>"));
  server.sendContent(F("<div id='tab_control' class='tab active'>"));
  server.sendContent(F("<script>setTimeout(refreshProtocolDash,700);</script>"));
  server.sendContent(F("<div class='card'><h3>Catalog Goto</h3><div id='catMsg' class='msg'></div><div class='formrow'><label>Search all objects</label><input id='cat_search' oninput='catPopulate()' placeholder='Moon, Jupiter, M31, Rosette, Vega'></div><div class='formrow'><label>Group</label><select id='cat_group'><option value='solar'>Planetary / Solar System</option><option value='messier'>Deep Sky / Messier+</option><option value='galaxy'>Galaxies</option><option value='nebula'>Nebulae</option><option value='planetary_nebula'>Planetary Nebulae</option><option value='globular'>Globular Clusters</option><option value='open'>Open Clusters</option><option value='stars'>Bright Stars</option><option value='bsc'>BSC5 Bright Stars</option></select></div><div class='formrow'><label>Sort</label><select id='cat_sort' onchange='catPopulate()'><option value='brightness' selected>Brightness</option><option value='visibility'>Visibility / altitude</option><option value='name'>Name</option></select></div><div class='formrow'><label>Selected</label><select id='cat_obj' onchange='catUpdate()'></select></div><div class='formrow'><label>Current</label><input id='pointingIdBox' readonly value='not checked yet' style='width:100%;max-width:100%;'></div><div class='formrow'><label>Nearby radius deg</label><input id='nearRadius' value='10' style='width:70px' oninput='lastPointingIdKey=&quot;&quot;;identifyPointingNow()' onchange='lastPointingIdKey=&quot;&quot;;identifyPointingNow()'></div><div class='formrow'><label>Nearby object</label><select id='nearestSelect' onchange='selectNearestCandidate()'><option value='' selected>Select a nearby object</option></select></div><div class='hint'>Nearby shown: <span id='nearCount'>0</span></div><div class='actions'><button onclick='catSlew()'>Slew to Object</button></div></div>"));
  server.sendContent(F("<script>function catGroupSelected(){let s=id('cat_search');if(s&&s.value)s.value='';let g=id('cat_group');if(g&&g.value==='bsc'&&!catBSCLoaded)loadBSC5().then(()=>catPopulate());catPopulate()}setTimeout(function(){let g=id('cat_group');if(g&&!g.dataset.bound){g.addEventListener('change',catGroupSelected);g.dataset.bound='1'}if(typeof catPopulate==='function')catPopulate();},250);</script>"));
  server.sendContent(F("<style>#cat_obj option.belowHorizon,#nearestSelect option.belowHorizon{color:#8a8a8a;background:#1a1a1a}.themeLight #cat_obj option.belowHorizon,.themeLight #nearestSelect option.belowHorizon{color:#777;background:#e7e9ee}.themeNight #cat_obj option.belowHorizon,.themeNight #nearestSelect option.belowHorizon{color:#8c3030;background:#120000}</style><script>function catIsBelowHorizon(c){let aa=c?catAltAzFromRaDec(c.ra,c.dec):null;return !!(aa&&isFinite(aa.alt)&&aa.alt<0)}function catDecoratedOptionText(x){return catOptionText(x)}function catPopulate(){setText('bscCount',String(catBscCount()));let o=id('cat_obj');if(!o)return;let arr=catArray(),old=o.value;o.innerHTML='';arr.forEach(x=>{let op=document.createElement('option'),c=x.src==='solar'?simpleSolar(x.id):x,below=catIsBelowHorizon(c);op.value=(x.src||'dso')+':'+x.id;op.textContent=catDecoratedOptionText(x);if(below){op.className='belowHorizon';op.dataset.below='1';op.style.color='#777';op.style.backgroundColor='#222'}o.appendChild(op)});setText('catShown',String(arr.length));setText('catTotal',String(window.catTotalBeforeLimit||arr.length));if(old){for(let i=0;i<o.options.length;i++){if(o.options[i].value===old){o.selectedIndex=i;break}}}if(o.selectedIndex<0&&o.options.length>0)o.selectedIndex=0;catUpdate()}function catRefreshLabels(){let o=id('cat_obj');if(!o)return;let arr=catArray(),m={};arr.forEach(x=>{m[(x.src||'dso')+':'+x.id]=catDecoratedOptionText(x)});for(let i=0;i<o.options.length;i++){let v=o.options[i].value;if(m[v])o.options[i].textContent=m[v]}}function catUpdate(){let c=catSelected();if(!c){catSetInfo('RA ? / Dec ?<br>Sky Alt ? / Az ?<br>Mount Alt ? / Az ?');return}let aa=catAltAzFromRaDec(c.ra,c.dec),below=catIsBelowHorizon(c);let coord=(below?'Below horizon<br>':'')+'RA '+hms(c.ra)+' h / Dec '+dms(c.dec)+' deg<br>Sky Alt '+(aa?Number(aa.alt).toFixed(2):'?')+' deg / Az '+(aa?Number(aa.az).toFixed(2):'?')+' deg';coord+='<br>Mount Alt '+(lastStatus&&lastStatus.altAzCacheValid?Number(lastStatus.altDeg).toFixed(2):'?')+' deg / Az '+(lastStatus&&lastStatus.altAzCacheValid?Number(lastStatus.azDeg).toFixed(2):'?')+' deg';if(c.approx)coord+=' APPROX ephemeris';catSetInfo(coord)}</script>"));
  server.sendContent(F("<div class='card'><h3>Manual Nudge</h3><div id='mainMsg' class='msg'></div><div class='formrow'><label>Rate</label><select id='rateSel' onchange='setRate()'><option value='1'>Rate 1</option><option value='2'>Rate 2</option><option value='3'>Rate 3</option><option value='4'>Rate 4</option></select></div><div class='pad'><div><button onclick=\"nudge('n')\">Up</button></div><div><button onclick=\"nudge('w')\">Left</button><button onclick=\"nudge('e')\">Right</button></div><div><button onclick=\"nudge('s')\">Down</button></div></div></div>"));

  server.sendContent(F("<div class='card'><h3>GOTO RA/Dec</h3><div id='gotoMsg' class='msg'></div><div class='formrow'><label>Target RA hours</label><input id='ra_hours'></div><div class='formrow'><label>Target Dec deg</label><input id='dec_deg'></div><div class='actions'><button onclick='gotoRaDec()'>GOTO RA/Dec</button><button onclick='readRaDec()'>Load Current RA/Dec</button></div></div>"));

  server.sendContent(F("<div class='card'><h3>GOTO Alt/Az</h3><div id='altazMsg' class='msg'></div><div class='formrow'><label>Target Alt deg</label><input id='alt_deg'></div><div class='formrow'><label>Target Az deg</label><input id='az_deg'></div><div class='actions'><button onclick='gotoAltAz()'>GOTO Alt/Az</button><button onclick='readAltAz()'>Load Current Alt/Az</button></div></div>"));
  server.sendContent(F("</div>"));

  server.sendContent(F("<div id='tab_status' class='tab'>"));
  server.sendContent(F("<div class='actions statusToolbar' style='grid-column:1/-1;margin-top:0'><button id='statusModeBtn' class='statusModeBtn' onclick='toggleStatusMode()'>Advanced</button></div><div id='statusBasicWrap' class='statusPair'><div id='statusCard' class='card status idle'><h3>Observer Status</h3><pre id='statusBasicBox'>Loading...</pre></div><div class='card'><h3>System Health</h3><pre id='sysBasicBox'>Loading...</pre></div></div><div id='statusAdvancedWrap' class='statusPair' style='display:none'><div id='statusCardAdvanced' class='card'><h3>Observer Status - Advanced</h3><pre id='statusBox'>Loading...</pre></div><div class='card'><h3>System Health - Advanced</h3><pre id='sysBox'>Loading...</pre></div></div>"));
  server.sendContent(F("</div>"));

  server.sendContent(F("<div id='tab_setup' class='tab'>"));
  server.sendContent(F("<div class='card'><h3>Site / Time / Polling</h3><div id='siteMsg' class='msg'></div><div class='formrow'><label>Latitude</label><input id='lat'></div><div class='formrow'><label>Longitude east-positive</label><input id='lon'></div><div class='formrow'><label>UTC offset minutes</label><input id='offset'></div><div class='formrow'><label>Date</label><div><input id='year' style='width:78px'> - <input id='month' style='width:58px'> - <input id='day' style='width:58px'></div></div><div class='formrow'><label>Time</label><div><input id='hour' style='width:58px'> : <input id='minute' style='width:58px'> : <input id='second' style='width:58px'></div></div><div class='formrow'><label>Active poll interval ms</label><input id='poll'></div><div class='formrow'><label>Idle poll interval ms</label><input id='idlepoll'></div><div class='formrow'><label>Client throttle ms</label><input id='throttle'></div><div class='actions'><button onclick='saveManualSiteTime()'>Save Site / Time / Polling</button><button onclick='startHttpsTimeLocation()'>GPS Sync</button><button onclick='clearSavedSite()'>Clear Saved Last Location</button></div><div class='hint'>Active polling starts after SkySafari, Stellarium, Alpaca, or the status page requests position. Idle polling is used otherwise.</div></div>"));
  server.sendContent(F("<div class='card'><h3>Time / NTP</h3><div id='ntpMsg' class='msg'></div><div class='formrow'><label>Enable NTP</label><label style='font-weight:normal'><input type='checkbox' id='ntp_enable'></label></div><div class='formrow'><label>NTP server 1</label><input id='ntp1' value='pool.ntp.org'></div><div class='formrow'><label>NTP server 2</label><input id='ntp2' value='time.nist.gov'></div><div class='formrow'><label>TZ / DST rule</label><input id='tzrule' value='MST7MDT,M3.2.0/2,M11.1.0/2'></div><div class='actions'><button onclick='saveNtp()'>Save NTP Settings</button><button onclick='syncNtp()'>Sync NTP Now</button></div><div class='hint'>Uses STA WiFi or phone hotspot internet. Default TZ is US Mountain with DST.</div></div>"));
  server.sendContent(F("<div class='card'><h3>Approx Internet Location</h3><div id='ipLocMsg' class='msg'></div><p class='small'>Uses public IP geolocation. City-level only; verify before precision alignment.</p>Status: <span id='ipLocStatus'>Not fetched</span><br>Lat/Lon: <span id='ipLocLatLon'>none</span><br><span id='ipLocText' class='small'></span><br><button onclick='fetchIpLoc()'>Get Approx Location</button><button onclick='useIpLoc()'>Use This Location</button></div>"));
  server.sendContent(F("<div class='card'><h3>STA WiFi Setup</h3><div id='wifiMsg' class='msg'></div><div class='formrow'><label>STA SSID</label><input id='wifi_ssid'></div><div class='formrow'><label>STA Password</label><input id='wifi_pass' type='text' value='"));
  server.sendContent(htmlEscape(staPass));
  server.sendContent(F("'></div><div class='actions'><button onclick='saveWifi()'>Save / Connect STA</button><button onclick='clearWifi()'>Reset WiFi To AP Only</button></div><div class='hint'>When STA is configured, the unit runs STA-only. If STA fails, it falls back to AP-only.</div></div>"));
  server.sendContent(F("<div class='card'><h3>AP Setup</h3><div id='apMsg' class='msg'></div><div class='formrow'><label>AP SSID</label><input id='ap_ssid'></div><div class='formrow'><label>AP Password</label><input id='ap_pass' type='text'></div><div class='formrow'><label>AP IP</label><input id='ap_ip'></div><div class='actions'><button onclick='saveAp()'>Save AP / Server Config</button></div><div class='hint'>Password may be blank/open or at least 8 characters. The AP SSID box shows the full broadcast name.</div></div>"));
  server.sendContent(F("<div class='card'><h3>Nudge Rates</h3><div id='settingsMsg' class='msg'></div><div class='formrow'><label>Rate 1 deg</label><input id='r1'></div><div class='formrow'><label>Rate 2 deg</label><input id='r2'></div><div class='formrow'><label>Rate 3 deg</label><input id='r3'></div><div class='formrow'><label>Rate 4 deg</label><input id='r4'></div><div class='actions'><button onclick='saveRates()'>Save Rates</button><button onclick='resetRates()'>Reset Defaults</button></div></div>")); 
  server.sendContent(F("<div class='card'><h3>Altitude Safety Limits</h3><div id='altLimitMsg' class='msg'></div><div class='formrow'><label>Enable limit</label><label style='font-weight:normal'><input type='checkbox' id='alt_limit_enable'></label></div><div class='formrow'><label>Minimum Alt deg</label><input id='alt_min' value='0'></div><div class='formrow'><label>Maximum Alt deg</label><input id='alt_max' value='85'></div><div class='actions'><button onclick='saveAltLimits()'>Save Alt Limits</button></div><div class='hint'>Blocks targets too low or too high. Use this to prevent near-zenith tube/mount clearance problems, such as Alt 86 deg.</div></div>"));
  server.sendContent(F("<div class='card'><h3>Connection / Server Config</h3><div id='connCfgMsg' class='msg'></div><div class='hint'>These are saved configuration values. Port changes automatically restart the device.</div><div class='formrow'><label>Web UI HTTP port</label><input id='web_port' disabled></div><div class='formrow'><label>Alpaca HTTP port</label><input id='alpaca_port' disabled></div><div class='formrow'><label>Alpaca Discovery UDP port</label><input id='alpaca_discovery_port' disabled></div><div class='formrow'><label>SkySafari LX200 port</label><input id='lx200_port'></div><div class='formrow'><label>Stellarium port</label><input id='stellarium_port'></div><div class='formrow'><label>Telnet console port</label><input id='telnet_port'></div><div class='formrow'><label>Telnet password</label><input id='telnet_pass' type='text' placeholder='blank disables password'></div><div class='actions'><button onclick='saveConnCfg()'>Save Server Config</button><button onclick='defaultConnCfg()'>Defaults</button></div><div class='hint'>Web UI uses port 80. Alpaca uses its own HTTP listener on port 11111. Discovery uses the separate UDP port shown above. Telnet default is port 23; password changes require reconnect, port changes restart the device.</div></div>"));
  server.sendContent(F("</div>"));

  server.sendContent(F("<div id='tab_logs' class='tab' style='grid-template-columns:1fr'>"));
  server.sendContent(F("<div class='card logs'><h3>Logs</h3><p class='hint'>Log level: 0 none, 1 error, 2 warn, 3 info, 4 debug, 5 trace. Systems select which firmware areas are shown. The serial monitor mirrors the same timestamped, filtered log lines.</p><div id='logsMsg' class='msg'></div>Log Level<br><select id='logLevelSel' onchange='applyLogSettings()'><option value='error'>error</option><option value='warn' selected>warn</option><option value='info'>info</option><option value='debug'>debug</option><option value='trace'>trace</option><option value='none'>none</option></select><div class='loggrid'><label><input class='logcat' type='checkbox' value='mount' checked onchange='applyLogSettings()'><span>mount</span></label><label><input class='logcat' type='checkbox' value='skysafari' checked onchange='applyLogSettings()'><span>SkySafari</span></label><label><input class='logcat' type='checkbox' value='bluetooth' checked onchange='applyLogSettings()'><span>Bluetooth</span></label><label><input class='logcat' type='checkbox' value='alpaca' checked onchange='applyLogSettings()'><span>Alpaca</span></label><label><input class='logcat' type='checkbox' value='stellarium' checked onchange='applyLogSettings()'><span>Stellarium</span></label><label><input class='logcat' type='checkbox' value='web' checked onchange='applyLogSettings()'><span>web</span></label><label><input class='logcat' type='checkbox' value='wifi' checked onchange='applyLogSettings()'><span>WiFi</span></label><label><input class='logcat' type='checkbox' value='timeloc' checked onchange='applyLogSettings()'><span>time/location</span></label><label><input class='logcat' type='checkbox' value='settings' checked onchange='applyLogSettings()'><span>settings</span></label><label><input class='logcat' type='checkbox' value='system' checked onchange='applyLogSettings()'><span>system</span></label></div><div class='row'><button id='logsLiveBtn' onclick='toggleLogsLive()'>Pause Logs</button><button onclick='clearLogs();refreshLogs()'>Clear Logs</button></div><p class='small'>Newest entries appear at the top. Pause stops updating the window, not internal logging.</p><pre id='logBox'>Waiting for logs...</pre></div>"));
  server.sendContent(F("</div>"));

  server.sendContent(F("</div></body></html>"));
#if defined(ESP32)
  // ESP32 WebServer needs the final zero-length chunk when CONTENT_LENGTH_UNKNOWN is used.
  server.sendContent("");
#endif

  server.sendContent("");
}

void handleLogsPage() {
  logHttpRequest("LOGS");
  String logs = getLogText();
  if (logs.length() == 0) logs = "No logs yet, or current log level/subsystem settings are filtering them before storage.\n";
  sendPlain(logs);
}

void handleStatusTextPage() {
  logHttpRequest("STATUS_TEXT");
  sendPlain(currentStateText());
}

void handleSystemTextPage() {
  logHttpRequest("SYS_TEXT");
  sendPlain(systemHealthText());
}

void handleBtStatusPage() {
  logHttpRequest("BT_STATUS");
  bool btConnected = false;
#if HAS_CLASSIC_BT
  btConnected = bluetoothRuntimeEnabled && SerialBT.hasClient();
#endif
  int dispYear, dispMonth, dispDay, dispHour, dispMinute, dispSecond;
  currentLocalParts(dispYear, dispMonth, dispDay, dispHour, dispMinute, dispSecond);

  String json;
  json.reserve(1100);
  json += "{";
  json += "\"bridgeMode\":\"" + jsonEscape(String(bridgeModeName())) + "\",";
  json += "\"apIp\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"staIp\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"staConnected\":" + String((staConnected && WiFi.status() == WL_CONNECTED) ? "true" : "false") + ",";
  json += "\"bluetoothEnabled\":" + String(bluetoothRuntimeEnabled ? "true" : "false") + ",";
  json += "\"bluetoothConnected\":" + String(btConnected ? "true" : "false") + ",";
  json += "\"lx200BtRxCommands\":" + String(lx200BtRxCommands) + ",";
  json += "\"lx200BtTxReplies\":" + String(lx200BtTxReplies) + ",";
  json += "\"lx200BtLastCommand\":\"" + jsonEscape(lx200BtLastCommand) + "\",";
  json += "\"lx200BtLastReply\":\"" + jsonEscape(lx200BtLastReply) + "\",";
  json += "\"lx200BtLastUnhandled\":\"" + jsonEscape(lx200BtLastUnhandled) + "\",";
  json += "\"timeValid\":" + String(timeValid ? "true" : "false") + ",";
  json += "\"timeSource\":\"" + jsonEscape(String(timeSourceName(currentTimeSource))) + "\",";
  json += "\"localYear\":" + String(dispYear) + ",";
  json += "\"localMonth\":" + String(dispMonth) + ",";
  json += "\"localDay\":" + String(dispDay) + ",";
  json += "\"localHour\":" + String(dispHour) + ",";
  json += "\"localMinute\":" + String(dispMinute) + ",";
  json += "\"localSecond\":" + String(dispSecond) + ",";
  json += "\"siteValid\":" + String(siteValid ? "true" : "false") + ",";
  json += "\"locationSource\":\"" + jsonEscape(currentLocationSource) + "\",";
  json += "\"latitude\":" + String(siteLatitudeDeg, 6) + ",";
  json += "\"longitude\":" + String(siteLongitudeDeg, 6) + ",";
  json += "\"mountBusy\":" + String(mountBusy ? "true" : "false") + ",";
  json += "\"mountCommFault\":" + String(mountCommFault ? "true" : "false") + ",";
  json += "\"mountUptime\":\"" + jsonEscape(formatMountUptime()) + "\",";
  json += "\"pollIntervalMs\":" + String(pollIntervalMs) + ",";
  json += "\"idlePollIntervalMs\":" + String(idlePollIntervalMs) + ",";
  json += "\"freeHeap\":" + String(ESP.getFreeHeap());
  json += "}";
  sendNoCacheHeaders();
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", json);
}

void handleStatusPage() {
  logHttpRequest("STATUS");
  if (bridgeMode == BRIDGE_MODE_BT_MIN_WEB) {
    handleBtStatusPage();
    return;
  }
  if (bridgeMode == BRIDGE_MODE_BT_MIN_WEB) {
    bool btConnected = false;
#if HAS_CLASSIC_BT
    btConnected = bluetoothRuntimeEnabled && SerialBT.hasClient();
#endif
    int dispYear, dispMonth, dispDay, dispHour, dispMinute, dispSecond;
    currentLocalParts(dispYear, dispMonth, dispDay, dispHour, dispMinute, dispSecond);

    String json;
    json.reserve(1200);
    json += "{";
    json += "\"board\":\"" + String(BOARD_NAME) + "\",";
    json += "\"bridgeMode\":\"" + jsonEscape(String(bridgeModeName())) + "\",";
    json += "\"apIp\":\"" + WiFi.softAPIP().toString() + "\",";
    json += "\"staIp\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"lastStaIp\":\"" + jsonEscape(lastStaIp) + "\",";
    json += "\"staConnected\":" + String((staConnected && WiFi.status() == WL_CONNECTED) ? "true" : "false") + ",";
    json += "\"webPort\":" + String(HTTP_WEB_PORT) + ",";
    json += "\"alpacaPort\":0,";
    json += "\"alpacaDiscoveryPort\":0,";
    json += "\"lx200Port\":0,";
    json += "\"stellariumPort\":0,";
    json += "\"bluetoothEnabled\":" + String(bluetoothRuntimeEnabled ? "true" : "false") + ",";
    json += "\"bluetoothConnected\":" + String(btConnected ? "true" : "false") + ",";
    json += "\"coexPreference\":\"" + jsonEscape(coexPreferenceText) + "\",";
    json += "\"coexPreferenceResult\":" + String(coexPreferenceResult) + ",";
    json += "\"lx200BtRxCommands\":" + String(lx200BtRxCommands) + ",";
    json += "\"lx200BtTxReplies\":" + String(lx200BtTxReplies) + ",";
    json += "\"lx200BtUnhandled\":" + String(lx200BtUnhandledCommands) + ",";
  json += "\"lx200CommonRouterCommands\":" + String(lx200CommonRouterCommands) + ",";
    json += "\"lx200BtLastCommand\":\"" + jsonEscape(lx200BtLastCommand) + "\",";
    json += "\"lx200BtLastReply\":\"" + jsonEscape(lx200BtLastReply) + "\",";
    json += "\"lx200BtLastUnhandled\":\"" + jsonEscape(lx200BtLastUnhandled) + "\",";
    json += "\"timeValid\":" + String(timeValid ? "true" : "false") + ",";
    json += "\"timeSource\":\"" + jsonEscape(String(timeSourceName(currentTimeSource))) + "\",";
    json += "\"localYear\":" + String(dispYear) + ",";
    json += "\"localMonth\":" + String(dispMonth) + ",";
    json += "\"localDay\":" + String(dispDay) + ",";
    json += "\"localHour\":" + String(dispHour) + ",";
    json += "\"localMinute\":" + String(dispMinute) + ",";
    json += "\"localSecond\":" + String(dispSecond) + ",";
    json += "\"utcOffsetMinutes\":" + String(utcOffsetMinutes) + ",";
    json += "\"timeZoneLabel\":\"" + jsonEscape(currentTimezoneAbbrev()) + "\",";
    json += "\"siteValid\":" + String(siteValid ? "true" : "false") + ",";
    json += "\"locationSource\":\"" + jsonEscape(currentLocationSource) + "\",";
    json += "\"latitude\":" + String(siteLatitudeDeg, 6) + ",";
    json += "\"longitude\":" + String(siteLongitudeDeg, 6) + ",";
    json += "\"mountBusy\":" + String(mountBusy ? "true" : "false") + ",";
    json += "\"mountCommFault\":" + String(mountCommFault ? "true" : "false") + ",";
    json += "\"cacheValid\":" + String(mountCurrentRaDecValid ? "true" : "false") + ",";
    json += "\"raDeg\":" + String(mountCurrentRA_deg, 6) + ",";
    json += "\"decDeg\":" + String(mountCurrentDec_deg, 6) + ",";
    json += "\"raHours\":" + String(mountCurrentRA_deg / 15.0, 6) + ",";
    json += "\"altAzCacheValid\":" + String(mountCurrentAltAzValid ? "true" : "false") + ",";
    json += "\"altDeg\":" + String(mountCurrentAlt_deg, 6) + ",";
    json += "\"azDeg\":" + String(mountCurrentAz_deg, 6) + ",";
    json += "\"mountCurrentRaDecAgeMs\":" + String(mountCurrentRaDecValid ? millis() - mountCurrentRaDecMs : 0) + ",";
    json += "\"mountCurrentAltAzAgeMs\":" + String(mountCurrentAltAzValid ? millis() - mountCurrentAltAzMs : 0) + ",";
    json += "\"pollIntervalMs\":" + String(pollIntervalMs) + ",";
    json += "\"idlePollIntervalMs\":" + String(idlePollIntervalMs) + ",";
    json += "\"btPostGotoFastPoll\":" + String((lastGotoAcceptedMs && millis() - lastGotoAcceptedMs < BT_POST_GOTO_FAST_POLL_WINDOW_MS) ? "true" : "false") + ",";
    json += "\"freeHeap\":" + String(ESP.getFreeHeap());
    json += "}";
    sendNoCacheHeaders();
    server.sendHeader("Connection", "close");
    server.send(200, "application/json", json);
    return;
  }
  // Status pages do not directly poll the mount. Current-position fields are
  // supplied by the demand-based independent mount poller.

  bool btConnected = false;
#if HAS_CLASSIC_BT
  btConnected = bluetoothRuntimeEnabled && SerialBT.hasClient();
#endif
  bool fullProtocolMode = (bridgeMode == BRIDGE_MODE_WIFI_FULL);

  int dispYear, dispMonth, dispDay, dispHour, dispMinute, dispSecond;
  currentLocalParts(dispYear, dispMonth, dispDay, dispHour, dispMinute, dispSecond);

  String st = currentStateText();
  String sys = systemHealthText();

  String json;
  json.reserve(5000);
  json += "{";
  json += "\"board\":\"" + String(BOARD_NAME) + "\",";
  json += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"apIp\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"staIp\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"lastStaIp\":\"" + jsonEscape(lastStaIp) + "\",";
  json += "\"staSsid\":\"" + jsonEscape(staSsid) + "\",";
  json += "\"staConfigured\":" + String(staConfigured ? "true" : "false") + ",";
  json += "\"staConnected\":" + String((staConnected && WiFi.status() == WL_CONNECTED) ? "true" : "false") + ",";
  json += "\"wifiMode\":\"" + jsonEscape(wifiModeText) + "\",";
  json += "\"wifiStatus\":\"" + jsonEscape(lastWifiStatus) + "\",";
  json += "\"alpacaConnected\":" + String((fullProtocolMode && alpacaConnected) ? "true" : "false") + ",";
  json += "\"alpacaHttpRequests\":" + String(fullProtocolMode ? alpacaHttpRequests : 0) + ",";
  json += "\"stellariumConnected\":" + String((fullProtocolMode && stellariumClientConnected) ? "true" : "false") + ",";
  json += "\"stellariumRxPackets\":" + String(fullProtocolMode ? stellariumRxPackets : 0) + ",";
  json += "\"stellariumTxPackets\":" + String(fullProtocolMode ? stellariumTxPackets : 0) + ",";
  json += "\"stellariumBadPackets\":" + String(fullProtocolMode ? stellariumBadPackets : 0) + ",";
  json += "\"stellariumLastRxAgeMs\":" + String((fullProtocolMode && stellariumLastRxMs) ? millis() - stellariumLastRxMs : 0) + ",";
  json += "\"stellariumLastTxAgeMs\":" + String((fullProtocolMode && stellariumLastTxMs) ? millis() - stellariumLastTxMs : 0) + ",";
  json += "\"lx200WifiConnected\":" + String((fullProtocolMode && lx200WifiClientConnected) ? "true" : "false") + ",";
  json += "\"lx200WifiRxCommands\":" + String(fullProtocolMode ? lx200WifiRxCommands : 0) + ",";
  json += "\"lx200WifiTxReplies\":" + String(fullProtocolMode ? lx200WifiTxReplies : 0) + ",";
  json += "\"lx200WifiUnhandled\":" + String(fullProtocolMode ? lx200WifiUnhandledCommands : 0) + ",";
  json += "\"lx200WifiLastRxAgeMs\":" + String((fullProtocolMode && lx200WifiLastRxMs) ? millis() - lx200WifiLastRxMs : 0) + ",";
  json += "\"lx200WifiLastTxAgeMs\":" + String((fullProtocolMode && lx200WifiLastTxMs) ? millis() - lx200WifiLastTxMs : 0) + ",";
  json += "\"lx200BtConnected\":" + String(lx200BtClientConnected ? "true" : "false") + ",";
  json += "\"lx200BtRxCommands\":" + String(lx200BtRxCommands) + ",";
  json += "\"lx200BtTxReplies\":" + String(lx200BtTxReplies) + ",";
  json += "\"lx200BtUnhandled\":" + String(lx200BtUnhandledCommands) + ",";
  json += "\"timeSource\":\"" + jsonEscape(String(timeSourceName(currentTimeSource))) + "\",";
  json += "\"locationSource\":\"" + jsonEscape(currentLocationSource) + "\",";
  json += "\"approxIpLocationValid\":" + String(approxIpLocationValid ? "true" : "false") + ",";
  json += "\"approxIpLat\":" + String(approxIpLatitudeDeg, 6) + ",";
  json += "\"approxIpLon\":" + String(approxIpLongitudeDeg, 6) + ",";
  json += "\"approxIpLocationStatus\":\"" + jsonEscape(approxIpLocationStatus) + "\",";
  json += "\"approxIpLocationText\":\"" + jsonEscape(approxIpLocationText) + "\",";
  json += "\"ntpEnabled\":" + String(ntpEnabled ? "true" : "false") + ",";
  json += "\"ntpServer1\":\"" + jsonEscape(String(ntpServer1)) + "\",";
  json += "\"ntpServer2\":\"" + jsonEscape(String(ntpServer2)) + "\",";
  json += "\"tzRule\":\"" + jsonEscape(String(tzRule)) + "\",";
  json += "\"apSsid\":\"" + jsonEscape(runtimeApSsid()) + "\",";
  json += "\"apPass\":\"" + jsonEscape(apPass) + "\",";
  json += "\"apIpConfig\":\"" + jsonEscape(apIp) + "\",";
  json += "\"webPort\":" + String(HTTP_WEB_PORT) + ",";
  json += "\"alpacaPort\":" + String(fullProtocolMode ? ALPACA_PORT : 0) + ",";
  json += "\"alpacaDiscoveryPort\":" + String(fullProtocolMode ? ALPACA_DISCOVERY_PORT : 0) + ",";
  json += "\"lx200Port\":" + String(fullProtocolMode ? LX200_PORT : 0) + ",";
  json += "\"stellariumPort\":" + String(fullProtocolMode ? STELLARIUM_PORT : 0) + ",";
  json += "\"telnetPort\":" + String(fullProtocolMode ? TELNET_PORT : 0) + ",";
  json += "\"telnetPasswordSet\":" + String(telnetPassword.length() ? "true" : "false") + ",";
  json += "\"telnetClientConnected\":" + String((telnetClient && telnetClient.connected()) ? "true" : "false") + ",";
  json += "\"telnetAuthenticated\":" + String(telnetAuthenticated ? "true" : "false") + ",";
  json += "\"telnetRxCommands\":" + String(telnetRxCommands) + ",";
  json += "\"telnetAuthFailures\":" + String(telnetAuthFailures) + ",";
  json += "\"bridgeMode\":\"" + jsonEscape(String(bridgeModeName())) + "\",";
  json += "\"bluetoothEnabled\":" + String(bluetoothRuntimeEnabled ? "true" : "false") + ",";
  json += "\"bluetoothConnected\":" + String(btConnected ? "true" : "false") + ",";
  json += "\"logLevel\":" + String(LOG_LEVEL) + ",";
  json += "\"logLevelName\":\"" + String(logLevelName(LOG_LEVEL)) + "\",";
  json += "\"logCategoryMask\":" + String(LOG_SUBSYSTEM_MASK) + ",";
  json += "\"cacheValid\":" + String(mountCurrentRaDecValid ? "true" : "false") + ",";
  json += "\"raDeg\":" + String(mountCurrentRA_deg, 6) + ",";
  json += "\"decDeg\":" + String(mountCurrentDec_deg, 6) + ",";
  json += "\"raHours\":" + String(mountCurrentRA_deg / 15.0, 6) + ",";
  json += "\"altAzCacheValid\":" + String(mountCurrentAltAzValid ? "true" : "false") + ",";
  json += "\"altAzComputed\":false,";
  json += "\"altDeg\":" + String(mountCurrentAlt_deg, 6) + ",";
  json += "\"azDeg\":" + String(mountCurrentAz_deg, 6) + ",";
  json += "\"mountCurrentRaDecAgeMs\":" + String(mountCurrentRaDecValid ? millis() - mountCurrentRaDecMs : 0) + ",";
  json += "\"mountCurrentAltAzAgeMs\":" + String(mountCurrentAltAzValid ? millis() - mountCurrentAltAzMs : 0) + ",";
  json += "\"siteValid\":" + String(siteValid ? "true" : "false") + ",";
  json += "\"timeValid\":" + String(timeValid ? "true" : "false") + ",";
  json += "\"latitude\":" + String(siteLatitudeDeg, 6) + ",";
  json += "\"longitude\":" + String(siteLongitudeDeg, 6) + ",";
  json += "\"utcOffsetMinutes\":" + String(utcOffsetMinutes) + ",";
  json += "\"localYear\":" + String(dispYear) + ",";
  json += "\"localMonth\":" + String(dispMonth) + ",";
  json += "\"localDay\":" + String(dispDay) + ",";
  json += "\"localHour\":" + String(dispHour) + ",";
  json += "\"localMinute\":" + String(dispMinute) + ",";
  json += "\"localSecond\":" + String(dispSecond) + ",";
  json += "\"pollIntervalMs\":" + String(pollIntervalMs) + ",";
  json += "\"idlePollIntervalMs\":" + String(idlePollIntervalMs) + ",";
  json += "\"effectivePollIntervalMs\":" + String(effectiveMountPollIntervalMs()) + ",";
  json += "\"positionDemandActive\":" + String(positionDemandActive() ? "true" : "false") + ",";
  json += "\"btPostGotoFastPoll\":" + String((bridgeMode == BRIDGE_MODE_BT_MIN_WEB && lastGotoAcceptedMs && millis() - lastGotoAcceptedMs < BT_POST_GOTO_FAST_POLL_WINDOW_MS) ? "true" : "false") + ",";
  json += "\"backgroundPollFailCount\":" + String(backgroundPollFailCount) + ",";
  json += "\"backgroundPollingAutoDisabled\":false,";
  json += "\"mountPollingPolicy\":\"independent_interval_only\",";
  json += "\"positionApiPolicy\":\"applications_read_cache_only\",";
  json += "\"positionApiCacheReplies\":" + String(positionApiCacheReplies) + ",";
  json += "\"positionApiCacheMisses\":" + String(positionApiCacheMisses) + ",";
  json += "\"gotoQueueActive\":" + String(hasQueuedGoto() ? "true" : "false") + ",";
  json += "\"gotoQueueType\":\"" + String(queuedGotoTypeName(queuedGotoType)) + "\",";
  json += "\"gotoQueueAgeMs\":" + String(hasQueuedGoto() ? millis() - queuedGotoMs : 0) + ",";
  json += "\"gotoQueueTimeoutMs\":" + String(gotoQueueTimeoutMs) + ",";
  json += "\"gotoQueueEffectiveTimeoutMs\":" + String(effectiveGotoQueueTimeoutMs()) + ",";
  json += "\"gotoQueueAccepted\":" + String(gotoQueueAccepted) + ",";
  json += "\"gotoQueueStarted\":" + String(gotoQueueStarted) + ",";
  json += "\"gotoQueueTimedOut\":" + String(gotoQueueTimedOut) + ",";
  json += "\"gotoQueueReplaced\":" + String(gotoQueueReplaced) + ",";
  json += "\"nudgeGotoQueueRequests\":" + String(nudgeGotoQueueRequests) + ",";
  json += "\"gotoQueueImmediateAcks\":" + String(gotoQueueImmediateAcks) + ",";
  json += "\"queuedGotoPositionCacheReplies\":" + String(queuedGotoPositionCacheReplies) + ",";
  json += "\"clientThrottleMs\":" + String(minClientPollIntervalMs) + ",";
  json += "\"persistentSaveCount\":" + String(persistentSaveCount) + ",";
  json += "\"lastPersistentSaveAgeMs\":" + String(lastPersistentSaveMs == 0 ? 0 : millis() - lastPersistentSaveMs) + ",";
  json += "\"logAlert\":" + String(logAlertActive ? "true" : "false") + ",";
  json += "\"logAlertText\":\"" + jsonEscape(logAlertText) + "\",";
  json += "\"mountBusy\":" + String((mountBusy || asyncSlewPending || asyncAltAzSlewPending || asyncNudgePending || asyncRaDecReadPending || asyncAltAzReadPending || asyncSlewRunning) ? "true" : "false") + ",";
  json += "\"mountAlive\":" + String(mountAlive() ? "true" : "false") + ",";
  json += "\"mountFault\":" + String(mountCommFault ? "true" : "false") + ",";
  json += "\"lastMountFault\":\"" + jsonEscape(lastMountFault) + "\",";
  json += "\"lastResponseAgeMs\":" + String(mountLastResponseAge()) + ",";
  json += "\"mountUptime\":\"" + jsonEscape(formatMountUptime()) + "\",";
  json += "\"webSelectedRate\":" + String(webSelectedRateIndex + 1) + ",";
  json += "\"nudgeRate1\":" + String(nudgeRateDeg[0], 3) + ",";
  json += "\"nudgeRate2\":" + String(nudgeRateDeg[1], 3) + ",";
  json += "\"nudgeRate3\":" + String(nudgeRateDeg[2], 3) + ",";
  json += "\"nudgeRate4\":" + String(nudgeRateDeg[3], 3) + ",";
  json += "\"safeDecLimitEnabled\":false,";
  json += "\"safeDecMinDeg\":" + String(safeDecMinDeg, 3) + ",";
  json += "\"safeDecMaxDeg\":" + String(safeDecMaxDeg, 3) + ",";
  json += "\"safeAltLimitEnabled\":" + String(safeAltLimitEnabled ? "true" : "false") + ",";
  json += "\"safeAltMinDeg\":" + String(safeAltMinDeg, 3) + ",";
  json += "\"safeAltMaxDeg\":" + String(safeAltMaxDeg, 3) + ",";
  json += "\"basicStatusText\":\"" + jsonEscape(basicStatusText()) + "\",";
  json += "\"basicSystemText\":\"" + jsonEscape(basicSystemHealthText()) + "\",";
  json += "\"statusText\":\"" + jsonEscape(st) + "\",";
  json += "\"uptime\":\"" + jsonEscape(formatUptime()) + "\",";
  json += "\"cpuLoad\":\"" + jsonEscape(sampleWebCpuLoadText()) + "\",";
  json += "\"loopLatencyCurrentMs\":" + String(lastLoopLatencyMs) + ",";
  json += "\"loopLatencyMaxMs\":" + String(maxLoopLatencyMs) + ",";
  json += "\"pollSchedulerLatencyCurrentMs\":" + String(lastPollSchedulerLatencyMs) + ",";
  json += "\"pollSchedulerLatencyMaxMs\":" + String(maxPollSchedulerLatencyMs) + ",";
  json += "\"pollMissedDeadlineCount\":" + String(mountPollsMissedDeadline) + ",";
  json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"systemText\":\"" + jsonEscape(sys) + "\"";
  json += "}";
  sendNoCacheHeaders();
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", json);
}

void handleSetSiteTimePage() {
  if (server.hasArg("lat")) {
    siteLatitudeDeg = server.arg("lat").toFloat();
    siteValid = true;
  }

  if (server.hasArg("lon")) {
    siteLongitudeDeg = server.arg("lon").toFloat();
    siteValid = true;
  }

  if (server.hasArg("elev")) {
    siteElevationMeters = server.arg("elev").toFloat();
  }

  if (server.hasArg("offset")) {
    utcOffsetMinutes = server.arg("offset").toInt();
    utcOffsetSource = "web/manual";
    timeValid = true;
    timeSetMillis = millis();
  }

  if (server.hasArg("poll")) {
    long p = server.arg("poll").toInt();
    if (p < 0) p = 0;
    if (p > 60000) p = 60000;
    pollIntervalMs = (unsigned long)p;
    nextMountPollDueMs = 0;
    backgroundPollingAutoDisabled = false;
    backgroundPollFailCount = 0;
  }
  if (server.hasArg("idlepoll")) {
    long p = server.arg("idlepoll").toInt();
    if (p < 0) p = 0;
    if (p > 60000) p = 60000;
    idlePollIntervalMs = (unsigned long)p;
    nextMountPollDueMs = 0;
  }

  if (server.hasArg("throttle")) {
    long t = server.arg("throttle").toInt();
    if (t < 250) t = 250;
    if (t > 60000) t = 60000;
    minClientPollIntervalMs = (unsigned long)t;
  }

  computeAltAzFromRaDec();
  savePersistentSettings();
  LOG_TIME_I("Web site/poll update: lat=%.6f lon=%.6f offset=%d poll=%lu throttle=%lu",
       siteLatitudeDeg, siteLongitudeDeg, utcOffsetMinutes, pollIntervalMs, minClientPollIntervalMs);

  if (wantsAjax()) sendAjaxOK("Site/time/polling saved");
  else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}



void handleSetManualSiteTimePage() {
  logHttpRequest("SET_MANUAL_SITE_TIME");
  LOG_SET_I("Setup button pressed: Save Site / Time / Polling");
  if (server.hasArg("lat")) {
    siteLatitudeDeg = server.arg("lat").toFloat();
    siteValid = true;
  }
  if (server.hasArg("lon")) {
    siteLongitudeDeg = server.arg("lon").toFloat();
    siteValid = true;
  }
  if (server.hasArg("offset")) utcOffsetMinutes = server.arg("offset").toInt();
  if (server.hasArg("year")) localYear = server.arg("year").toInt();
  if (server.hasArg("month")) localMonth = server.arg("month").toInt();
  if (server.hasArg("day")) localDay = server.arg("day").toInt();
  if (server.hasArg("hour")) localHour = server.arg("hour").toInt();
  if (server.hasArg("minute")) localMinute = server.arg("minute").toInt();
  if (server.hasArg("second")) localSecond = server.arg("second").toInt();

  timeValid = true;
  markTimeSource(TIME_SRC_WEB);
  timeSetMillis = millis();

  if (server.hasArg("poll")) {
    long p = server.arg("poll").toInt();
    if (p < 0) p = 0;
    if (p > 60000) p = 60000;
    pollIntervalMs = (unsigned long)p;
    nextMountPollDueMs = 0;
    backgroundPollingAutoDisabled = false;
    backgroundPollFailCount = 0;
  }
  if (server.hasArg("idlepoll")) {
    long p = server.arg("idlepoll").toInt();
    if (p < 0) p = 0;
    if (p > 60000) p = 60000;
    idlePollIntervalMs = (unsigned long)p;
    nextMountPollDueMs = 0;
  }
  if (server.hasArg("throttle")) {
    long t = server.arg("throttle").toInt();
    if (t < 250) t = 250;
    if (t > 60000) t = 60000;
    minClientPollIntervalMs = (unsigned long)t;
  }

  computeAltAzFromRaDec();
  bool savedOk = savePersistentSettings();
  LOGI("Manual site/time update: lat=%.6f lon=%.6f offset=%d date=%04d-%02d-%02d time=%02d:%02d:%02d poll=%lu throttle=%lu",
       siteLatitudeDeg, siteLongitudeDeg, utcOffsetMinutes,
       localYear, localMonth, localDay, localHour, localMinute, localSecond,
       pollIntervalMs, minClientPollIntervalMs);

  if (wantsAjax()) {
    if (savedOk) sendAjaxOK("Manual site/time saved");
    else sendAjaxFail("Manual site/time save failed");
  }
  else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

void handleSetDeviceSiteTimePage() {
  bool gotLocation = false;

  if (server.hasArg("lat")) {
    siteLatitudeDeg = server.arg("lat").toFloat();
    gotLocation = true;
  }

  if (server.hasArg("lon")) {
    siteLongitudeDeg = server.arg("lon").toFloat();
    gotLocation = true;
  }

  if (gotLocation) {
    siteValid = true;
    markLocationSource("Browser HTTPS");
  }

  if (server.hasArg("elev")) siteElevationMeters = server.arg("elev").toFloat();

  if (server.hasArg("offset")) {
    utcOffsetMinutes = server.arg("offset").toInt();
    utcOffsetSource = "device/browser";
  }
  if (server.hasArg("year")) localYear = server.arg("year").toInt();
  if (server.hasArg("month")) localMonth = server.arg("month").toInt();
  if (server.hasArg("day")) localDay = server.arg("day").toInt();
  if (server.hasArg("hour")) localHour = server.arg("hour").toInt();
  if (server.hasArg("minute")) localMinute = server.arg("minute").toInt();
  if (server.hasArg("second")) localSecond = server.arg("second").toInt();

  timeValid = true;
  timeSetMillis = millis();

  if (server.hasArg("poll")) {
    long p = server.arg("poll").toInt();
    if (p < 0) p = 0;
    if (p > 60000) p = 60000;
    pollIntervalMs = (unsigned long)p;
    nextMountPollDueMs = 0;
    backgroundPollingAutoDisabled = false;
    backgroundPollFailCount = 0;
  }
  if (server.hasArg("idlepoll")) {
    long p = server.arg("idlepoll").toInt();
    if (p < 0) p = 0;
    if (p > 60000) p = 60000;
    idlePollIntervalMs = (unsigned long)p;
    nextMountPollDueMs = 0;
  }

  if (server.hasArg("throttle")) {
    long t = server.arg("throttle").toInt();
    if (t < 250) t = 250;
    if (t > 60000) t = 60000;
    minClientPollIntervalMs = (unsigned long)t;
  }

  computeAltAzFromRaDec();
  savePersistentSettings();

  LOGI("Device site/time update: location=%s lat=%.6f lon=%.6f offset=%d date=%04d-%02d-%02d time=%02d:%02d:%02d poll=%lu throttle=%lu",
       gotLocation ? "yes" : "no",
       siteLatitudeDeg,
       siteLongitudeDeg,
       utcOffsetMinutes,
       localYear,
       localMonth,
       localDay,
       localHour,
       localMinute,
       localSecond,
       pollIntervalMs,
       minClientPollIntervalMs);

  if (gotLocation) sendAjaxOK("Device time and location saved");
  else sendAjaxOK("Device time saved; browser did not provide location");
}

uint16_t logCategoryBitFromName(const String &name) {
  if (name == "mount") return LOG_CAT_MOUNT;
  if (name == "skysafari" || name == "sky" || name == "safari" || name == "lx200") return LOG_CAT_SKYSAFARI;
  if (name == "bluetooth" || name == "bt") return LOG_CAT_BLUETOOTH;
  if (name == "alpaca") return LOG_CAT_ALPACA;
  if (name == "stellarium" || name == "stel") return LOG_CAT_STELLARIUM;
  if (name == "web" || name == "http") return LOG_CAT_WEB;
  if (name == "wifi" || name == "wi-fi" || name == "network") return LOG_CAT_WIFI;
  if (name == "timeloc" || name == "time_location" || name == "time" || name == "location" || name == "ntp") return LOG_CAT_TIMELOC;
  if (name == "settings" || name == "setting" || name == "config" || name == "setup" || name == "fs" || name == "littlefs") return LOG_CAT_SETTINGS;
  if (name == "system" || name == "sys" || name == "boot") return LOG_CAT_SYSTEM;
  return 0;
}


void handleSetupActionPage() {
  logHttpRequest("SETUP_ACTION");
  String name = server.hasArg("name") ? urlDecodeSimple(server.arg("name")) : "unknown";
  name.trim();
  LOG_SET_I("Setup button pressed: %s", name.c_str());

  if (wantsAjax()) sendAjaxOK("Setup action logged");
  else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

void handleSetLogPage() {
  logHttpRequest("SET_LOG");
  if (server.hasArg("level")) {
    String lv = server.arg("level");
    lv.toLowerCase();
    int lvl = LOG_INFO;

    if (lv == "none" || lv == "0") lvl = LOG_NONE;
    else if (lv == "error" || lv == "err" || lv == "1") lvl = LOG_ERROR;
    else if (lv == "warn" || lv == "warning" || lv == "2") lvl = LOG_WARN;
    else if (lv == "info" || lv == "3") lvl = LOG_INFO;
    else if (lv == "debug" || lv == "4") lvl = LOG_DEBUG;
    else if (lv == "trace" || lv == "5") lvl = LOG_TRACE;
    else lvl = lv.toInt();

    if (lvl < 0) lvl = 0;
    if (lvl > 5) lvl = 5;
    LOG_LEVEL = lvl;
  }

  if (server.hasArg("cats")) {
    String cats = server.arg("cats");
    cats.toLowerCase();

    if (cats == "all") {
      LOG_SUBSYSTEM_MASK = LOG_CAT_ALL;
    } else {
      uint16_t mask = 0;
      int start = 0;
      while (start < (int)cats.length()) {
        int comma = cats.indexOf(',', start);
        String part = comma >= 0 ? cats.substring(start, comma) : cats.substring(start);
        part.trim();
        mask |= logCategoryBitFromName(part);
        if (comma < 0) break;
        start = comma + 1;
      }
      LOG_SUBSYSTEM_MASK = mask;
    }
  }

  if (LOG_SUBSYSTEM_MASK & LOG_CAT_SETTINGS) {
    LOG_SET_I("Log settings changed: level=%s subsystemMask=0x%04X", logLevelName(LOG_LEVEL), LOG_SUBSYSTEM_MASK);
  } else {
    String line = "[info][settings] Log settings changed: level=";
    line += logLevelName(LOG_LEVEL);
    line += " subsystemMask=0x";
    line += String(LOG_SUBSYSTEM_MASK, HEX);
    addLogLine(line);
  }

  if (wantsAjax()) sendAjaxOK("Log settings changed");
  else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

bool decWithinSafeLimits(double decDeg) {
  if (!safeDecLimitEnabled) return true;
  return decDeg >= safeDecMinDeg && decDeg <= safeDecMaxDeg;
}

bool altWithinSafeLimits(double altDeg) {
  if (altDeg < 0.0) return false;
  if (!safeAltLimitEnabled) return true;
  return altDeg >= safeAltMinDeg && altDeg <= safeAltMaxDeg;
}

bool targetAltitudeFromRaDec(double raDeg, double decDeg, double &altDeg) {
  if (!siteValid || !timeValid) return false;

  double lstDeg = normalizeRA(gmstDegrees() + siteLongitudeDeg);
  double haDeg = normalizeRA(lstDeg - normalizeRA(raDeg));
  if (haDeg > 180.0) haDeg -= 360.0;

  double ha = degToRad(haDeg);
  double dec = degToRad(decDeg);
  double lat = degToRad(siteLatitudeDeg);

  double sinAlt = sin(dec) * sin(lat) + cos(dec) * cos(lat) * cos(ha);
  sinAlt = constrain(sinAlt, -1.0, 1.0);
  altDeg = radToDeg(asin(sinAlt));
  return true;
}

bool targetRaDecFromAltAz(double altDeg, double azDeg, double &raDeg, double &decDeg) {
  if (!siteValid || !timeValid) return false;

  double lstDeg = normalizeRA(gmstDegrees() + siteLongitudeDeg);
  double alt = degToRad(altDeg);
  double az = degToRad(normalizeAz(azDeg));
  double lat = degToRad(siteLatitudeDeg);

  double sinDec = sin(alt) * sin(lat) + cos(alt) * cos(lat) * cos(az);
  sinDec = constrain(sinDec, -1.0, 1.0);
  double dec = asin(sinDec);

  double y = -sin(az) * cos(alt);
  double x = sin(alt) * cos(lat) - cos(alt) * sin(lat) * cos(az);
  double haDeg = normalizeRA(radToDeg(atan2(y, x)));
  if (haDeg > 180.0) haDeg -= 360.0;

  raDeg = normalizeRA(lstDeg - haDeg);
  decDeg = radToDeg(dec);
  return true;
}

bool allowSlewRaDecBySafety(double raDeg, double decDeg, String &reason) {
  double targetAlt = 0.0;
  if (!targetAltitudeFromRaDec(raDeg, decDeg, targetAlt)) {
    reason = "Blocked: valid site/time required for horizon/altitude safety check";
    return false;
  }

  if (!altWithinSafeLimits(targetAlt)) {
    if (targetAlt < 0.0) reason = "Blocked by horizon safety: target Alt ";
    else reason = "Blocked by Alt safety limit: target Alt ";
    reason += String(targetAlt, 2);
    reason += " deg";
    if (safeAltLimitEnabled && targetAlt >= 0.0) {
      reason += " allowed ";
      reason += String(safeAltMinDeg, 2);
      reason += " to ";
      reason += String(safeAltMaxDeg, 2);
    }
    return false;
  }

  LOG_MOUNT_D("RA/Dec altitude safety OK: targetRA=%.3f targetDec=%.3f computedAlt=%.3f",
              normalizeRA(raDeg), decDeg, targetAlt);
  return true;
}

bool allowSlewAltAzBySafety(double altDeg, double azDeg, String &reason, double *outRaDeg = nullptr, double *outDecDeg = nullptr) {
  if (!altWithinSafeLimits(altDeg)) {
    if (altDeg < 0.0) reason = "Blocked by horizon safety: target Alt ";
    else reason = "Blocked by Alt safety limit: target Alt ";
    reason += String(altDeg, 2);
    reason += " deg";
    if (safeAltLimitEnabled && altDeg >= 0.0) {
      reason += " allowed ";
      reason += String(safeAltMinDeg, 2);
      reason += " to ";
      reason += String(safeAltMaxDeg, 2);
    }
    return false;
  }

  if (outRaDeg || outDecDeg) {
    double r = 0.0, d = 0.0;
    if (targetRaDecFromAltAz(altDeg, azDeg, r, d)) {
      if (outRaDeg) *outRaDeg = r;
      if (outDecDeg) *outDecDeg = d;
    }
  }

  LOG_MOUNT_D("Alt/Az altitude safety OK: targetAlt=%.3f targetAz=%.3f",
              altDeg, normalizeAz(azDeg));
  return true;
}

double sanitizeRateValue(double v, double fallback) {
  if (isnan(v) || v <= 0.0) return fallback;
  if (v > 5.0) return 5.0;
  return v;
}

void handleSetAltLimitsPage() {
  logHttpRequest("SET_ALT_LIMITS");
  LOG_SET_I("Setup button pressed: Save Alt Safety Limits");

  bool enabled = server.hasArg("enabled") && server.arg("enabled") == "1";
  double mn = server.hasArg("min") ? server.arg("min").toFloat() : safeAltMinDeg;
  double mx = server.hasArg("max") ? server.arg("max").toFloat() : safeAltMaxDeg;

  if (isnan(mn) || isnan(mx) || mn < 0.0 || mx > 90.0 || mn >= mx) {
    if (wantsAjax()) sendAjaxFail("Invalid Alt limits. Use 0..90 and min < max.");
    else server.send(400, "text/plain", "Invalid Alt limits");
    return;
  }

  safeAltLimitEnabled = enabled;
  safeAltMinDeg = mn;
  safeAltMaxDeg = mx;
  bool ok = savePersistentSettings();

  LOG_SET_I("Alt safety limits %s: min=%.2f max=%.2f save=%s",
            safeAltLimitEnabled ? "enabled" : "disabled",
            safeAltMinDeg, safeAltMaxDeg, ok ? "ok" : "failed");

  if (wantsAjax()) {
    if (ok) sendAjaxOK("Alt safety limits saved");
    else sendAjaxFail("Alt safety limits changed but save failed");
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

void handleSetDecLimitsPage() {
  logHttpRequest("SET_DEC_LIMITS_DEPRECATED");
  safeDecLimitEnabled = false;
  savePersistentSettings();
  LOG_SET_W("Deprecated Dec safety limit request ignored; use Altitude Safety Limits instead");
  if (wantsAjax()) sendAjaxOK("Dec limits are deprecated; use Altitude Safety Limits");
  else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

void handleSetRatesPage() {
  logHttpRequest("SET_RATES");
  LOG_SET_I("Setup button pressed: Save Rates");
  if (server.hasArg("r1")) nudgeRateDeg[0] = sanitizeRateValue(server.arg("r1").toFloat(), nudgeRateDeg[0]);
  if (server.hasArg("r2")) nudgeRateDeg[1] = sanitizeRateValue(server.arg("r2").toFloat(), nudgeRateDeg[1]);
  if (server.hasArg("r3")) nudgeRateDeg[2] = sanitizeRateValue(server.arg("r3").toFloat(), nudgeRateDeg[2]);
  if (server.hasArg("r4")) nudgeRateDeg[3] = sanitizeRateValue(server.arg("r4").toFloat(), nudgeRateDeg[3]);

  lx200CurrentNudgeStepDeg = nudgeRateDeg[1];

  savePersistentSettings();

  LOG_SET_D("Nudge rates saved: RS=%.3f RM=%.3f RC=%.3f RG=%.3f",
       nudgeRateDeg[0], nudgeRateDeg[1], nudgeRateDeg[2], nudgeRateDeg[3]);

  if (wantsAjax()) sendAjaxOK("Nudge rates saved");
  else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

void handleSetWebRatePage() {
  logHttpRequest("SET_WEB_RATE");
  if (server.hasArg("rate")) {
    int r = server.arg("rate").toInt();
    if (r < 1) r = 1;
    if (r > 4) r = 4;
    webSelectedRateIndex = r - 1;
    lx200CurrentNudgeStepDeg = nudgeRateDeg[webSelectedRateIndex];
    savePersistentSettings();
    LOGI("Web selected nudge rate changed to %d = %.3f deg",
         r, nudgeRateDeg[webSelectedRateIndex]);
  }

  if (wantsAjax()) sendAjaxOK("Selected rate set");
  else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

void handleWebNudgePage() {
  logHttpRequest("WEB_NUDGE");
  String dir = server.hasArg("dir") ? server.arg("dir") : "";
  double step = nudgeRateDeg[webSelectedRateIndex];

  double altDelta = 0.0;
  double azDelta = 0.0;

  if (dir == "n") altDelta = step;
  else if (dir == "s") altDelta = -step;
  else if (dir == "e") azDelta = step;
  else if (dir == "w") azDelta = -step;
  else {
    if (wantsAjax()) sendAjaxFail("Bad direction");
    else server.send(400, "text/plain", "Bad direction");
    return;
  }

  if (asyncSlewPending || asyncAltAzSlewPending || asyncNudgePending || asyncRaDecReadPending || asyncAltAzReadPending || asyncSlewRunning) {
    if (wantsAjax()) sendAjaxFail("Another mount command is already queued; nudge not queued");
    else {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "Mount busy");
    }
    return;
  }

  queueAsyncNudge(altDelta, azDelta);

  // Optimistically update the displayed cache so the UI reflects the requested motion.
  if (altAzCacheValid) {
    cachedAlt_deg = clampAlt(cachedAlt_deg + altDelta);
    cachedAz_deg = normalizeAz(cachedAz_deg + azDelta);
    computeRaDecFromAltAz();
  }

  if (wantsAjax()) {
    sendAjaxOK("Nudge queued");
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Nudge queued");
  }
}


void handleGetRaDecWebPage() {
  logHttpRequest("GET_RADEC_WEB");
  if (asyncSlewPending || asyncAltAzSlewPending || asyncNudgePending || asyncRaDecReadPending || asyncAltAzReadPending || asyncSlewRunning) {
    if (wantsAjax()) sendAjaxFail("Another mount command is already queued; RA/Dec read not queued");
    else {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "Mount busy");
    }
    return;
  }

  asyncRaDecReadPending = true;

  if (wantsAjax()) {
    sendAjaxOK("RA/Dec read queued");
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "RA/Dec read queued");
  }
}

void handleGetAltAzWebPage() {
  logHttpRequest("GET_ALTAZ_WEB");
  if (asyncSlewPending || asyncAltAzSlewPending || asyncNudgePending || asyncRaDecReadPending || asyncAltAzReadPending || asyncSlewRunning) {
    if (wantsAjax()) sendAjaxFail("Another mount command is already queued; Alt/Az read not queued");
    else {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "Mount busy");
    }
    return;
  }

  asyncAltAzReadPending = true;

  if (wantsAjax()) {
    sendAjaxOK("Alt/Az read queued");
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Alt/Az read queued");
  }
}

void handleWebGotoRaDecPage() {
  logHttpRequest("WEB_GOTO_RADEC");
  double raHours = server.hasArg("ra_hours") ? server.arg("ra_hours").toFloat() : 0.0;
  double decDeg = server.hasArg("dec_deg") ? server.arg("dec_deg").toFloat() : 0.0;
  double raDeg = normalizeRA(raHours * 15.0);
  LOG_MOUNT_D("Web RA/Dec GOTO request safety: dec=%.3f limitEnabled=%d min=%.3f max=%.3f", decDeg, safeDecLimitEnabled ? 1 : 0, safeDecMinDeg, safeDecMaxDeg);

  String safetyReason;
  if (!allowSlewRaDecBySafety(raDeg, decDeg, safetyReason)) {
    LOG_MOUNT_W("Web RA/Dec GOTO blocked: %s", safetyReason.c_str());
    if (wantsAjax()) sendAjaxFail(safetyReason);
    else server.send(400, "text/plain", safetyReason);
    return;
  }

  if (asyncSlewPending || asyncAltAzSlewPending || asyncNudgePending || asyncRaDecReadPending || asyncAltAzReadPending || asyncSlewRunning) {
    if (wantsAjax()) sendAjaxFail("Another mount command is already queued; GOTO not queued");
    else {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "Mount busy");
    }
    return;
  }

  // Queue the RA/Dec GOTO and return immediately. Do NOT overwrite cached/current
  // position with the target here; the top banner must show actual/current
  // mount position, not the requested target.
  markPositionDemand("web RA/Dec GOTO");
  queueAsyncSlew(raDeg, decDeg);

  if (wantsAjax()) {
    sendAjaxOK("GOTO RA/Dec queued");
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "GOTO RA/Dec queued");
  }
}

void handleWebGotoAltAzPage() {
  logHttpRequest("WEB_GOTO_ALTAZ");
  double altDeg = server.hasArg("alt_deg") ? server.arg("alt_deg").toFloat() : 0.0;
  double azDeg = server.hasArg("az_deg") ? server.arg("az_deg").toFloat() : 0.0;

  altDeg = clampAlt(altDeg);
  azDeg = normalizeAz(azDeg);

  String safetyReason;
  if (!allowSlewAltAzBySafety(altDeg, azDeg, safetyReason)) {
    LOG_MOUNT_W("Web Alt/Az GOTO blocked: %s", safetyReason.c_str());
    if (wantsAjax()) sendAjaxFail(safetyReason);
    else server.send(400, "text/plain", safetyReason);
    return;
  }

  if (asyncSlewPending || asyncAltAzSlewPending || asyncNudgePending || asyncRaDecReadPending || asyncAltAzReadPending || asyncSlewRunning) {
    if (wantsAjax()) sendAjaxFail("Another mount command is already queued; Alt/Az GOTO not queued");
    else {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "Mount busy");
    }
    return;
  }

  // Queue the Alt/Az GOTO and return immediately. Do NOT overwrite cached/current
  // Alt/Az with the target here; the top banner must remain actual/current
  // position until the mount reports or the slew completes.
  markPositionDemand("web Alt/Az GOTO");
  queueAsyncAltAzSlew(altDeg, azDeg);

  if (wantsAjax()) {
    sendAjaxOK("GOTO Alt/Az queued");
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "GOTO Alt/Az queued");
  }
}

void handleResetRatesPage() {
  logHttpRequest("RESET_RATES");
  LOG_SET_I("Setup button pressed: Reset Rate Defaults");
  nudgeRateDeg[0] = 0.10;
  nudgeRateDeg[1] = 0.25;
  nudgeRateDeg[2] = 0.50;
  nudgeRateDeg[3] = 1.00;
  lx200CurrentNudgeStepDeg = nudgeRateDeg[1];
  webSelectedRateIndex = 1;
  savePersistentSettings();

  if (wantsAjax()) sendAjaxOK("Rates reset");
  else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}










void handleClearSettingsPage() {
  logHttpRequest("CLEAR_SETTINGS");
  LOG_SET_I("Setup button pressed: Clear Saved Last Location");
  LOG_SET_D("Clearing saved last location/site/time settings");
  siteLatitudeDeg = 0.0;
  siteLongitudeDeg = 0.0;
  siteElevationMeters = 0.0;
  utcOffsetMinutes = 0;
  siteValid = false;
  timeValid = false;
  savePersistentSettings();

  if (wantsAjax()) sendAjaxOK("Saved last location cleared");
  else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}



void handleSetWiFiPage() {
  logHttpRequest("SET_WIFI");
  LOG_SET_I("Setup button pressed: Save / Connect STA WiFi");
  String ssid = server.hasArg("ssid") ? urlDecodeSimple(server.arg("ssid")) : "";
  String pass = server.hasArg("pass") ? urlDecodeSimple(server.arg("pass")) : "";

  ssid.trim();

  if (ssid.length() == 0) {
    if (wantsAjax()) sendAjaxFail("SSID is required");
    else server.send(400, "text/plain", "SSID is required");
    return;
  }

  LOG_WIFI_I("Web requested STA WiFi save/connect: ssid=%s", ssid.c_str());
  staRuntimeDisabled = false;
  if (!saveWiFiConfig(ssid, pass)) {
    if (wantsAjax()) sendAjaxFail("WiFi config save failed");
    else server.send(500, "text/plain", "WiFi config save failed");
    return;
  }

  if (bridgeMode == BRIDGE_MODE_BT_MIN_WEB) {
    staRuntimeDisabled = false;
    String apUrl = "http://" + apIp + "/";
    WiFi.mode(WIFI_STA);
    startFallbackAP();
    bool ok = connectConfiguredSTA();
    if (ok) {
      wifiModeText = "STA-only/AP-only + Bluetooth";
      saveLastStaIp(WiFi.localIP().toString());
    } else {
      wifiModeText = "AP only + Bluetooth";
    }
    String url = ok ? ("http://" + WiFi.localIP().toString() + "/") : apUrl;
    String msg = ok
      ? ("STA WiFi saved and connected at " + WiFi.localIP().toString() + ". Bluetooth remains enabled. Redirecting...")
      : ("STA WiFi saved, but connection failed: " + lastWifiStatus + ". Bluetooth remains enabled. AP fallback remains " + apUrl);

    if (wantsAjax()) {
      if (ok) sendAjaxRedirectOK(msg, url, 1500);
      else sendAjaxOK(msg);
    } else {
      String page;
      page.reserve(1900);
      page += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
      page += F("<title>NexStar WiFi Switch</title>");
      page += F("<style>body{font-family:Arial,sans-serif;background:#111;color:#eee;margin:20px}.box{border:1px solid #444;background:#1b1b1b;border-radius:8px;padding:14px;max-width:560px}a{color:#8cf}</style>");
      if (ok) {
        page += F("<script>setTimeout(function(){location.href='");
        page += url;
        page += F("'},1500);</script>");
      }
      page += F("</head><body><div class='box'><h2>Switching To STA WiFi</h2><p>");
      page += htmlEscape(msg);
      page += F("</p><p>STA target: <a href='");
      page += url;
      page += F("'>");
      page += url;
      page += F("</a></p><p>AP fallback: <a href='");
      page += apUrl;
      page += F("'>");
      page += apUrl;
      page += F("</a></p></div></body></html>");
      sendNoCacheHeaders();
      server.sendHeader("Connection", "close");
      server.send(200, "text/html", page);
    }
    return;
  }

  WiFi.mode(WIFI_STA);
  startFallbackAP();
  bool ok = connectConfiguredSTA();
  LOG_WIFI_I("STA WiFi connect after save result: %s ip=%s", ok ? "connected" : "failed", WiFi.localIP().toString().c_str());

  if (wantsAjax()) {
    if (ok) {
      String url = "http://" + WiFi.localIP().toString() + "/";
      sendAjaxRedirectOK("WiFi STA saved and connected: " + WiFi.localIP().toString() + ". Redirecting...", url, 1200);
    }
    else sendAjaxOK("WiFi STA saved, but connection failed: " + lastWifiStatus + ". AP fallback remains active.");
  } else {
    server.sendHeader("Location", ok ? ("http://" + WiFi.localIP().toString() + "/") : "/");
    server.send(302, "text/plain", "Redirecting");
  }
}


bool extractJsonNumber(const String &body, const String &key, double &out) {
  int k = body.indexOf("\"" + key + "\"");
  if (k < 0) return false;
  int colon = body.indexOf(':', k);
  if (colon < 0) return false;
  int start = colon + 1;
  while (start < (int)body.length() && (body[start] == ' ' || body[start] == '\t' || body[start] == '\r' || body[start] == '\n')) start++;
  int end = start;
  while (end < (int)body.length()) {
    char c = body[end];
    if (!((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.')) break;
    end++;
  }
  if (end <= start) return false;
  out = body.substring(start, end).toDouble();
  return true;
}

String extractJsonString(const String &body, const String &key) {
  int k = body.indexOf("\"" + key + "\"");
  if (k < 0) return "";
  int colon = body.indexOf(':', k);
  if (colon < 0) return "";
  int q1 = body.indexOf('"', colon + 1);
  if (q1 < 0) return "";
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return body.substring(q1 + 1, q2);
}

bool fetchApproxLocationFromInternet() {
  approxIpLocationValid = false;

  if (!staConnected || WiFi.status() != WL_CONNECTED) {
    approxIpLocationStatus = "STA WiFi is not connected";
    LOG_TIME_W("IP geolocation failed: STA WiFi is not connected");
    return false;
  }

#if defined(ESP8266)
  WiFiClient client;
  HTTPClient http;
  bool beginOk = http.begin(client, "http://ip-api.com/json/?fields=status,message,lat,lon,city,regionName,country,query");
#else
  HTTPClient http;
  bool beginOk = http.begin("http://ip-api.com/json/?fields=status,message,lat,lon,city,regionName,country,query");
#endif

  if (!beginOk) {
    approxIpLocationStatus = "HTTP begin failed";
    LOG_TIME_W("IP geolocation failed: HTTP begin failed");
    return false;
  }

  http.setTimeout(7000);
  int code = http.GET();
  if (code != 200) {
    approxIpLocationStatus = "HTTP error " + String(code);
    LOG_TIME_W("IP geolocation failed: HTTP code %d", code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  if (body.indexOf("\"status\":\"success\"") < 0) {
    String msg = extractJsonString(body, "message");
    approxIpLocationStatus = "Service failed" + (msg.length() ? ": " + msg : "");
    LOG_TIME_W("IP geolocation service failed: %s", approxIpLocationStatus.c_str());
    return false;
  }

  double lat = 0.0;
  double lon = 0.0;
  if (!extractJsonNumber(body, "lat", lat) || !extractJsonNumber(body, "lon", lon)) {
    approxIpLocationStatus = "No lat/lon in response";
    LOG_TIME_W("IP geolocation failed: no lat/lon in response");
    return false;
  }

  approxIpLatitudeDeg = lat;
  approxIpLongitudeDeg = lon;
  String city = extractJsonString(body, "city");
  String region = extractJsonString(body, "regionName");
  String country = extractJsonString(body, "country");
  String ip = extractJsonString(body, "query");

  approxIpLocationText = city;
  if (region.length()) approxIpLocationText += (approxIpLocationText.length() ? ", " : "") + region;
  if (country.length()) approxIpLocationText += (approxIpLocationText.length() ? ", " : "") + country;
  if (ip.length()) approxIpLocationText += " IP " + ip;

  approxIpLocationStatus = "Approx IP location fetched";
  approxIpLocationValid = true;

  LOG_TIME_I("IP geolocation approximate location: lat=%.6f lon=%.6f %s",
       approxIpLatitudeDeg, approxIpLongitudeDeg, approxIpLocationText.c_str());
  return true;
}


void handleFetchIpLocationPage() {
  logHttpRequest("FETCH_IP_LOCATION");
  LOG_SET_I("Setup button pressed: Get Approx Internet Location");
  LOG_TIME_I("Web requested approximate internet location fetch");
  bool ok = fetchApproxLocationFromInternet();

  String msg;
  if (ok) {
    msg = "Approx location fetched: " + String(approxIpLatitudeDeg, 6) + ", " + String(approxIpLongitudeDeg, 6);
    if (approxIpLocationText.length()) msg += " (" + approxIpLocationText + ")";
  } else {
    msg = approxIpLocationStatus;
  }

  if (wantsAjax()) {
    if (ok) sendAjaxOK(msg);
    else sendAjaxFail(msg);
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

void handleUseIpLocationPage() {
  logHttpRequest("USE_IP_LOCATION");
  LOG_SET_I("Setup button pressed: Use Approx Internet Location");
  LOG_TIME_I("Web requested use of approximate IP location");
  if (!approxIpLocationValid) {
    if (wantsAjax()) sendAjaxFail("No approximate IP location has been fetched yet");
    else server.send(400, "text/plain", "No approximate IP location has been fetched yet");
    return;
  }

  siteLatitudeDeg = approxIpLatitudeDeg;
  siteLongitudeDeg = approxIpLongitudeDeg;
  siteValid = true;
  markLocationSource("IP Approx");
  computeAltAzFromRaDec();
  bool savedOk = savePersistentSettings();
  LOG_TIME_I("Approx IP location applied from web: lat=%.6f lon=%.6f save=%s", siteLatitudeDeg, siteLongitudeDeg, savedOk ? "ok" : "failed");

  String msg = "Approx IP location applied: " + String(siteLatitudeDeg, 6) + ", " + String(siteLongitudeDeg, 6);
  if (wantsAjax()) {
    if (savedOk) sendAjaxOK(msg);
    else sendAjaxFail("Approx IP location applied but save failed");
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

void handleSetNtpPage() {
  logHttpRequest("SET_NTP");
  LOG_SET_I("Setup button pressed: Save NTP Settings");
  bool newEnabled = server.hasArg("enabled") && server.arg("enabled") == "1";
  String s1 = server.hasArg("server1") ? urlDecodeSimple(server.arg("server1")) : String(ntpServer1);
  String s2 = server.hasArg("server2") ? urlDecodeSimple(server.arg("server2")) : String(ntpServer2);
  String tz = server.hasArg("tz") ? urlDecodeSimple(server.arg("tz")) : String(tzRule);

  s1.trim(); s2.trim(); tz.trim();
  if (s1.length() == 0) s1 = "pool.ntp.org";
  if (s2.length() == 0) s2 = "time.nist.gov";
  if (tz.length() == 0) tz = "MST7MDT,M3.2.0/2,M11.1.0/2";

  ntpEnabled = newEnabled;
  strlcpy(ntpServer1, s1.c_str(), sizeof(ntpServer1));
  strlcpy(ntpServer2, s2.c_str(), sizeof(ntpServer2));
  strlcpy(tzRule, tz.c_str(), sizeof(tzRule));

  LOG_TIME_I("Web NTP settings save requested: enabled=%d server1=%s server2=%s tz=%s", ntpEnabled ? 1 : 0, ntpServer1, ntpServer2, tzRule);
  bool saveOk = savePersistentSettings();
  LOG_SET_I("NTP settings persistent save: %s", saveOk ? "ok" : "failed");
  LOG_TIME_D("NTP settings changed in RAM: enabled=%d server1=%s server2=%s tz=%s save=%s", ntpEnabled ? 1 : 0, ntpServer1, ntpServer2, tzRule, saveOk ? "ok" : "failed");
  bool syncOk = false;
  if (ntpEnabled && staConnected && WiFi.status() == WL_CONNECTED) syncOk = syncTimeFromNTP(true);

  String msg = "NTP settings saved";
  if (ntpEnabled) {
    msg += syncOk ? " and synced" : " (waiting for STA/internet sync)";
  } else {
    msg += " (disabled)";
  }

  if (wantsAjax()) {
    if (saveOk) sendAjaxOK(msg);
    else sendAjaxFail("NTP settings save failed");
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

void handleSyncNtpPage() {
  logHttpRequest("SYNC_NTP");
  LOG_SET_I("Setup button pressed: Sync NTP Now");
  LOG_TIME_I("Web NTP Sync Now button pressed");
  LOG_TIME_T("NTP button context: ntpEnabled=%d staConnected=%d wifiStatus=%d staIp=%s",
             ntpEnabled ? 1 : 0,
             (staConnected && WiFi.status() == WL_CONNECTED) ? 1 : 0,
             (int)WiFi.status(),
             WiFi.localIP().toString().c_str());

  if (!ntpEnabled) {
    LOG_TIME_W("NTP Sync Now failed: NTP is disabled");
    if (wantsAjax()) sendAjaxFail("NTP is disabled");
    else server.send(400, "text/plain", "NTP is disabled");
    return;
  }

  if (!staConnected || WiFi.status() != WL_CONNECTED) {
    LOG_TIME_W("NTP Sync Now failed: STA WiFi is not connected");
    if (wantsAjax()) sendAjaxFail("STA WiFi is not connected");
    else server.send(400, "text/plain", "STA WiFi is not connected");
    return;
  }

  bool ok = syncTimeFromNTP(true);
  LOG_TIME_I("NTP Sync Now result: %s", ok ? "success" : "failed");

  if (wantsAjax()) {
    if (ok) sendAjaxOK("NTP sync successful");
    else sendAjaxFail("NTP sync failed");
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}


void handleSetApPage() {
  logHttpRequest("SET_AP");
  LOG_SET_I("Setup button pressed: Save AP / Server Config");
  String ssid = server.hasArg("ap_ssid") ? urlDecodeSimple(server.arg("ap_ssid")) : apSsid;
  String pass = server.hasArg("ap_pass") ? urlDecodeSimple(server.arg("ap_pass")) : apPass;
  String newApIp = server.hasArg("ap_ip") ? urlDecodeSimple(server.arg("ap_ip")) : apIp;
  uint16_t newAlpacaPort = 11111;
  uint16_t newLx200Port = server.hasArg("lx200_port") ? server.arg("lx200_port").toInt() : LX200_PORT;
  uint16_t newStellariumPort = server.hasArg("stellarium_port") ? server.arg("stellarium_port").toInt() : STELLARIUM_PORT;
  uint16_t newTelnetPort = server.hasArg("telnet_port") ? server.arg("telnet_port").toInt() : TELNET_PORT;
  String newTelnetPass = server.hasArg("telnet_pass") ? urlDecodeSimple(server.arg("telnet_pass")) : telnetPassword;

  ssid.trim();
  pass.trim();
  newApIp.trim();
  newTelnetPass.trim();

  IPAddress testIp;
  if (ssid.length() < 1 || ssid.length() > 32) {
    if (wantsAjax()) sendAjaxFail("AP SSID must be 1..32 chars");
    else server.send(400, "text/plain", "AP SSID must be 1..32 chars");
    return;
  }
  if (pass.length() > 0 && pass.length() < 8) {
    if (wantsAjax()) sendAjaxFail("AP password must be blank/open or at least 8 chars");
    else server.send(400, "text/plain", "AP password must be blank/open or at least 8 chars");
    return;
  }
  if (!parseIpAddress(newApIp, testIp)) {
    if (wantsAjax()) sendAjaxFail("AP fallback IP is invalid");
    else server.send(400, "text/plain", "AP fallback IP is invalid");
    return;
  }
  if ((newAlpacaPort != 80 && newAlpacaPort < 1024) || newLx200Port < 1024 || newStellariumPort < 1024 || newTelnetPort < 1) {
    if (wantsAjax()) sendAjaxFail("HTTP port must be 80 or 1024..65535; LX200/Stellarium must be 1024..65535; Telnet must be 1..65535");
    else server.send(400, "text/plain", "HTTP port must be 80 or 1024..65535; LX200/Stellarium must be 1024..65535; Telnet must be 1..65535");
    return;
  }
  if (HTTP_WEB_PORT == newAlpacaPort || HTTP_WEB_PORT == newLx200Port || HTTP_WEB_PORT == newStellariumPort || HTTP_WEB_PORT == newTelnetPort ||
      newAlpacaPort == newLx200Port || newAlpacaPort == newStellariumPort || newAlpacaPort == newTelnetPort ||
      newLx200Port == newStellariumPort || newLx200Port == newTelnetPort || newStellariumPort == newTelnetPort) {
    if (wantsAjax()) sendAjaxFail("Web, Alpaca, LX200, Stellarium, and Telnet ports must be different");
    else server.send(400, "text/plain", "Web, Alpaca, LX200, Stellarium, and Telnet ports must be different");
    return;
  }

  bool portsChanged = (newAlpacaPort != ALPACA_PORT || newLx200Port != LX200_PORT || newStellariumPort != STELLARIUM_PORT || newTelnetPort != TELNET_PORT);

  apSsid = ssid;
  apPass = pass;
  apIp = newApIp;
  ALPACA_PORT = newAlpacaPort;
  LX200_PORT = newLx200Port;
  STELLARIUM_PORT = newStellariumPort;
  TELNET_PORT = newTelnetPort;
  telnetPassword = newTelnetPass;
  if (bridgeMode == BRIDGE_MODE_BT_MIN_WEB) staRuntimeDisabled = true;

  bool saveOk = savePersistentSettings();
  LOG_SET_I("AP/server settings changed: apSsid=%s apIp=%s lx200Port=%u stellariumPort=%u portsChanged=%d staPreserved=%d save=%s", apSsid.c_str(), apIp.c_str(), LX200_PORT, STELLARIUM_PORT, portsChanged ? 1 : 0, staConfigured ? 1 : 0, saveOk ? "ok" : "failed");

  // Do not touch the WiFi/BT radio from this request in Bluetooth mode. On this
  // ESP32, STA+AP -> AP-only transitions while Classic BT is active can hard
  // wedge the radio/serial stack, even when delayed until after the response.
  // Save the AP-only preference and let the next external reset/power cycle
  // apply it through the direct WIFI_AP boot path.
  if (saveOk) {
    if (bridgeMode == BRIDGE_MODE_BT_MIN_WEB) {
      staRuntimeDisabled = true;
      wifiModeText = "AP only + Bluetooth pending reset";
      lastWifiStatus = "AP-only mode saved; reset or power-cycle to apply safely";
    } else {
      scheduleRestart(portsChanged ? "server/AP config change" : "AP config change");
    }
  }

  String returnUrl = "http://" + apIp + "/";
  if (server.hasArg("return")) {
    returnUrl = cleanGpsReturnUrl(urlDecodeSimple(server.arg("return")));
  }

  if (wantsAjax()) {
    if (saveOk) {
      String url = returnUrl;
      String msg = (bridgeMode == BRIDGE_MODE_BT_MIN_WEB)
        ? "AP settings saved. STA WiFi settings preserved. Reset or power-cycle the ESP32 to enter AP-only Bluetooth mode safely."
        : "AP/server settings saved. STA WiFi settings preserved. Device will restart shortly. Redirecting...";
      if (bridgeMode == BRIDGE_MODE_BT_MIN_WEB) sendAjaxOK(msg);
      else sendAjaxRedirectOK(msg, url, 4500);
    } else {
      sendAjaxFail("AP/server settings save failed");
    }
  } else {
    server.sendHeader("Location", saveOk ? returnUrl : "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

void handleClearWiFiPage() {
  logHttpRequest("CLEAR_WIFI");
  LOG_SET_I("Setup button pressed: Reset WiFi To AP Only");
  clearWiFiConfig();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  startFallbackAP();
  wifiModeText = "AP";
  lastWifiStatus = "WiFi config cleared; AP fallback active";

  if (wantsAjax()) sendAjaxOK("WiFi config cleared; AP fallback active");
  else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

void handleClearLogsPage() {
  LOG_WEB_I("Web requested clear logs");
  for (int i = 0; i < LOG_BUFFER_LINES; i++) logBuffer[i] = "";
  logWriteIndex = 0;
  logWrapped = false;
  logAlertActive = false;
  logAlertText = "";
  logAlertMs = 0;

  if (wantsAjax()) sendAjaxOK("Logs cleared");
  else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

void handleClearLogAlertPage() {
  logAlertActive = false;
  logAlertText = "";
  logAlertMs = 0;

  if (wantsAjax()) sendAjaxOK("Log alert cleared");
  else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

bool isRoutinePollPath(const String &path) {
  return path == "/status" || path == "/logs" || path == "/status_text" || path == "/sys_text";
}

void logHttpRequest(const char* where) {
  String path = server.uri();
  currentWebRequestPath = path.c_str();

  bool alpacaPath = path.startsWith("/management") || path.startsWith("/api/");
  if (alpacaPath) alpacaHttpRequests++;
  uint16_t catMask = alpacaPath ? LOG_CAT_ALPACA : LOG_CAT_WEB;
  if (LOG_LEVEL < LOG_DEBUG) return;
  if ((LOG_SUBSYSTEM_MASK & catMask) == 0) return;

  const char* method =
    server.method() == HTTP_GET ? "GET" :
    server.method() == HTTP_PUT ? "PUT" :
    server.method() == HTTP_POST ? "POST" :
    server.method() == HTTP_OPTIONS ? "OPTIONS" : "OTHER";

  bool routine = isRoutinePollPath(path);

  if (!routine || LOG_LEVEL >= LOG_TRACE) {
    String js = server.hasArg("js") ? server.arg("js") : "";
    if (alpacaPath) {
      LOG_ALP_D("%s HTTP %s path=%s args=%d", where, method, path.c_str(), server.args());
    } else if (routine) {
      LOG_WEB_T("%s HTTP %s path=%s args=%d", where, method, path.c_str(), server.args());
    } else if (js.length()) {
      LOG_WEB_D("%s HTTP %s path=%s js=%s args=%d", where, method, path.c_str(), js.c_str(), server.args());
    } else {
      LOG_WEB_D("%s HTTP %s path=%s args=%d", where, method, path.c_str(), server.args());
    }
  }

  // Only trace query args for non-Alpaca web requests to avoid heap churn during Alpaca discovery/client probing.
  if (!alpacaPath && LOG_LEVEL >= LOG_TRACE) {
    for (uint8_t i = 0; i < server.args(); i++) {
      String n = server.argName(i);
      String v = server.arg(i);
      if (n.indexOf("pass") >= 0 || n.indexOf("password") >= 0) v = "<hidden>";
      LOG_WEB_T("  arg[%u] %s=%s", i, n.c_str(), v.c_str());
    }
  }
}

void handleMountTestPage() {
  String s;
  s.reserve(1200);
  s += "Mount serial test " + String(FW_VERSION) + "\n";
  s += "Board: " + String(BOARD_NAME) + "\n";
  s += "ESP32 mount RX GPIO: " + String(MOUNT_RX_PIN) + "\n";
  s += "ESP32 mount TX GPIO: " + String(MOUNT_TX_PIN) + "\n";
  s += "Baud: " + String(MOUNT_BAUD) + "\n";
  s += "Test: send '?' and wait for '#'\n\n";

  int drained = 0;
  while (MountSerial.available()) {
    int d = MountSerial.read();
    drained++;
    s += "Drained before test: 0x" + String(d, HEX);
    if (d >= 32 && d <= 126) {
      s += " '";
      s += (char)d;
      s += "'";
    }
    s += "\n";
  }
  if (drained == 0) s += "No pre-existing serial bytes to drain.\n";

  Serial.println("[MOUNT_TEST] sending '?' handshake on UART2");
  MountSerial.write((uint8_t)'?');
  MountSerial.flush();

  unsigned long start = millis();
  int got = -1;
  while (millis() - start < 1500) {
    if (MountSerial.available()) {
      got = MountSerial.read();
      break;
    }
    delay(1);
    yield();
  }

  if (got < 0) {
    s += "\nResult: NO RESPONSE within 1500 ms\n";
    Serial.println("[MOUNT_TEST] no response");
  } else {
    char c = (got >= 32 && got <= 126) ? (char)got : '.';
    s += "\nResult byte: 0x" + String(got, HEX) + " '";
    s += c;
    s += "'\n";
    s += (got == '#') ? "Handshake: OK\n" : "Handshake: FAILED, expected '#'\n";
    Serial.printf("[MOUNT_TEST] got 0x%02X '%c'\n", got, c);
  }

  s += "\nWiring check:\n";
  s += "ESP32 GPIO17 TX -> level converter TTL RX\n";
  s += "ESP32 GPIO16 RX <- level converter TTL TX\n";
  s += "Converter RS232 side -> NexStar hand controller serial port\n";
  s += "Common ground required on TTL side.\n";
  s += "\nIf result is 0x3F '?', ESP32 is likely reading back its own transmitted byte or TX/RX/level path is wrong.\n";

  sendNoCacheHeaders();
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", s);
}

void handleHttpsSetupRedirectPage() {
  String target = String("https://") + WiFi.softAPIP().toString() + "/";
  if (WiFi.status() == WL_CONNECTED && !requestCameFromApSubnet()) {
    target = String("https://") + WiFi.localIP().toString() + "/";
  }
  target += "?return=" + urlEncodeSimple(cleanGpsReturnUrl(gpsSyncReturnUrl));
  sendNoCacheHeaders();
  server.sendHeader("Connection", "close");
  server.sendHeader("Location", target);
  server.send(302, "text/plain", "Redirecting to HTTPS GPS Sync");
}


String urlEncodeSimple(const String &s) {
  String out;
  out.reserve(s.length() * 3);
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if ((c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

String cleanGpsReturnUrl(const String &raw) {
  String r = raw;
  r.trim();
  if (!r.startsWith("http://")) return String("http://") + WiFi.softAPIP().toString() + "/";
  int scheme = r.indexOf("://");
  int start = (scheme >= 0) ? scheme + 3 : 0;
  int slash = r.indexOf('/', start);
  if (slash < 0) return r + "/";
  String base = r.substring(0, slash + 1);
  return base;
}

String gpsReturnUrlWithCacheBuster() {
  String r = cleanGpsReturnUrl(gpsSyncReturnUrl);
  r += "?gpssync_done=";
  r += String(millis());
  return r;
}

void scheduleHttpsSetupStop(unsigned long delayMs) {
  if (httpsSetupServerHandle) {
    httpsSetupStopAtMillis = millis() + delayMs;
  }
}

void serviceHttpsSetupStop() {
  if (httpsSetupServerHandle && httpsSetupStopAtMillis != 0 && (long)(millis() - httpsSetupStopAtMillis) >= 0) {
    httpd_handle_t h = httpsSetupServerHandle;
    httpsSetupServerHandle = NULL;
    httpsSetupStopAtMillis = 0;
    Serial.println("[HTTPS] stopping GPS Sync HTTPS server after completed sync");
    httpd_ssl_stop(h);
  }
}


void handleStartHttpsPage() {
#if defined(ESP32)
  if (server.hasArg("return")) {
    String r = urlDecodeSimple(server.arg("return"));
    if (r.startsWith("http://")) gpsSyncReturnUrl = cleanGpsReturnUrl(r);
  }

  startHttpsSetupServer();

  String target = String("https://") + WiFi.softAPIP().toString() + "/";
  if (WiFi.status() == WL_CONNECTED && !requestCameFromApSubnet()) {
    target = String("https://") + WiFi.localIP().toString() + "/";
  }
  target += "?return=" + urlEncodeSimple(cleanGpsReturnUrl(gpsSyncReturnUrl));

  sendNoCacheHeaders();
  server.sendHeader("Connection", "close");

  if (server.hasArg("json")) {
    if (httpsSetupServerHandle) {
      String json = "{\"ok\":true,\"url\":\"" + target + "\"}";
      server.send(200, "application/json", json);
    } else {
      server.send(500, "application/json", "{\"ok\":false,\"error\":\"HTTPS GPS Sync server failed to start\"}");
    }
  } else {
    if (httpsSetupServerHandle) server.send(200, "text/plain", target);
    else server.send(500, "text/plain", "HTTPS GPS Sync server failed to start");
  }
#else
  sendNoCacheHeaders();
  server.sendHeader("Connection", "close");
  if (server.hasArg("json")) server.send(501, "application/json", "{\"ok\":false,\"error\":\"HTTPS only available on ESP32\"}");
  else server.send(501, "text/plain", "HTTPS only available on ESP32");
#endif
}

void handleHttpHealthPage() {
  String s;
  s.reserve(500);
  s += String(FW_NAME) + " " + String(FW_VERSION) + " HTTP health OK\n";
  s += "Bridge mode: " + String(bridgeModeName()) + "\n";
  s += "Web UI HTTP port: " + String(HTTP_WEB_PORT) + "\n";
  s += "Alpaca HTTP port: " + String(bridgeMode == BRIDGE_MODE_WIFI_FULL ? ALPACA_PORT : 0) + "\n";
  s += "AP IP: " + WiFi.softAPIP().toString() + "\n";
  s += "STA IP: " + WiFi.localIP().toString() + "\n";
  s += "Bluetooth SkySafari: " + String(bluetoothRuntimeEnabled ? "enabled" : "disabled") + "\n";
  s += "Coex preference: " + coexPreferenceText + " result=" + String(coexPreferenceResult) + "\n";
  s += "HTTPS port: " + String(HTTPS_SETUP_PORT) + "\n";
  sendNoCacheHeaders();
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", s);
}

void handleTimeLocationStatusPage() {
  String s;
  s.reserve(1200);
  s += "Time / Location status " + String(FW_VERSION) + "\n";
  s += "Time valid: " + String(timeValid ? "true" : "false") + "\n";
  s += "Time source: " + String(timeSourceName(currentTimeSource)) + "\n";
  s += "UTC offset minutes: " + String(utcOffsetMinutes) + "\n";
  s += "UTC offset source: " + utcOffsetSource + "\n";
  s += "Local date: " + String(localYear) + "-";
  if (localMonth < 10) s += "0";
  s += String(localMonth) + "-";
  if (localDay < 10) s += "0";
  s += String(localDay) + "\n";
  s += "Local time: ";
  if (localHour < 10) s += "0";
  s += String(localHour) + ":";
  if (localMinute < 10) s += "0";
  s += String(localMinute) + ":";
  if (localSecond < 10) s += "0";
  s += String(localSecond) + "\n";
  s += "Site valid: " + String(siteValid ? "true" : "false") + "\n";
  s += "Location source: " + currentLocationSource + "\n";
  s += "Latitude: " + String(siteLatitudeDeg, 6) + "\n";
  s += "Longitude: " + String(siteLongitudeDeg, 6) + "\n";
  sendNoCacheHeaders();
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", s);
}

bool browserTimeLocationEstablished() {
  return timeValid && siteValid;
}

bool requestCameFromApSubnet() {
#if defined(ESP32)
  if (!apRunning) return false;
  IPAddress rip = server.client().remoteIP();
  IPAddress ap = WiFi.softAPIP();
  return rip[0] == ap[0] && rip[1] == ap[1] && rip[2] == ap[2];
#else
  return false;
#endif
}

String currentTimezoneAbbrev() {
  if (utcOffsetMinutes == -420) return "MST";
  if (utcOffsetMinutes == -360) return "MDT";
  int off = utcOffsetMinutes;
  char sign = '+';
  if (off < 0) {
    sign = '-';
    off = -off;
  }
  char buf[12];
  snprintf(buf, sizeof(buf), "UTC%c%02d:%02d", sign, off / 60, off % 60);
  return String(buf);
}

void redirectToHttpsSetup443() {
#if defined(ESP32)
  String target = String("https://") + WiFi.softAPIP().toString() + "/";
  server.sendHeader("Location", target);
  server.send(302, "text/plain", "Redirecting to secure browser time/location setup");
#else
  sendWebPage();
#endif
}

void sendMinimalWebPage() {
  bool btConn = false;
#if HAS_CLASSIC_BT
  btConn = bluetoothRuntimeEnabled && SerialBT.hasClient();
#endif
  String page;
  page.reserve(4200);
  page += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<meta http-equiv='Cache-Control' content='no-store'><title>NexStar Minimal</title>");
  page += F("<style>body{font-family:Arial,sans-serif;background:#111;color:#eee;margin:14px}button,input,.btn{font-size:1em;margin:4px;padding:8px}input{width:190px;max-width:95%}.btn,button{display:inline-block;border:1px solid #666;background:#333;color:#eee;border-radius:4px;text-decoration:none}.box{border:1px solid #444;background:#1b1b1b;border-radius:8px;padding:10px;margin:10px 0}.ok{color:#8cff8c}.warn{color:#ffd36b}.bad{color:#ff8888}pre{white-space:pre-wrap;background:#000;color:#0f0;padding:8px;border-radius:6px}</style>");
  page += F("<script>async function upd(){try{let r=await fetch('/btstatus?t='+Date.now(),{cache:'no-store'}),j=await r.json();");
  page += F("let bt=j.bluetoothConnected?'connected':(j.bluetoothEnabled?'waiting':'disabled'),bc=j.bluetoothConnected?'ok':(j.bluetoothEnabled?'warn':'bad');");
  page += F("function t(id,v){let e=document.getElementById(id);if(e)e.textContent=v}function c(id,v){let e=document.getElementById(id);if(e)e.className=v}");
  page += F("t('m',j.bridgeMode||'?');t('bt',bt);c('bt',bc);t('ap',j.apIp||'?');t('sta',j.staConnected?(j.staIp||'?'):'not connected');");
  page += F("t('mnt',j.mountCommFault?'fault':(j.mountBusy?'busy':'ready'));t('mountUptime',(j.mountUptime||'0:00')+' h:mm');t('cnt',(j.lx200BtRxCommands||0)+' / '+(j.lx200BtTxReplies||0));");
  page += F("t('time',j.timeValid?('set '+(j.timeSource||'')):('not set'));t('loc',(j.siteValid?((j.locationSource||'set')+' '+Number(j.latitude).toFixed(6)+', '+Number(j.longitude).toFixed(6)):'not set'));t('lastcmd',j.lx200BtLastCommand||'none');t('lastrsp',j.lx200BtLastReply||'none');t('lastbad',j.lx200BtLastUnhandled||'none');");
  page += F("t('pollms',j.pollIntervalMs===0?'off':((j.pollIntervalMs||0)+' ms'));let p=document.getElementById('poll');if(p&&document.activeElement!==p)p.value=j.pollIntervalMs||0;");
  page += F("t('live','live '+new Date().toLocaleTimeString());}catch(e){t('live','status update failed')}}setInterval(upd,8000);window.addEventListener('load',upd);</script>");
  page += F("</head><body><h2>");
  page += FW_NAME;
  page += F(" ");
  page += FW_VERSION;
  page += F("</h2><div class='box'><b>Mode:</b> <span id='m'>");
  page += bridgeModeName();
  page += F("</span><br><b>Bluetooth SkySafari:</b> <span id='bt' class='");
  page += btConn ? F("ok'>connected") : (bluetoothRuntimeEnabled ? F("warn'>waiting") : F("bad'>disabled"));
  page += F("</span><br><b>BT name:</b> ");
  page += runtimeBtName();
  page += F("<br><b>AP:</b> <span id='ap'>");
  page += WiFi.softAPIP().toString();
  page += F("</span><br><b>STA:</b> <span id='sta'>");
  page += (staConnected ? WiFi.localIP().toString() : String("not connected"));
  page += F("</span><br><b>Mount:</b> <span id='mnt'>");
  page += mountCommFault ? F("fault") : (mountBusy ? F("busy") : F("ready"));
  page += F("</span><br><b>Mount uptime:</b> <span id='mountUptime'>");
  page += htmlEscape(formatMountUptime());
  page += F(" h:mm");
  page += F("</span><br><b>SkySafari BT RX/TX:</b> <span id='cnt'>");
  page += String(lx200BtRxCommands);
  page += F(" / ");
  page += String(lx200BtTxReplies);
  page += F("</span><br><b>Time:</b> <span id='time'>");
  page += timeValid ? String("set ") + timeSourceName(currentTimeSource) : String("not set");
  page += F("</span><br><b>Location:</b> <span id='loc'>");
  if (siteValid) {
    page += htmlEscape(currentLocationSource);
    page += F(" ");
    page += String(siteLatitudeDeg, 6);
    page += F(", ");
    page += String(siteLongitudeDeg, 6);
  } else {
    page += F("not set");
  }
  page += F("</span><br><b>Last BT RX:</b> <span id='lastcmd'>");
  page += htmlEscape(lx200BtLastCommand.length() ? lx200BtLastCommand : String("none"));
  page += F("</span><br><b>Last BT TX:</b> <span id='lastrsp'>");
  page += htmlEscape(lx200BtLastReply.length() ? lx200BtLastReply : String("none"));
  page += F("</span><br><b>Last unsupported:</b> <span id='lastbad'>");
  page += htmlEscape(lx200BtLastUnhandled.length() ? lx200BtLastUnhandled : String("none"));
  page += F("</span><br><b>Mount poll:</b> <span id='pollms'>");
  page += pollIntervalMs == 0 ? String("off") : String(pollIntervalMs) + " ms";
  page += F("</span><br><span id='live' class='small'>loading live status...</span></div><div class='box'><a class='btn' href='/mode?radio=wifi'>WiFi</a>");
  page += F("<button onclick=\"location.reload()\">Refresh</button></div>");
  page += F("<div class='box'><h3>Status</h3>");
  page += F("Time: ");
  if (timeValid) {
    int yy, mo, dd, hh, mi, ss;
    currentLocalParts(yy, mo, dd, hh, mi, ss);
    page += String(yy) + "-";
    if (mo < 10) page += "0";
    page += String(mo) + "-";
    if (dd < 10) page += "0";
    page += String(dd) + " ";
    if (hh < 10) page += "0";
    page += String(hh) + ":";
    if (mi < 10) page += "0";
    page += String(mi);
    page += " ";
    page += currentTimezoneAbbrev();
    page += " from ";
    page += timeSourceName(currentTimeSource);
  } else {
    page += F("not set");
  }
  page += F("<br>Location: ");
  if (siteValid) {
    page += String(siteLatitudeDeg, 5);
    page += F(", ");
    page += String(siteLongitudeDeg, 5);
    page += F(" from ");
    page += htmlEscape(currentLocationSource);
  } else {
    page += F("not set");
  }
  page += F("<br>Mount serial: ");
  page += mountCommFault ? F("fault") : F("ok");
  page += F("</div></body></html>");
  sendNoCacheHeaders();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  const size_t chunkSize = 512;
  for (size_t i = 0; i < page.length(); i += chunkSize) {
    server.sendContent(page.substring(i, i + chunkSize));
    delay(1);
    yield();
  }
  server.sendContent("");
}

void handleRoot() {
  logHttpRequest("ROOT");
  // v3.30 recovery: port 80 must always answer with the normal UI.
  // HTTPS experiments stay separate and must not break HTTP reachability.
  if (bridgeMode == BRIDGE_MODE_BT_MIN_WEB) sendMinimalWebPage();
  else sendWebPage();
}

void handleManagementApiVersions() {
  logHttpRequest("MGMT apiversions");
  sendJson("{\"Value\":[1]}");
}

void handleManagementDescription() {
  logHttpRequest("MGMT description");
  sendJson("{\"ServerName\":\"Portable NexStar Bridge\",\"Manufacturer\":\"Local\",\"ManufacturerVersion\":\"1.07-utc-offset-fix\",\"Location\":\"Local network\"}");
}

void handleConfiguredDevices() {
  logHttpRequest("MGMT configureddevices");
  sendJson("{\"Value\":[{\"DeviceName\":\"Original NexStar 8\",\"DeviceType\":\"Telescope\",\"DeviceNumber\":0,\"UniqueID\":\"portable-nexstar-0\"}]}");
}

void handleOptions() {
  logHttpRequest("OPTIONS");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,PUT,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

void handleTelescopeGet(const String &action) {
  LOG_ALP_I("Alpaca GET action=%s", action.c_str());

  if (action == "rightascension" || action == "declination" ||
      action == "altitude" || action == "azimuth") {
    markPositionDemand("Alpaca position GET");
  }

  serviceMountPolling();

  if (action == "name") sendAlpacaValue("\"Original NexStar 8\"");
  else if (action == "description") sendAlpacaValue("\"Portable ESP8266/ESP32/ESP32-S2 bridge for original NexStar serial protocol\"");
  else if (action == "driverinfo") sendAlpacaValue("\"Alpaca plus LX200 WiFi/Bluetooth SkySafari bridge with cached polling\"");
  else if (action == "driverversion") sendAlpacaValue("\"1.07-utc-offset-fix\"");
  else if (action == "interfaceversion") sendAlpacaValue("3");
  else if (action == "connected") sendAlpacaValue(alpacaConnected ? "true" : "false");
  else if (action == "supportedactions") sendAlpacaValue("[]");

  else if (action == "rightascension") sendAlpacaValue(String(cachedRA_deg / 15.0, 8));
  else if (action == "declination") sendAlpacaValue(String(cachedDec_deg, 8));

  else if (action == "altitude") {
    double alt, az;
    getAltAz(alt, az, false);
    sendAlpacaValue(String(cachedAlt_deg, 8));
  }

  else if (action == "azimuth") {
    double alt, az;
    getAltAz(alt, az, false);
    sendAlpacaValue(String(cachedAz_deg, 8));
  }

  else if (action == "targetrightascension") sendAlpacaValue(String(targetRA_deg / 15.0, 8));
  else if (action == "targetdeclination") sendAlpacaValue(String(targetDec_deg, 8));
  else if (action == "destinationrightascension") sendAlpacaValue(String(targetRA_deg / 15.0, 8));
  else if (action == "destinationdeclination") sendAlpacaValue(String(targetDec_deg, 8));
  else if (action == "slewing") sendAlpacaValue((mountBusy || asyncSlewPending || asyncAltAzSlewPending || asyncNudgePending || asyncRaDecReadPending || asyncAltAzReadPending || asyncSlewRunning) ? "true" : "false");

  else if (action == "tracking") sendAlpacaValue("true");
  else if (action == "canslew") sendAlpacaValue("true");
  else if (action == "canslewasync") sendAlpacaValue("true");
  else if (action == "canslewaltaz") sendAlpacaValue("true");
  else if (action == "canslewaltazasync") sendAlpacaValue("false");
  else if (action == "cansync") sendAlpacaValue("false");
  else if (action == "cansyncaltaz") sendAlpacaValue("false");
  else if (action == "canpark") sendAlpacaValue("false");
  else if (action == "canunpark") sendAlpacaValue("false");
  else if (action == "canfindhome") sendAlpacaValue("false");
  else if (action == "canmoveaxis") sendAlpacaValue("true");
  else if (action == "canpulseguide") sendAlpacaValue("false");
  else if (action == "cansettracking") sendAlpacaValue("false");
  else if (action == "cansetrightascensionrate") sendAlpacaValue("false");
  else if (action == "cansetdeclinationrate") sendAlpacaValue("false");
  else if (action == "alignmentmode") sendAlpacaValue("0");
  else if (action == "equatorialsystem") sendAlpacaValue("1");
  else if (action == "athome") sendAlpacaValue("false");
  else if (action == "atpark") sendAlpacaValue("false");
  else if (action == "pierside") sendAlpacaValue("0");
  else if (action == "siteelevation") sendAlpacaValue(String(siteElevationMeters, 2));
  else if (action == "sitelatitude") sendAlpacaValue(String(siteLatitudeDeg, 8));
  else if (action == "sitelongitude") sendAlpacaValue(String(siteLongitudeDeg, 8));
  else if (action == "declinationrate") sendAlpacaValue("0");
  else if (action == "rightascensionrate") sendAlpacaValue("0");
  else if (action == "doesrefraction") sendAlpacaValue("false");
  else if (action == "siderealtime") sendAlpacaValue(String(normalizeRA(gmstDegrees() + siteLongitudeDeg) / 15.0, 8));
  else if (action == "utcdate") sendAlpacaValue("\"" + currentUtcIsoString() + "\"");
  else if (action == "trackingrates") sendAlpacaValue("[]");
  else if (action == "axisrates") sendAlpacaValue("[{\"Minimum\":-4.0,\"Maximum\":4.0}]");
  else sendAlpacaError(1024, "GET action not implemented: " + action);
}

void queueAsyncSlew(double raDeg, double decDeg) {
  pendingRA_deg = normalizeRA(raDeg);
  pendingDec_deg = decDeg;
  targetRA_deg = pendingRA_deg;
  targetDec_deg = pendingDec_deg;
  asyncSlewEarliestMs = millis() + 750;
  lastGotoAcceptedMs = millis();
  asyncSlewPending = true;
  LOG_MOUNT_I("Queued async slew RA_deg=%.6f DEC_deg=%.6f", pendingRA_deg, pendingDec_deg);
}

void queueAsyncAltAzSlew(double altDeg, double azDeg) {
  pendingAlt_deg = clampAlt(altDeg);
  pendingAz_deg = normalizeAz(azDeg);
  asyncAltAzSlewEarliestMs = millis() + 750;
  lastGotoAcceptedMs = millis();
  asyncAltAzSlewPending = true;
  LOGI("Queued async Alt/Az slew ALT_deg=%.6f AZ_deg=%.6f", pendingAlt_deg, pendingAz_deg);
}

void queueAsyncNudge(double altDelta, double azDelta) {
  pendingNudgeAltDelta = altDelta;
  pendingNudgeAzDelta = azDelta;
  asyncNudgePending = true;
  LOGI("Queued async nudge ALT_delta=%.6f AZ_delta=%.6f", pendingNudgeAltDelta, pendingNudgeAzDelta);
}



bool parseAlpacaUtcDate(const String &raw) {
  if (raw.length() < 19) return false;

  int y = raw.substring(0, 4).toInt();
  int mo = raw.substring(5, 7).toInt();
  int d = raw.substring(8, 10).toInt();
  int h = raw.substring(11, 13).toInt();
  int mi = raw.substring(14, 16).toInt();
  int se = raw.substring(17, 19).toInt();

  if (y < 2000 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 60) return false;

  pendingLocalYear = y;
  pendingLocalMonth = mo;
  pendingLocalDay = d;
  pendingLocalHour = h;
  pendingLocalMinute = mi;
  pendingLocalSecond = se;
  pendingUtcDateSet = true;
  pendingSiteTimeUpdate = true;
  return true;
}

void applyPendingSiteTimeUpdate() {
  if (!pendingSiteTimeUpdate) return;

  if (pendingSiteLatitudeSet) {
    siteLatitudeDeg = pendingSiteLatitudeDeg;
    siteValid = true;
  }
  if (pendingSiteLongitudeSet) {
    siteLongitudeDeg = pendingSiteLongitudeDeg;
    siteValid = true;
  }
  if (pendingSiteElevationSet) {
    siteElevationMeters = pendingSiteElevationMeters;
  }
  if (pendingUtcDateSet) {
    int offsetToUse = utcOffsetMinutes;

    // Alpaca UTCDate is UTC and does not include the local UTC offset.
    // If we have site longitude and no better offset has been supplied, estimate
    // the standard time-zone offset from longitude so the status page is useful.
    if (offsetToUse == 0 && (pendingSiteLongitudeSet || siteValid)) {
      double lonForOffset = pendingSiteLongitudeSet ? pendingSiteLongitudeDeg : siteLongitudeDeg;
      offsetToUse = estimateUtcOffsetMinutesFromLongitude(lonForOffset);
      utcOffsetSource = "Alpaca UTCDate + longitude estimate";
    } else if (utcOffsetSource == "unset") {
      utcOffsetSource = "Alpaca UTCDate as UTC";
    }

    utcOffsetMinutes = offsetToUse;
    setLocalFromUtcWithOffset(
      pendingLocalYear, pendingLocalMonth, pendingLocalDay,
      pendingLocalHour, pendingLocalMinute, pendingLocalSecond,
      utcOffsetMinutes
    );
    timeSetMillis = millis();
    timeValid = true;
  }

  pendingSiteLatitudeSet = false;
  pendingSiteLongitudeSet = false;
  pendingSiteElevationSet = false;
  pendingUtcDateSet = false;
  pendingSiteTimeUpdate = false;

  invalidateComputedAltAz();
  if (cacheValid && siteValid && timeValid) computeAltAzFromRaDec();

  savePersistentSettings();
  LOGI("Applied pending Alpaca site/time: siteValid=%d timeValid=%d lat=%.6f lon=%.6f elev=%.2f",
       siteValid, timeValid, siteLatitudeDeg, siteLongitudeDeg, siteElevationMeters);
}

void handleTelescopePut(const String &action) {
  LOGI("Alpaca PUT action=%s args=%d", action.c_str(), server.args());
  if (action == "sitelatitude" || action == "sitelongitude" || action == "siteelevation" || action == "utcdate") {
    for (uint8_t i = 0; i < server.args(); i++) {
      LOGI("  Alpaca arg[%u] %s=%s", i, server.argName(i).c_str(), server.arg(i).c_str());
    }
  }

  const uint32_t minAlpacaPutHeap =
#if defined(ESP8266)
    4500;
#else
    20000;
#endif
  if (ESP.getFreeHeap() < minAlpacaPutHeap) {
    LOGW("Low memory before Alpaca PUT: freeHeap=%u min=%u", ESP.getFreeHeap(), minAlpacaPutHeap);
    sendAlpacaError(1035, "Low memory; request deferred");
    return;
  }

  if (action == "connected") {
    String v = alpacaPutValue("Connected");
    alpacaConnected = !(v == "false" || v == "False" || v == "0");
    sendAlpacaOK();
    return;
  }

  if (action == "sitelatitude" || action == "latitude") {
    String v = alpacaPutValue("SiteLatitude");
    if (v.length() == 0) v = alpacaPutValue("Latitude");
    double parsed = 0.0;
    if (!safeFloatValue(v, parsed) || parsed < -90.0 || parsed > 90.0) {
      sendAlpacaError(1025, "Invalid SiteLatitude value");
      return;
    }
    pendingSiteLatitudeDeg = parsed;
    pendingSiteLatitudeSet = true;
    pendingSiteTimeUpdate = true;
    LOGI("Alpaca RECEIVED SiteLatitude %.6f", parsed);
    sendAlpacaOK();
    return;
  }

  if (action == "sitelongitude" || action == "longitude") {
    String v = alpacaPutValue("SiteLongitude");
    if (v.length() == 0) v = alpacaPutValue("Longitude");
    double parsed = 0.0;
    if (!safeFloatValue(v, parsed) || parsed < -180.0 || parsed > 180.0) {
      sendAlpacaError(1025, "Invalid SiteLongitude value");
      return;
    }
    pendingSiteLongitudeDeg = parsed;
    pendingSiteLongitudeSet = true;
    pendingSiteTimeUpdate = true;
    LOGI("Alpaca RECEIVED SiteLongitude %.6f east-positive", parsed);
    sendAlpacaOK();
    return;
  }

  if (action == "siteelevation" || action == "elevation") {
    String v = alpacaPutValue("SiteElevation");
    if (v.length() == 0) v = alpacaPutValue("Elevation");
    double parsed = 0.0;
    if (!safeFloatValue(v, parsed) || parsed < -500.0 || parsed > 10000.0) {
      sendAlpacaError(1025, "Invalid SiteElevation value");
      return;
    }
    pendingSiteElevationMeters = parsed;
    pendingSiteElevationSet = true;
    pendingSiteTimeUpdate = true;
    LOGI("Alpaca RECEIVED SiteElevation %.2f m", parsed);
    sendAlpacaOK();
    return;
  }

  if (action == "utcdate") {
    String v = alpacaPutValue("UTCDate");
    if (v.length() == 0 || v.length() > 32) {
      sendAlpacaError(1025, "Invalid UTCDate value");
      return;
    }
    if (!parseAlpacaUtcDate(v)) {
      sendAlpacaError(1025, "Invalid UTCDate format");
      return;
    }
    LOGI("Alpaca RECEIVED UTCDate %s", v.c_str());
    sendAlpacaOK();
    return;
  }

  if (action == "targetrightascension") {
    targetRA_deg = normalizeRA(alpacaPutValue("TargetRightAscension").toFloat() * 15.0);
    sendAlpacaOK();
    return;
  }

  if (action == "targetdeclination") {
    targetDec_deg = alpacaPutValue("TargetDeclination").toFloat();
    sendAlpacaOK();
    return;
  }

  if (action == "slewtocoordinates" || action == "slewtocoordinatesasync") {
    double raDeg = alpacaPutValue("RightAscension").toFloat() * 15.0;
    double decDeg = alpacaPutValue("Declination").toFloat();
    if (mountCommandPathBusy()) {
      enqueueGotoRaDec(raDeg, decDeg, LX_SRC_WIFI, "Alpaca SlewToCoordinates while mount busy");
    } else {
      queueAsyncSlew(raDeg, decDeg);
    }
    markGotoQueueImmediateAck("Alpaca", "SlewToCoordinates accepted");
    sendAlpacaOK();
    return;
  }

  if (action == "slewtotarget" || action == "slewtotargetasync") {
    if (mountCommandPathBusy()) {
      enqueueGotoRaDec(targetRA_deg, targetDec_deg, LX_SRC_WIFI, "Alpaca SlewToTarget while mount busy");
    } else {
      queueAsyncSlew(targetRA_deg, targetDec_deg);
    }
    markGotoQueueImmediateAck("Alpaca", "SlewToTarget accepted");
    sendAlpacaOK();
    return;
  }

  if (action == "slewtoaltaz" || action == "slewtoaltazasync") {
    double altDeg = alpacaPutValue("Altitude").toFloat();
    double azDeg = alpacaPutValue("Azimuth").toFloat();
    if (mountCommandPathBusy()) {
      enqueueGotoAltAz(altDeg, azDeg, LX_SRC_WIFI, "Alpaca SlewToAltAz while mount busy");
    } else {
      queueAsyncAltAzSlew(altDeg, azDeg);
    }
    markGotoQueueImmediateAck("Alpaca", "SlewToAltAz accepted");
    sendAlpacaOK();
    return;
  }

  if (action == "moveaxis") {
    bool ok = nudgeAxis(alpacaPutValue("Axis").toInt(), alpacaPutValue("Rate").toFloat());
    if (ok) sendAlpacaOK();
    else sendAlpacaError(1035, "MoveAxis nudge failed");
    return;
  }

  if (action == "abortslew") {
    sendAlpacaError(1024, "AbortSlew is not supported by this original NexStar protocol");
    return;
  }

  sendAlpacaError(1024, "PUT action not implemented: " + action);
}

void handleWebNotFound() {
  logHttpRequest("WEB_NOT_FOUND");
  server.send(404, "text/plain", "Not found on Web UI port. Alpaca API is on port " + String(ALPACA_PORT));
}

void handleNotFound() {
  logHttpRequest("REQ");

  String path = server.uri();

  if (server.method() == HTTP_OPTIONS) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET,PUT,POST,OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.send(204);
    return;
  }

  if (path == "/management/apiversions") {
    handleManagementApiVersions();
    return;
  }

  if (path == "/management/v1/description") {
    handleManagementDescription();
    return;
  }

  if (path == "/management/v1/configureddevices") {
    handleConfiguredDevices();
    return;
  }

  String prefix0 = "/api/v1/telescope/0/";
  String prefixMinus1 = "/api/v1/telescope/-1/";
  String action = "";

  if (path.startsWith(prefix0)) action = path.substring(prefix0.length());
  else if (path.startsWith(prefixMinus1)) action = path.substring(prefixMinus1.length());

  if (action.length() > 0) {
    action.toLowerCase();
    if (server.method() == HTTP_GET) handleTelescopeGet(action);
    else if (server.method() == HTTP_PUT || server.method() == HTTP_POST) handleTelescopePut(action);
    else sendAlpacaError(1024, "Unsupported HTTP method");
    return;
  }

  server.send(404, "application/json", "{\"Error\":\"Not found\"}");
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



String consoleLogMaskNames(uint16_t mask) {
  if (mask == 0) return "none";
  if ((mask & LOG_CAT_ALL) == LOG_CAT_ALL) return "all";

  String s = "";
  if (mask & LOG_CAT_MOUNT)      s += "mount,";
  if (mask & LOG_CAT_SKYSAFARI)  s += "skysafari,";
  if (mask & LOG_CAT_BLUETOOTH)  s += "bluetooth,";
  if (mask & LOG_CAT_ALPACA)     s += "alpaca,";
  if (mask & LOG_CAT_STELLARIUM) s += "stellarium,";
  if (mask & LOG_CAT_WEB)        s += "web,";
  if (mask & LOG_CAT_WIFI)       s += "wifi,";
  if (mask & LOG_CAT_TIMELOC)    s += "time_location,";
  if (mask & LOG_CAT_SETTINGS)   s += "settings,";
  if (mask & LOG_CAT_SYSTEM)     s += "system,";
  if (s.endsWith(",")) s.remove(s.length() - 1);
  return s.length() ? s : "none";
}

void printConsoleLogStatus() {
  Serial.println("Log status:");
  Serial.printf("  level: %d (%s)\n", LOG_LEVEL, logLevelName(LOG_LEVEL));
  Serial.printf("  system mask: 0x%04X\n", LOG_SUBSYSTEM_MASK);
  Serial.print("  systems: ");
  Serial.println(consoleLogMaskNames(LOG_SUBSYSTEM_MASK));
}


uint16_t consoleLogMaskFromList(String cats) {
  cats.trim();
  cats.toLowerCase();
  cats.replace(",", " ");
  cats.replace(";", " ");
  cats.replace("|", " ");
  cats.replace("+", " ");

  if (cats == "all") return LOG_CAT_ALL;
  if (cats == "none" || cats == "off" || cats.length() == 0) return 0;

  uint16_t mask = 0;
  int start = 0;
  while (start < (int)cats.length()) {
    while (start < (int)cats.length() && cats.charAt(start) == ' ') start++;
    if (start >= (int)cats.length()) break;

    int sp = cats.indexOf(' ', start);
    String part = sp >= 0 ? cats.substring(start, sp) : cats.substring(start);
    part.trim();

    if (part == "bt") part = "bluetooth";
    else if (part == "sky") part = "skysafari";
    else if (part == "safari") part = "skysafari";
    else if (part == "time") part = "time_location";
    else if (part == "timeloc") part = "time_location";
    else if (part == "location") part = "time_location";
    else if (part == "config") part = "settings";
    else if (part == "setup") part = "settings";
    else if (part == "wifi") part = "wifi";
    else if (part == "wi-fi") part = "wifi";

    uint16_t bit = logCategoryBitFromName(part);
    if (bit == 0 && part.length() > 0) {
      Serial.print("[console] unknown log system ignored: ");
      Serial.println(part);
    }
    mask |= bit;

    if (sp < 0) break;
    start = sp + 1;
  }
  return mask;
}

void printHelpLog() {
  Serial.println();
  Serial.println("=== Logging Commands ===");
  Serial.println("  log");
  Serial.println("      Show the current log level, subsystem mask, and enabled subsystems.");
  Serial.println("  log <0..5>");
  Serial.println("      Set log level:");
  Serial.println("        0 none   Disable stored/streamed firmware log output.");
  Serial.println("        1 error  Only failures that usually need action.");
  Serial.println("        2 warn   Errors plus abnormal/retry/fallback conditions.");
  Serial.println("        3 info   Normal high-level events: clients, saves, connect/disconnect.");
  Serial.println("        4 debug  Detailed decisions and state changes for troubleshooting.");
  Serial.println("        5 trace  Very chatty low-level flow; use briefly, can flood logs.");
  Serial.println("  log <0..5> [systems]");
  Serial.println("      Set log level and one or more subsystems separated by spaces or commas.");
  Serial.println("      Systems: all, none, mount, skysafari, bluetooth, wifi, web, alpaca,");
  Serial.println("               stellarium, time_location, settings, system");
  Serial.println("      Aliases: bt=bluetooth, sky/lx200=skysafari, time/location/ntp=time_location,");
  Serial.println("               config/setup/littlefs=settings, sys=system");
  Serial.println("      Examples: log 3 system   |   log 4 mount bluetooth   |   log 5 mount,skysafari");
  Serial.println("      SkySafari position-poll traffic is intentionally suppressed to prevent log flooding.");
}

void printHelpMode() {
  Serial.println();
  Serial.println("=== Mode and Runtime Commands ===");
  Serial.println("  modebt | modebt_web");
  Serial.println("      Save BT mode with the small BT web interface enabled, then reboot.");
  Serial.println("  modebt_telnet | modebt_noweb | modebt_telnetonly");
  Serial.println("      Save BT Telnet-only mode with the small web interface disabled, then reboot.");
  Serial.println("  modewifi");
  Serial.println("      Save Full WiFi mode, then reboot.");
  Serial.println("  web | web status | webserver | webserver status");
  Serial.println("      Show the BT web-server runtime state.");
  Serial.println("  web on | web off | web toggle");
  Serial.println("      Change the BT web-server runtime state. This is not saved across reboot.");
  Serial.println("  wifi | wifi status");
  Serial.println("      Show WiFi runtime, AP/STA connection, and IP information.");
  Serial.println("  wifi on | wifi off | wifi toggle");
  Serial.println("      Change WiFi runtime state. In BT mode, Bluetooth remains active when WiFi is off.");
  Serial.println("  reboot | restart");
  Serial.println("      Restart the controller immediately.");
}

void printHelpWifi() {
  Serial.println();
  Serial.println("=== WiFi Configuration Commands ===");
  Serial.println("  setsta <ssid> <password>");
  Serial.println("      Save STA credentials and immediately attempt a connection.");
  Serial.println("      SSIDs and passwords containing spaces are not supported by this console command.");
  Serial.println("  apdefault");
  Serial.println("      Restore AP SSID, password, and IP defaults, then restart the AP.");
  Serial.println("  wifi | wifi status");
  Serial.println("      Show current WiFi and IP status.");
  Serial.println("  wifi on | wifi off | wifi toggle");
  Serial.println("      Enable, disable, or toggle WiFi at runtime.");
  Serial.println("      Use the Full WiFi Setup page for complete STA and AP configuration.");
}

void printHelpMount() {
  Serial.println();
  Serial.println("=== Mount and Polling Commands ===");
  Serial.println("  testinit");
  Serial.println("      Test the NexStar '?' handshake and safe E-command payload read.");
  Serial.println("  get");
  Serial.println("      Query and print current RA/Dec.");
  Serial.println("  getaltaz");
  Serial.println("      Query and print current Alt/Az.");
  Serial.println("  pos | current_state | currentstate | state");
  Serial.println("      Print cached telescope coordinates and current bridge state.");
  Serial.println("  rates");
  Serial.println("      Show all four nudge rates, mount poll interval, and client-poll throttle.");
  Serial.println("  mountpoll | mountpoll <ms>");
  Serial.println("      Get or set the active mount poll interval. 0 turns it off.");
  Serial.println("  nudge az+ | nudge az- | nudge alt+ | nudge alt-");
  Serial.println("      Perform one relative Alt/Az nudge using nudge-rate slot 2.");
  Serial.println("  drain");
  Serial.println("      Drain and print pending bytes from the mount serial input.");
}

void printHelpStatus() {
  Serial.println();
  Serial.println("=== Status and Diagnostics Commands ===");
  Serial.println("  status");
  Serial.println("      Show firmware, mode, network, clients, GOTO queue, heap, loop latency,");
  Serial.println("      poll scheduler timing, and mount communication status.");
  Serial.println("  current_state | currentstate | state | pos");
  Serial.println("      Show current cached telescope coordinates and operational state.");
  Serial.println("  system_health | systemhealth | health");
  Serial.println("      Show system-health counters, loop and poll latency, memory, uptime, and errors.");
  Serial.println("  tasks");
  Serial.println("      Show FreeRTOS runtime task stats if enabled in the build.");
  Serial.println("  gpio_startup | gpiostartup | startup_pins");
  Serial.println("      Show startup mode-pin readings and the selected boot-mode source.");
  Serial.println("  telnet | telnet status");
  Serial.println("      Show Telnet port, connection, authentication, and command counters.");
}

void printHelpTopic(const String &topicRaw) {
  String topic = topicRaw;
  topic.trim();
  topic.toLowerCase();

  if (topic == "mount" || topic == "scope" || topic == "nexstar") {
    printHelpMount();
  } else if (topic == "mode" || topic == "boot" || topic == "runtime" || topic == "radio") {
    printHelpMode();
  } else if (topic == "wifi" || topic == "network") {
    printHelpWifi();
  } else if (topic == "log" || topic == "logging" || topic == "logs") {
    printHelpLog();
  } else if (topic == "web" || topic == "webserver") {
    Serial.println();
    Serial.println("=== Web Server Command ===");
    Serial.println("  web | web status | webserver | webserver status");
    Serial.println("      Show the BT web-server runtime state.");
    Serial.println("  web on | web off | web toggle");
    Serial.println("      Enable, disable, or toggle the BT web server for this boot only.");
  } else if (topic == "reboot" || topic == "restart") {
    Serial.println();
    Serial.println("=== Reboot Command ===");
    Serial.println("  reboot | restart");
    Serial.println("      Restart the controller immediately.");
  } else if (topic == "testinit") {
    Serial.println();
    Serial.println("=== testinit ===");
    Serial.println("  testinit");
    Serial.println("      Test the NexStar '?' handshake and safe E-command payload read.");
  } else if (topic == "get") {
    Serial.println();
    Serial.println("=== get ===");
    Serial.println("  get");
    Serial.println("      Query the mount and print current RA/Dec.");
  } else if (topic == "getaltaz") {
    Serial.println();
    Serial.println("=== getaltaz ===");
    Serial.println("  getaltaz");
    Serial.println("      Query the mount and print current Alt/Az.");
  } else if (topic == "rates" || topic == "poll" || topic == "mountpoll") {
    Serial.println();
    Serial.println("=== Mount Poll / Rates ===");
    Serial.println("  rates");
    Serial.println("      Show all four nudge rates, mount poll interval, and client-poll throttle.");
    Serial.println("  mountpoll | mountpoll <ms>");
    Serial.println("      Get or set the active mount poll interval. 0 turns it off.");
  } else if (topic == "nudge") {
    Serial.println();
    Serial.println("=== nudge ===");
    Serial.println("  nudge az+ | nudge az- | nudge alt+ | nudge alt-");
    Serial.println("      Perform one relative Alt/Az nudge using nudge-rate slot 2.");
  } else if (topic == "drain") {
    Serial.println();
    Serial.println("=== drain ===");
    Serial.println("  drain");
    Serial.println("      Drain and print pending bytes from the mount serial input.");
  } else if (topic == "pos" || topic == "state" || topic == "current_state" || topic == "currentstate") {
    Serial.println();
    Serial.println("=== Current State Command ===");
    Serial.println("  current_state | currentstate | state | pos");
    Serial.println("      Show cached telescope coordinates and current bridge state.");
  } else if (topic == "health" || topic == "system_health" || topic == "systemhealth") {
    Serial.println();
    Serial.println("=== System Health Command ===");
    Serial.println("  system_health | systemhealth | health");
    Serial.println("      Show counters, loop and poll latency, memory, uptime, and errors.");
  } else if (topic == "tasks") {
    Serial.println();
    Serial.println("=== tasks ===");
    Serial.println("  tasks");
    Serial.println("      Show FreeRTOS cumulative per-task runtime stats if enabled in this build.");
    Serial.println("      Serial is one-shot. Telnet can refresh in place: tasks [seconds|millisecondsms].");
    Serial.println("      Fields: Task=name, Abs Time=cumulative CPU ticks, % Time=CPU share since boot.");
    Serial.println("      Telnet CPU Load is interval load from IDLE0/IDLE1 deltas; first sample shows --%.");
    Serial.println("      Common tasks:");
    Serial.println("        IDLE0/IDLE1 idle tasks for core 0/1; high values mean spare CPU.");
    Serial.println("        tiT TCP/IP stack; Tmr Svc FreeRTOS software timers.");
    Serial.println("        ipc0/ipc1 inter-core calls; esp_timer high-resolution timer callbacks.");
    Serial.println("        wifi WiFi driver; sys_evt ESP system events; arduino_events Arduino events.");
  } else if (topic == "gpio" || topic == "gpio_startup" || topic == "gpiostartup" || topic == "startup_pins") {
    Serial.println();
    Serial.println("=== Startup GPIO Command ===");
    Serial.println("  gpio_startup | gpiostartup | startup_pins");
    Serial.println("      Show startup mode-pin readings and selected boot-mode source.");
  } else if (topic == "telnet") {
    Serial.println();
    Serial.println("=== Telnet Status Command ===");
    Serial.println("  telnet | telnet status");
    Serial.println("      Show Telnet port, connection, authentication, and command counters.");
  } else if (topic == "telnetlog") {
    Serial.println();
    Serial.println("=== telnetlog ===");
    Serial.println("  telnetlog");
    Serial.println("      Show whether log lines are streamed to the Telnet terminal.");
    Serial.println("  telnetlog 0");
    Serial.println("      Turn off log streaming to the Telnet terminal.");
    Serial.println("  telnetlog 1");
    Serial.println("      Turn on log streaming to the Telnet terminal.");
  } else if (topic == "idlepoll") {
    Serial.println();
    Serial.println("=== idlepoll ===");
    Serial.println("  idlepoll | poll idle");
    Serial.println("      Show idle poll interval and effective poll interval.");
    Serial.println("  idlepoll <ms> | poll idle <ms>");
    Serial.println("      Set and save idle poll interval from 0 through 60000 ms.");
  } else if (topic == "setsta" || topic == "sta") {
    Serial.println();
    Serial.println("=== setsta ===");
    Serial.println("  setsta <ssid> <password>");
    Serial.println("      Save STA credentials and immediately attempt a connection.");
    Serial.println("      Spaces in SSID or password are not supported by this command.");
  } else if (topic == "apdefault" || topic == "ap") {
    Serial.println();
    Serial.println("=== apdefault ===");
    Serial.println("  apdefault");
    Serial.println("      Restore AP SSID, password, and IP defaults, then restart the AP.");
  } else {
    Serial.print("Unknown help topic: ");
    Serial.println(topicRaw);
    Serial.println("Grouped topics: mount, mode, wifi");
    Serial.println("Specific topics: log, web, reboot, testinit, get, getaltaz, rates, mountpoll, nudge,");
    Serial.println("                 idlepoll, drain, pos, health, tasks, gpio, telnet,");
    Serial.println("                 telnetlog, setsta, apdefault");
  }
}

void printHelp() {
  Serial.println();
  Serial.printf("%s %s commands\n", FW_NAME, FW_VERSION);
  Serial.println("Help:");
  Serial.println("  help | ? | help mount | help mode | help wifi | help <command>");
  Serial.println("Status:");
  Serial.println("  status");
  Serial.println("  current_state | currentstate | state | pos");
  Serial.println("  system_health | systemhealth | health");
  Serial.println("  tasks");
  Serial.println("  gpio_startup | gpiostartup | startup_pins");
  Serial.println("  telnet | telnet status");
  Serial.println("  telnetlog");
  Serial.println("Mount:");
  Serial.println("  testinit | get | getaltaz | rates | drain");
  Serial.println("  mountpoll | mountpoll <ms>");
  Serial.println("  idlepoll | idlepoll <ms> | poll idle <ms>");
  Serial.println("  nudge az+ | nudge az- | nudge alt+ | nudge alt-");
  Serial.println("Mode:");
  Serial.println("  modebt | modebt_web");
  Serial.println("  modebt_telnet | modebt_noweb | modebt_telnetonly");
  Serial.println("  modewifi");
  Serial.println("  web [status|on|off|toggle]");
  Serial.println("  webserver [status|on|off|toggle]");
  Serial.println("  wifi [status|on|off|toggle]");
  Serial.println("  reboot | restart");
  Serial.println("Config:");
  Serial.println("  setsta <ssid> <password>");
  Serial.println("  apdefault");
  Serial.println("  telnetlog 0 | telnetlog 1");
  Serial.println("  log | log <0..5> [systems]");
}


void printConsoleStatus() {
  Serial.println();
  Serial.printf("Firmware: %s %s\n", FW_NAME, FW_VERSION);
  Serial.printf("Bridge mode: %s\n", bridgeModeName());
  Serial.printf("Startup mode source: %s\n", startupModeSource.c_str());
  Serial.printf("Startup mode pin used: %d\n", startupModePinUsed);
  Serial.printf("WiFi mode: %s\n", wifiModeText.c_str());
  Serial.printf("WiFi radio active: %d\n", bridgeMode == BRIDGE_MODE_WIFI_FULL ? 1 : 0);
  Serial.printf("WiFi status: %s\n", lastWifiStatus.c_str());
  Serial.printf("AP SSID: %s\n", apSsid.c_str());
  Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("STA configured: %d\n", staConfigured ? 1 : 0);
  Serial.printf("STA SSID: %s\n", staSsid.c_str());
  Serial.printf("STA connected: %d\n", (staConnected && WiFi.status() == WL_CONNECTED) ? 1 : 0);
  Serial.printf("STA IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("Last STA IP: %s\n", lastStaIp.c_str());
  Serial.printf("BT tiny web boot option: %s\n", btLiteBootWebEnabled ? "enabled" : "disabled / Telnet-only");
  Serial.printf("BT tiny web server runtime: %s\n", tinyWebServerRuntimeEnabled ? "enabled" : "disabled");
  Serial.printf("Active poll interval: %lu ms\n", pollIntervalMs);
  Serial.printf("Idle poll interval: %lu ms\n", idlePollIntervalMs);
  Serial.printf("Effective poll interval: %lu ms (%s)\n",
                effectiveMountPollIntervalMs(),
                positionDemandActive() ? "active demand" : "idle");
  Serial.printf("WiFi runtime: %s\n", wifiRuntimeEnabled ? "enabled" : "disabled");
  Serial.printf("Telnet console: port=%u password=%s client=%s auth=%s commands=%lu authFailures=%lu\n",
                TELNET_PORT,
                telnetMaskedPassword().c_str(),
                (telnetClient && telnetClient.connected()) ? "connected" : "not connected",
                telnetAuthenticated ? "yes" : "no",
                (unsigned long)telnetRxCommands,
                (unsigned long)telnetAuthFailures);
  Serial.printf("Telnet live logs: %s lines=%lu\n", telnetLiveLogEnabled ? "enabled" : "disabled", (unsigned long)telnetLiveLogLines);
  Serial.printf("GOTO queue: active=%s type=%s timeout=%lu ms effective=%lu ms accepted=%lu started=%lu timedOut=%lu replaced=%lu immediateAcks=%lu cachedPositionReplies=%lu\n",
                hasQueuedGoto() ? "yes" : "no",
                queuedGotoTypeName(queuedGotoType),
                gotoQueueTimeoutMs,
                effectiveGotoQueueTimeoutMs(),
                (unsigned long)gotoQueueAccepted,
                (unsigned long)gotoQueueStarted,
                (unsigned long)gotoQueueTimedOut,
                (unsigned long)gotoQueueReplaced,
                (unsigned long)gotoQueueImmediateAcks,
                (unsigned long)queuedGotoPositionCacheReplies);
  Serial.printf("LX200 UI GOTO: active=%s source=%s started=%lu completed=%lu stopRequests=%lu age=%lu ms\n",
                lx200GotoUiActive ? "yes" : "no",
                lx200SourceName(lx200GotoUiSource),
                (unsigned long)lx200GotoUiStartedCount,
                (unsigned long)lx200GotoUiCompletedCount,
                (unsigned long)lx200GotoUiStopRequests,
                lx200GotoUiActive ? (unsigned long)(millis() - lx200GotoUiStartedMs) : 0UL);
  Serial.printf("Mount poll active=%lu ms idle=%lu ms effective=%lu ms demand=%s lastPollAge=%lu ms schedulerLatency=%lu/%lu ms started=%lu deferredBusy=%lu missedDeadline=%lu\n",
                pollIntervalMs,
                idlePollIntervalMs,
                effectiveMountPollIntervalMs(),
                positionDemandActive() ? "active" : "idle",
                lastMountPollMs == 0 ? 0UL : (unsigned long)(millis() - lastMountPollMs),
                lastPollSchedulerLatencyMs,
                maxPollSchedulerLatencyMs,
                mountPollsStarted,
                mountPollsDeferredBusy,
                mountPollsMissedDeadline);
  Serial.printf("Loop latency current/max=%lu/%lu ms\n", lastLoopLatencyMs, maxLoopLatencyMs);
  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
#if defined(ESP32)
  Serial.printf("Min free heap: %u bytes\n", ESP.getMinFreeHeap());
#endif
#if HAS_CLASSIC_BT
  Serial.printf("Bluetooth enabled: %d\n", bluetoothRuntimeEnabled ? 1 : 0);
  Serial.printf("Bluetooth client: %d\n", SerialBT.hasClient() ? 1 : 0);
#else
  Serial.println("Bluetooth enabled: 0");
#endif
  Serial.printf("Mount fault: %d busy: %d\n", mountCommFault ? 1 : 0, mountBusy ? 1 : 0);
  Serial.println();
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

String telnetMaskedPassword() {
  if (telnetPassword.length() == 0) return String("(disabled)");
  return String("(set, ") + String(telnetPassword.length()) + " chars)";
}

void telnetPrintHelp(Print &out) {
  out.println();
  out.printf("%s %s commands\n", FW_NAME, FW_VERSION);
  out.println("Help:");
  out.println("  help | ? | help mount | help mode | help wifi | help <command>");
  out.println("Status:");
  out.println("  status");
  out.println("  current_state | currentstate | state | pos");
  out.println("  system_health | systemhealth | health");
  out.println("  tasks [seconds|millisecondsms]");
  out.println("  gpio_startup | gpiostartup | startup_pins");
  out.println("  telnet | telnet status");
  out.println("  telnetlog");
  out.println("  monitor [seconds|millisecondsms]");
  out.println("Mount:");
  out.println("  testinit | get | getaltaz | rates | drain");
  out.println("  mountpoll | mountpoll <ms>");
  out.println("  idlepoll | idlepoll <ms> | poll idle <ms>");
  out.println("  nudge az+ | nudge az- | nudge alt+ | nudge alt-");
  out.println("Mode:");
  out.println("  modebt | modebt_web");
  out.println("  modebt_telnet | modebt_noweb | modebt_telnetonly");
  out.println("  modewifi");
  out.println("  web [status|on|off|toggle]");
  out.println("  webserver [status|on|off|toggle]");
  out.println("  wifi [status|on|off|toggle]");
  out.println("  reboot | restart | exit | quit");
  out.println("Config:");
  out.println("  apdefault");
  out.println("  setsta <ssid> <password>");
  out.println("  telnetlog 0 | telnetlog 1");
  out.println("  log | log 0..5 [systems]");
}

void telnetPrintStatus(Print &out) {
  out.println();
  out.println("=== Status ===");
  out.printf("Firmware: %s %s\n", FW_NAME, FW_VERSION);
  out.printf("Bridge mode: %s\n", bridgeModeName());
  out.printf("Startup mode source: %s\n", startupModeSource.c_str());
  out.printf("Startup mode pin used: %d\n", startupModePinUsed);
  out.printf("WiFi mode: %s\n", wifiModeText.c_str());
  out.printf("WiFi status: %s\n", lastWifiStatus.c_str());
  out.printf("AP SSID: %s\n", runtimeApSsid().c_str());
  out.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  out.printf("STA SSID: %s\n", staSsid.c_str());
  out.printf("STA IP: %s\n", WiFi.localIP().toString().c_str());
  out.printf("Telnet port: %u\n", TELNET_PORT);
  out.printf("Telnet password: %s\n", telnetMaskedPassword().c_str());
  out.printf("Telnet authenticated: %s\n", telnetAuthenticated ? "yes" : "no");
  out.printf("Telnet live logs: %s lines=%lu\n", telnetLiveLogEnabled ? "enabled" : "disabled", (unsigned long)telnetLiveLogLines);
  out.printf("GOTO queue: active=%s type=%s timeout=%lu ms effective=%lu ms accepted=%lu started=%lu timedOut=%lu replaced=%lu immediateAcks=%lu cachedPositionReplies=%lu\n",
             hasQueuedGoto() ? "yes" : "no",
             queuedGotoTypeName(queuedGotoType),
             gotoQueueTimeoutMs,
             effectiveGotoQueueTimeoutMs(),
             (unsigned long)gotoQueueAccepted,
             (unsigned long)gotoQueueStarted,
             (unsigned long)gotoQueueTimedOut,
             (unsigned long)gotoQueueReplaced,
             (unsigned long)gotoQueueImmediateAcks,
             (unsigned long)queuedGotoPositionCacheReplies);
  out.printf("Mount poll active=%lu ms idle=%lu ms effective=%lu ms demand=%s lastPollAge=%lu ms schedulerLatency=%lu/%lu ms started=%lu deferredBusy=%lu missedDeadline=%lu\n",
             pollIntervalMs,
             idlePollIntervalMs,
             effectiveMountPollIntervalMs(),
             positionDemandActive() ? "active" : "idle",
             lastMountPollMs == 0 ? 0UL : (unsigned long)(millis() - lastMountPollMs),
             lastPollSchedulerLatencyMs,
             maxPollSchedulerLatencyMs,
             mountPollsStarted,
             mountPollsDeferredBusy,
             mountPollsMissedDeadline);
  out.printf("Loop latency current/max=%lu/%lu ms\n", lastLoopLatencyMs, maxLoopLatencyMs);
  out.printf("BT tiny web boot option: %s\n", btLiteBootWebEnabled ? "enabled" : "disabled / Telnet-only");
  out.printf("BT tiny web server runtime: %s\n", tinyWebServerRuntimeEnabled ? "enabled" : "disabled");
  out.printf("WiFi runtime: %s\n", wifiRuntimeEnabled ? "enabled" : "disabled");
  out.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
#if defined(ESP32)
  out.printf("Min free heap: %u bytes\n", ESP.getMinFreeHeap());
#endif
#if HAS_CLASSIC_BT
  out.printf("Bluetooth runtime: %s\n", bluetoothRuntimeEnabled ? "enabled" : "disabled");
  out.printf("Bluetooth client: %s\n", (bluetoothRuntimeEnabled && SerialBT.hasClient()) ? "connected" : "not connected");
#endif
  out.printf("Mount alive: %s\n", mountAlive() ? "yes" : "unknown/no recent response");
  out.printf("Mount fault: %s\n", mountCommFault ? "yes" : "no");
  if (lastMountFault.length()) out.printf("Last mount fault: %s\n", lastMountFault.c_str());
  out.println("=== End Status ===");
}

void printTaskStatsHeader(Print &out) {
  out.printf("%-16s%-14s%s\n", "Task", "Abs Time", "% Time");
}

bool parseTaskStatsLine(const char *line, char *taskName, size_t taskNameSize, unsigned long &absTime, char *percentTime, size_t percentSize) {
  if (!line || !*line || taskNameSize == 0 || percentSize == 0) return false;
  taskName[0] = '\0';
  percentTime[0] = '\0';
  absTime = 0;

  const char *nameStart = line;
  while (*nameStart == ' ' || *nameStart == '\t') nameStart++;
  const char *tab = strchr(nameStart, '\t');
  if (tab) {
    const char *nameEnd = tab;
    while (nameEnd > nameStart && (*(nameEnd - 1) == ' ' || *(nameEnd - 1) == '\t')) nameEnd--;
    size_t nameLen = (size_t)(nameEnd - nameStart);
    if (nameLen >= taskNameSize) nameLen = taskNameSize - 1;
    memcpy(taskName, nameStart, nameLen);
    taskName[nameLen] = '\0';

    const char *p = tab + 1;
    while (*p == ' ' || *p == '\t') p++;
    char *endPtr = nullptr;
    absTime = strtoul(p, &endPtr, 10);
    if (endPtr == p) return false;
    p = endPtr;
    while (*p == ' ' || *p == '\t') p++;
    size_t pctLen = 0;
    while (p[pctLen] && p[pctLen] != ' ' && p[pctLen] != '\t' && p[pctLen] != '\r' && p[pctLen] != '\n') pctLen++;
    if (pctLen >= percentSize) pctLen = percentSize - 1;
    memcpy(percentTime, p, pctLen);
    percentTime[pctLen] = '\0';
    return taskName[0] && percentTime[0];
  }

  return sscanf(line, " %23s %lu %15s", taskName, &absTime, percentTime) == 3;
}

bool taskStatsIncludeInBasic(const char *taskName) {
  return strcmp(taskName, "loopTask") == 0 ||
         strcmp(taskName, "IDLE0") == 0 ||
         strcmp(taskName, "IDLE1") == 0;
}

void appendTaskStatsRow(String &s, const char *taskName, unsigned long absTime, const char *percentTime) {
  char row[72];
  snprintf(row, sizeof(row), "%-16s%-14lu%s\n", taskName, absTime, percentTime);
  s += row;
}

String taskStatsSectionText(bool basicMode) {
  String s;
  s += basicMode ? "TASKS\n" : "=== TASKS ===\n";
  s += "Task            Abs Time      % Time\n";
#if defined(ESP32) && defined(configGENERATE_RUN_TIME_STATS) && (configGENERATE_RUN_TIME_STATS == 1) && defined(configUSE_STATS_FORMATTING_FUNCTIONS) && (configUSE_STATS_FORMATTING_FUNCTIONS == 1)
  static char taskStats[2048];
  memset(taskStats, 0, sizeof(taskStats));
  vTaskGetRunTimeStats(taskStats);

  bool printedAny = false;
  char *line = taskStats;
  while (line && *line) {
    char *next = line;
    while (*next && *next != '\n' && *next != '\r') next++;
    char saved = *next;
    *next = '\0';

    char taskName[24] = "";
    char percentTime[16] = "";
    unsigned long absTime = 0;
    if (parseTaskStatsLine(line, taskName, sizeof(taskName), absTime, percentTime, sizeof(percentTime)) &&
        (!basicMode || taskStatsIncludeInBasic(taskName))) {
      appendTaskStatsRow(s, taskName, absTime, percentTime);
      printedAny = true;
    }

    if (!saved) break;
    line = next + 1;
    while (*line == '\n' || *line == '\r') line++;
  }
  if (!printedAny) s += basicMode ? "loopTask/IDLE0/IDLE1 unavailable\n" : "No task rows available\n";
#else
  s += "FreeRTOS runtime stats are not enabled in this build.\n";
#endif
  return s;
}

void printTaskStatsLine(Print &out, const char *line) {
  char taskName[24] = "";
  unsigned long absTime = 0;
  char percentTime[16] = "";
  if (parseTaskStatsLine(line, taskName, sizeof(taskName), absTime, percentTime, sizeof(percentTime))) {
    out.printf("%-16s%-14lu%s\n", taskName, absTime, percentTime);
  } else if (line && *line) {
    out.println(line);
  }
}

void updateTelnetTasksCpuLoad(char *taskStats) {
  unsigned long idle0 = 0;
  unsigned long idle1 = 0;
  unsigned long total = 0;
  bool foundIdle0 = false;
  bool foundIdle1 = false;
  const char *p = taskStats;
  while (p && *p) {
    char line[128] = "";
    size_t len = 0;
    while (p[len] && p[len] != '\n' && p[len] != '\r' && len < sizeof(line) - 1) len++;
    memcpy(line, p, len);
    line[len] = '\0';

    char taskName[24] = "";
    char percentTime[16] = "";
    unsigned long absTime = 0;
    if (parseTaskStatsLine(line, taskName, sizeof(taskName), absTime, percentTime, sizeof(percentTime))) {
      total += absTime;
      if (strcmp(taskName, "IDLE0") == 0) {
        idle0 = absTime;
        foundIdle0 = true;
      } else if (strcmp(taskName, "IDLE1") == 0) {
        idle1 = absTime;
        foundIdle1 = true;
      }
    }

    p += len;
    while (*p == '\n' || *p == '\r') p++;
  }

  if (foundIdle0 && foundIdle1 && telnetTasksLastLoadMs > 0 &&
      idle0 >= telnetTasksLastIdle0 && idle1 >= telnetTasksLastIdle1 && total > telnetTasksLastRuntimeTotal) {
    unsigned long idleDelta = (idle0 - telnetTasksLastIdle0) + (idle1 - telnetTasksLastIdle1);
    unsigned long totalDelta = total - telnetTasksLastRuntimeTotal;
    int loadPct = 100 - (int)((idleDelta * 100UL) / totalDelta);
    if (loadPct < 0) loadPct = 0;
    if (loadPct > 100) loadPct = 100;
    telnetTasksCpuLoadPct = loadPct;
  } else {
    telnetTasksCpuLoadPct = -1;
  }

  if (foundIdle0 && foundIdle1) {
    telnetTasksLastIdle0 = idle0;
    telnetTasksLastIdle1 = idle1;
    telnetTasksLastRuntimeTotal = total;
    telnetTasksLastLoadMs = millis();
  }
}

String sampleWebCpuLoadText() {
#if defined(ESP32) && defined(configGENERATE_RUN_TIME_STATS) && (configGENERATE_RUN_TIME_STATS == 1) && defined(configUSE_STATS_FORMATTING_FUNCTIONS) && (configUSE_STATS_FORMATTING_FUNCTIONS == 1)
  static char taskStats[2048];
  memset(taskStats, 0, sizeof(taskStats));
  vTaskGetRunTimeStats(taskStats);

  unsigned long idle0 = 0;
  unsigned long idle1 = 0;
  unsigned long total = 0;
  bool foundIdle0 = false;
  bool foundIdle1 = false;
  char *line = taskStats;
  while (line && *line) {
    char *next = line;
    while (*next && *next != '\n' && *next != '\r') next++;
    char saved = *next;
    *next = '\0';

    char taskName[24] = "";
    char percentTime[16] = "";
    unsigned long absTime = 0;
    if (parseTaskStatsLine(line, taskName, sizeof(taskName), absTime, percentTime, sizeof(percentTime))) {
      total += absTime;
      if (strcmp(taskName, "IDLE0") == 0) {
        idle0 = absTime;
        foundIdle0 = true;
      } else if (strcmp(taskName, "IDLE1") == 0) {
        idle1 = absTime;
        foundIdle1 = true;
      }
    }

    if (!saved) break;
    line = next + 1;
    while (*line == '\n' || *line == '\r') line++;
  }

  if (foundIdle0 && foundIdle1 && webCpuLastRuntimeTotal > 0 &&
      idle0 >= webCpuLastIdle0 && idle1 >= webCpuLastIdle1 && total > webCpuLastRuntimeTotal) {
    unsigned long idleDelta = (idle0 - webCpuLastIdle0) + (idle1 - webCpuLastIdle1);
    unsigned long totalDelta = total - webCpuLastRuntimeTotal;
    int loadPct = 100 - (int)((idleDelta * 100UL) / totalDelta);
    if (loadPct < 0) loadPct = 0;
    if (loadPct > 100) loadPct = 100;
    webCpuLoadPct = loadPct;
  } else {
    webCpuLoadPct = -1;
  }

  if (foundIdle0 && foundIdle1) {
    webCpuLastIdle0 = idle0;
    webCpuLastIdle1 = idle1;
    webCpuLastRuntimeTotal = total;
  }
#endif
  if (webCpuLoadPct < 0) return "CPU Load --%";
  return String("CPU Load ") + String(webCpuLoadPct) + "%";
}

String taskCpuLoadText() {
  if (telnetTasksCpuLoadPct < 0) return "CPU Load --%";
  return String("CPU Load ") + String(telnetTasksCpuLoadPct) + "%";
}

String taskRefreshText();

void printTasksBanner(Print &out) {
  String left = String(FW_VERSION) + " Tasks  Up " + formatUptime() + "  Refresh " + taskRefreshText();
  out.println(left);
}

void printFormattedTaskRuntimeStats(Print &out, char *taskStats) {
  printTaskStatsHeader(out);
  char *line = taskStats;
  while (line && *line) {
    char *next = line;
    while (*next && *next != '\n' && *next != '\r') next++;
    char saved = *next;
    *next = '\0';
    printTaskStatsLine(out, line);
    if (!saved) break;
    line = next + 1;
    while (*line == '\n' || *line == '\r') line++;
  }
}

void printTaskRuntimeStats(Print &out) {
#if defined(ESP32) && defined(configGENERATE_RUN_TIME_STATS) && (configGENERATE_RUN_TIME_STATS == 1) && defined(configUSE_STATS_FORMATTING_FUNCTIONS) && (configUSE_STATS_FORMATTING_FUNCTIONS == 1)
  static char taskStats[2048];
  memset(taskStats, 0, sizeof(taskStats));
  vTaskGetRunTimeStats(taskStats);
  out.println("=== FreeRTOS Runtime Stats ===");
  out.println("Task runtime percentages are cumulative since boot.");
  printFormattedTaskRuntimeStats(out, taskStats);
  out.println("=== End Runtime Stats ===");
#else
  out.println("FreeRTOS runtime stats are not enabled in this build.");
  out.println("Required: configGENERATE_RUN_TIME_STATS=1 and configUSE_STATS_FORMATTING_FUNCTIONS=1.");
  out.println("Command is compiled as a safe no-op unless those options are available.");
#endif
}

String taskRefreshText() {
  if (telnetTasksRefreshMs % 1000UL == 0) return String(telnetTasksRefreshMs / 1000UL) + "s";
  return String(telnetTasksRefreshMs) + "ms";
}

bool consoleConfirmYes(const String &cmd) {
  return cmd == "y" || cmd == "yes";
}

bool consoleConfirmNo(const String &cmd) {
  return cmd == "n" || cmd == "no";
}

bool commandRequiresDisconnectConfirm(const String &cmd, bool telnetSession, String &description) {
  if (cmd == "reboot" || cmd == "restart") {
    description = "This command will reboot the controller.";
    return true;
  }
  if (cmd == "wifi off") {
    description = "This command will turn WiFi off and may disconnect clients.";
    return true;
  }
  if (cmd == "wifi toggle" && wifiRuntimeEnabled) {
    description = "This command will turn WiFi off and may disconnect clients.";
    return true;
  }
  if (cmd == "modebt" || cmd == "modebt_web" ||
      cmd == "modebt_telnet" || cmd == "modebt_noweb" || cmd == "modebt_telnetonly" ||
      cmd == "modewifi") {
    description = "This command will save boot mode and reboot the controller.";
    return true;
  }
  if (cmd == "apdefault") {
    description = "This command will restart the AP/radio path and may disconnect clients.";
    return true;
  }
  if (cmd.startsWith("setsta ")) {
    description = "This command will change STA WiFi and may disconnect clients.";
    return true;
  }
  if (telnetSession && (cmd == "exit" || cmd == "quit")) {
    description = "This command will close the Telnet session.";
    return true;
  }
  return false;
}

void printCommandConfirmPrompt(Print &out, const String &line, const String &description) {
  out.println(description);
  out.print("Run '");
  out.print(line);
  out.println("'? y/n");
}

void printMountPollValue(Print &out) {
  out.printf("mountpoll=%lu ms%s\n", pollIntervalMs, pollIntervalMs == 0 ? " (off)" : "");
}

bool setMountPollValue(long valueMs) {
  if (valueMs < 0) valueMs = 0;
  if (valueMs > 60000) valueMs = 60000;
  pollIntervalMs = (unsigned long)valueMs;
  nextMountPollDueMs = 0;
  backgroundPollingAutoDisabled = false;
  return savePersistentSettings();
}

String formatAgeSeconds(unsigned long timestampMs) {
  if (timestampMs == 0) return "never";
  unsigned long s = (millis() - timestampMs) / 1000UL;
  if (s < 60UL) return String(s) + "s";
  unsigned long m = s / 60UL;
  s %= 60UL;
  if (m < 60UL) return String(m) + "m" + twoDigits(s) + "s";
  unsigned long h = m / 60UL;
  m %= 60UL;
  return String(h) + "h" + twoDigits(m) + "m";
}

String monitorRefreshText() {
  if (telnetMonitorRefreshMs % 1000UL == 0) return String(telnetMonitorRefreshMs / 1000UL) + "s";
  return String(telnetMonitorRefreshMs) + "ms";
}

unsigned long parseMonitorRefreshMs(const String &cmd) {
  if (cmd == "monitor") return 2000UL;
  if (!cmd.startsWith("monitor ")) return 2000UL;
  String arg = cmd.substring(8);
  arg.trim();
  arg.toLowerCase();
  if (arg.length() == 0) return 2000UL;

  bool milliseconds = arg.endsWith("ms");
  if (milliseconds) arg.remove(arg.length() - 2);
  else if (arg.endsWith("s")) arg.remove(arg.length() - 1);
  arg.trim();

  unsigned long value = (unsigned long)arg.toInt();
  if (value == 0) return 2000UL;
  unsigned long ms = milliseconds ? value : value * 1000UL;
  if (ms < 500UL) ms = 500UL;
  if (ms > 60000UL) ms = 60000UL;
  return ms;
}

unsigned long parseIntervalArgumentMs(const String &cmd, const String &commandName, unsigned long defaultMs) {
  if (cmd == commandName) return defaultMs;
  String prefix = commandName;
  prefix += " ";
  if (!cmd.startsWith(prefix)) return defaultMs;
  String arg = cmd.substring(prefix.length());
  arg.trim();
  arg.toLowerCase();
  if (arg.length() == 0) return defaultMs;

  bool milliseconds = arg.endsWith("ms");
  if (milliseconds) arg.remove(arg.length() - 2);
  else if (arg.endsWith("s")) arg.remove(arg.length() - 1);
  arg.trim();

  unsigned long value = (unsigned long)arg.toInt();
  if (value == 0) return defaultMs;
  unsigned long ms = milliseconds ? value : value * 1000UL;
  if (ms < 1000UL) ms = 1000UL;
  if (ms > 60000UL) ms = 60000UL;
  return ms;
}

void telnetDrawMonitor(Print &out) {
  bool btConnected = false;
#if HAS_CLASSIC_BT
  btConnected = bluetoothRuntimeEnabled && SerialBT.hasClient();
#endif
  bool skyWifiConnected = lx200Client && lx200Client.connected();
  bool stelConnected = stellariumClient && stellariumClient.connected();
  const char* mountStateShort = mountCommFault ? "Fault" : ((mountBusy || asyncSlewRunning || asyncSlewPending) ? "Busy" : (lastMountResponseMs > 0 ? "Connected" : "Unknown"));

  out.print("\x1B[H");
  out.printf("%-20s %-10s %-8s %-12s %-8s %-8s\n",
             FW_NAME,
             FW_VERSION,
             "Uptime",
             formatUptime().c_str(),
             "Refresh",
             monitorRefreshText().c_str());
  out.println("q / Enter / Ctrl-C exits");
  out.println("----------------------------------------------------------------------------");
  out.printf("%-12s %-12s %-12s %-12s %-12s %-12s\n", "MOUNT", "FAULT", "MOUNT UP", "LAST POLL", "TX", "RX");
  out.printf("%-12s %-12s %-12s %-12s %-12lu %-12lu\n",
             mountStateShort,
             mountCommFault ? "yes" : "no",
             formatMountUptime().c_str(),
             lastSuccessfulMountPollMs == 0 ? "none" : (twoDigits(lastSuccessfulMountPollHour) + ":" + twoDigits(lastSuccessfulMountPollMinute) + ":" + twoDigits(lastSuccessfulMountPollSecond)).c_str(),
             mountPollsStarted,
             mountPollsSucceeded);
  out.println();
  out.printf("%-12s %-12s %-12s %-12s %-12s\n", "ALT", "AZ", "MOUNTPOLL", "IDLEPOLL", "CURRENT POLL");
  out.printf("%-12.6f %-12.6f %-12s %-12s %-12s\n",
             mountCurrentAlt_deg,
             mountCurrentAz_deg,
             (String(pollIntervalMs) + " ms").c_str(),
             (String(idlePollIntervalMs) + " ms").c_str(),
             (String(effectiveMountPollIntervalMs()) + " ms").c_str());
  out.println();
  out.printf("%-24s %-24s %-20s\n", "POLLS STARTED", "POLLS MISSED", "LOGS");
  out.printf("%-24lu %-24lu %-20s\n",
             mountPollsStarted,
             mountPollsMissedDeadline,
             telnetMonitorActive ? (telnetMonitorSavedLiveLog ? "paused/on" : "paused/off") : (telnetLiveLogEnabled ? "on" : "off"));
  out.println();
  out.printf("%-12s %-12s %-12s %-12s %-12s\n", "CLIENT", "STATE", "TX", "RX", "ERRORS");
  out.printf("%-12s %-12s %-12lu %-12lu %-12lu\n",
             "Sky WiFi",
             skyWifiConnected ? "connected" : "idle",
             lx200WifiTxReplies,
             lx200WifiRxCommands,
             lx200WifiUnhandledCommands);
  out.printf("%-12s %-12s %-12lu %-12lu %-12lu\n",
             "Sky BT",
             btConnected ? "connected" : (bluetoothRuntimeEnabled ? "waiting" : "disabled"),
             lx200BtTxReplies,
             lx200BtRxCommands,
             lx200BtUnhandledCommands);
  out.printf("%-12s %-12s %-12lu %-12lu %-12lu\n",
             "Stellarium",
             stelConnected ? "connected" : "idle",
             stellariumTxPackets,
             stellariumRxPackets,
             stellariumBadPackets);
  out.printf("%-12s %-12s %-12s %-12lu %-12s\n",
             "Alpaca",
             alpacaConnected ? "active" : "idle",
             "-",
             alpacaHttpRequests,
             "-");
  out.println();
  out.printf("%-12s %-12s %-12s %-12s\n", "HEAP", "MIN HEAP", "LOOP", "MAX LOOP");
  out.printf("%-12u %-12u %-12s %-12s\n",
             ESP.getFreeHeap(),
#if defined(ESP32)
             ESP.getMinFreeHeap(),
#else
             0,
#endif
             (String(lastLoopLatencyMs) + " ms").c_str(),
             (String(maxLoopLatencyMs) + " ms").c_str());
  out.println();
  out.println("FAULT DETAIL");
  out.printf("%.76s\n", lastMountFault.length() ? lastMountFault.c_str() : "none");
  out.print("\x1B[J");
}

void telnetStartMonitor(Print &out, unsigned long refreshMs) {
  telnetMonitorRefreshMs = refreshMs;
  telnetMonitorSavedLiveLog = telnetLiveLogEnabled;
  telnetLiveLogEnabled = false;
  telnetMonitorActive = true;
  telnetMonitorLastDrawMs = millis();
  out.print("\x1B[2J\x1B[?25l");
  telnetDrawMonitor(out);
}

void telnetStopMonitor(Print &out) {
  telnetMonitorActive = false;
  telnetLiveLogEnabled = telnetMonitorSavedLiveLog;
  out.print("\x1B[?25h");
  out.println();
  out.println("Monitor stopped.");
}

void telnetDrawTasks(Print &out) {
  out.print("\x1B[H");
#if defined(ESP32) && defined(configGENERATE_RUN_TIME_STATS) && (configGENERATE_RUN_TIME_STATS == 1) && defined(configUSE_STATS_FORMATTING_FUNCTIONS) && (configUSE_STATS_FORMATTING_FUNCTIONS == 1)
  static char taskStats[2048];
  memset(taskStats, 0, sizeof(taskStats));
  vTaskGetRunTimeStats(taskStats);
  updateTelnetTasksCpuLoad(taskStats);
  printTasksBanner(out);
  out.println("q / Enter / Ctrl-C exits. Runtime percentages are cumulative since boot.");
  out.println("----------------------------------------------------------------------------");
  out.println(taskCpuLoadText());
  printFormattedTaskRuntimeStats(out, taskStats);
#else
  printTasksBanner(out);
  out.println("q / Enter / Ctrl-C exits. Runtime percentages are cumulative since boot.");
  out.println("----------------------------------------------------------------------------");
  out.println("FreeRTOS runtime stats are not enabled in this build.");
  out.println("Required:");
  out.println("  configGENERATE_RUN_TIME_STATS=1");
  out.println("  configUSE_STATS_FORMATTING_FUNCTIONS=1");
  out.println();
  out.println("The command is present as a safe no-op until those options are available.");
#endif
  out.print("\x1B[J");
}

void telnetStartTasks(Print &out, unsigned long refreshMs) {
  telnetTasksRefreshMs = refreshMs;
  telnetTasksSavedLiveLog = telnetLiveLogEnabled;
  telnetLiveLogEnabled = false;
  telnetMonitorActive = false;
  telnetTasksActive = true;
  telnetTasksLastDrawMs = millis();
  telnetTasksLastIdle0 = 0;
  telnetTasksLastIdle1 = 0;
  telnetTasksLastRuntimeTotal = 0;
  telnetTasksLastLoadMs = 0;
  telnetTasksCpuLoadPct = -1;
  out.print("\x1B[2J\x1B[?25l");
  telnetDrawTasks(out);
}

void telnetStopTasks(Print &out) {
  telnetTasksActive = false;
  telnetLiveLogEnabled = telnetTasksSavedLiveLog;
  out.print("\x1B[?25h");
  out.println();
  out.println("Tasks stopped.");
}

void telnetRunCommand(String line, Print &out) {
  line.trim();
  if (line.length() == 0) return;
  String cmd = line;
  cmd.toLowerCase();
  telnetRxCommands++;
  telnetLastRxMs = millis();
  Serial.print("[telnet] command: ");
  Serial.println(line);

  if (cmd == "help" || cmd == "?") telnetPrintHelp(out);
  else if (cmd.startsWith("help ")) {
    String topic = line.substring(5);
    topic.trim();
    topic.toLowerCase();

    if (topic == "mount" || topic == "scope" || topic == "nexstar") {
      out.println("Mount and polling commands:");
      out.println("  testinit                    Test '?' handshake and E payload read.");
      out.println("  get                         Query current RA/Dec.");
      out.println("  getaltaz                    Query current Alt/Az.");
      out.println("  pos | current_state | state Show cached coordinates and bridge state.");
      out.println("  rates                       Show nudge rates, poll interval, throttle.");
      out.println("  mountpoll                   Get active mount poll interval.");
      out.println("  mountpoll <ms>              Set active mount poll interval. 0 turns it off.");
      out.println("  nudge az+ | az- | alt+ | alt-");
      out.println("                              Perform one relative nudge at rate slot 2.");
      out.println("  drain                       Drain pending mount serial bytes.");
    } else if (topic == "mode" || topic == "boot" || topic == "runtime" || topic == "radio") {
      out.println("Mode and runtime commands:");
      out.println("  modebt | modebt_web         Save BT mode with tiny web enabled; reboot.");
      out.println("  modebt_telnet | modebt_noweb | modebt_telnetonly");
      out.println("                              Save BT Telnet-only mode; reboot.");
      out.println("  modewifi                    Save Full WiFi mode; reboot.");
      out.println("  web [status|on|off|toggle]  Control BT web server for this boot.");
      out.println("  reboot | restart            Restart immediately.");
    } else if (topic == "wifi" || topic == "network") {
      out.println("WiFi configuration commands:");
      out.println("  wifi [status|on|off|toggle] Show or change WiFi runtime state.");
      out.println("  setsta <ssid> <password>    Save STA credentials and connect.");
      out.println("  apdefault                   Restore default AP settings.");
    } else if (topic == "log" || topic == "logging" || topic == "logs") {
      out.println("log");
      out.println("  Show current level and enabled subsystems.");
      out.println("log <0..5> [systems]");
      out.println("  Set level and optional subsystem filter.");
      out.println("Levels:");
      out.println("  0 none   Disable stored/streamed firmware log output.");
      out.println("  1 error  Only failures that usually need action.");
      out.println("  2 warn   Errors plus abnormal/retry/fallback conditions.");
      out.println("  3 info   Normal high-level events: clients, saves, connect/disconnect.");
      out.println("  4 debug  Detailed decisions and state changes for troubleshooting.");
      out.println("  5 trace  Very chatty low-level flow; use briefly, can flood logs.");
    } else if (topic == "web" || topic == "webserver") {
      out.println("web | web status | webserver | webserver status");
      out.println("  Show the BT web-server runtime state.");
      out.println("web on | web off | web toggle");
      out.println("  Change the BT web-server state for this boot only.");
    } else if (topic == "reboot" || topic == "restart") {
      out.println("reboot | restart");
      out.println("  Restart the controller immediately and close Telnet.");
    } else if (topic == "testinit") {
      out.println("testinit"); out.println("  Test the NexStar '?' handshake and E payload read.");
    } else if (topic == "get") {
      out.println("get"); out.println("  Query the mount and print current RA/Dec.");
    } else if (topic == "getaltaz") {
      out.println("getaltaz"); out.println("  Query the mount and print current Alt/Az.");
    } else if (topic == "rates" || topic == "poll" || topic == "mountpoll") {
      out.println("rates"); out.println("  Show nudge rates, poll interval, and client throttle.");
      out.println("mountpoll | mountpoll <ms>");
      out.println("  Get or set the active mount poll interval. 0 turns it off.");
    } else if (topic == "nudge") {
      out.println("nudge az+ | nudge az- | nudge alt+ | nudge alt-");
      out.println("  Perform one relative nudge using rate slot 2.");
    } else if (topic == "drain") {
      out.println("drain"); out.println("  Drain and print pending mount serial bytes.");
    } else if (topic == "pos" || topic == "state" || topic == "current_state" || topic == "currentstate") {
      out.println("current_state | currentstate | state | pos");
      out.println("  Show cached coordinates and current bridge state.");
    } else if (topic == "health" || topic == "system_health" || topic == "systemhealth") {
      out.println("system_health | systemhealth | health");
      out.println("  Show counters, latency, memory, uptime, and errors.");
    } else if (topic == "tasks") {
      out.println("tasks [seconds|millisecondsms]");
      out.println("  Show FreeRTOS cumulative per-task runtime stats in place.");
      out.println("  Default refresh is 5 seconds. Examples: tasks | tasks 10 | tasks 1500ms");
      out.println("  Press q, Enter, or Ctrl-C to exit.");
      out.println("  Fields: Task=name, Abs Time=cumulative CPU ticks, % Time=CPU share since boot.");
      out.println("  CPU Load is interval load from IDLE0/IDLE1 deltas; first sample shows --%.");
      out.println("  IDLE0/IDLE1 idle core 0/1; high values mean spare CPU.");
      out.println("  tiT TCP/IP stack; Tmr Svc software timers; ipc0/ipc1 inter-core calls.");
      out.println("  esp_timer hi-res timers; wifi driver; sys_evt ESP events; arduino_events Arduino events.");
    } else if (topic == "gpio" || topic == "gpio_startup" || topic == "gpiostartup" || topic == "startup_pins") {
      out.println("gpio_startup | gpiostartup | startup_pins");
      out.println("  Show startup mode-pin readings and boot-mode source.");
    } else if (topic == "telnet") {
      out.println("telnet | telnet status");
      out.println("  Show Telnet port, connection, authentication, and counters.");
    } else if (topic == "monitor") {
      out.println("monitor [seconds|millisecondsms]");
      out.println("  Start a live in-place Telnet status view. Default refresh is 2 seconds.");
      out.println("  Examples: monitor | monitor 5 | monitor 750ms");
      out.println("  Press q, Enter, or Ctrl-C to exit.");
    } else if (topic == "telnetlog") {
      out.println("telnetlog");
      out.println("  Show whether log lines are streamed to the Telnet terminal.");
      out.println("telnetlog 0");
      out.println("  Turn off log streaming to the Telnet terminal.");
      out.println("telnetlog 1");
      out.println("  Turn on log streaming to the Telnet terminal.");
    } else if (topic == "idlepoll") {
      out.println("idlepoll | poll idle");
      out.println("  Show idle poll interval and effective poll interval.");
      out.println("idlepoll <ms> | poll idle <ms>");
      out.println("  Set and save idle poll interval from 0 to 60000 ms.");
    } else if (topic == "setsta" || topic == "sta") {
      out.println("setsta <ssid> <password>");
      out.println("  Save STA credentials and connect. Spaces are unsupported.");
    } else if (topic == "apdefault" || topic == "ap") {
      out.println("apdefault");
      out.println("  Restore AP SSID, password, and IP defaults.");
    } else {
      out.print("Unknown help topic: "); out.println(topic);
      out.println("Grouped topics: mount, mode, wifi");
      out.println("Specific topics: log, web, reboot, testinit, get, getaltaz, rates,");
      out.println("mountpoll, nudge, idlepoll, drain, pos, health, tasks, gpio, telnet,");
      out.println("telnetlog, setsta, apdefault");
    }
  }
  else if (cmd == "status") telnetPrintStatus(out);
  else if (cmd == "monitor" || cmd.startsWith("monitor ")) {
    telnetStartMonitor(out, parseMonitorRefreshMs(cmd));
  }
  else if (cmd == "gpio_startup" || cmd == "gpiostartup" || cmd == "startup_pins") {
    out.print(gpioStartupModeText());
  }
  else if (cmd == "current_state" || cmd == "currentstate" || cmd == "state" || cmd == "pos") {
    out.println();
    out.println("=== Current State ===");
    out.print(currentStateText());
    out.println("=== End Current State ===");
  }
  else if (cmd == "system_health" || cmd == "systemhealth" || cmd == "health") {
    out.println();
    out.println("=== System Health ===");
    out.print(systemHealthText());
    out.println("=== End System Health ===");
  }
  else if (cmd == "tasks" || cmd.startsWith("tasks ")) {
    telnetStartTasks(out, parseIntervalArgumentMs(cmd, "tasks", 5000UL));
  }
  else if (cmd == "telnetlog") {
    out.printf("Telnet live logs: %s lines=%lu\n",
               telnetLiveLogEnabled ? "enabled" : "disabled",
               (unsigned long)telnetLiveLogLines);
    out.println("Commands: telnetlog 0 | telnetlog 1");
  }
  else if (cmd == "telnetlog 0") {
    telnetLiveLogEnabled = false;
    out.println("Telnet live logs disabled.");
  }
  else if (cmd == "telnetlog 1") {
    telnetLiveLogEnabled = true;
    out.println("Telnet live logs enabled.");
  }
  else if (cmd == "telnet" || cmd == "telnet status") {
    out.printf("Telnet console: port=%u password=%s client=%s auth=%s commands=%lu authFailures=%lu liveLogs=%s liveLogLines=%lu\n",
               TELNET_PORT,
               telnetMaskedPassword().c_str(),
               (telnetClient && telnetClient.connected()) ? "connected" : "not connected",
               telnetAuthenticated ? "yes" : "no",
               (unsigned long)telnetRxCommands,
               (unsigned long)telnetAuthFailures,
               telnetLiveLogEnabled ? "enabled" : "disabled",
               (unsigned long)telnetLiveLogLines);
  }
  else if (cmd == "web" || cmd == "web status" || cmd == "webserver" || cmd == "webserver status") {
    out.printf("BT tiny web server runtime: %s\n", tinyWebServerRuntimeEnabled ? "enabled" : "disabled");
    out.println("Commands: web on | web off | web toggle");
  }
  else if (cmd == "web on" || cmd == "webserver on") {
    tinyWebServerRuntimeEnabled = true;
    out.println("BT tiny web server runtime enabled.");
  }
  else if (cmd == "web off" || cmd == "webserver off") {
    tinyWebServerRuntimeEnabled = false;
    out.println("BT tiny web server runtime disabled.");
  }
  else if (cmd == "web toggle" || cmd == "webserver toggle") {
    tinyWebServerRuntimeEnabled = !tinyWebServerRuntimeEnabled;
    out.printf("BT tiny web server runtime now: %s\n", tinyWebServerRuntimeEnabled ? "enabled" : "disabled");
  }
  else if (cmd == "wifi" || cmd == "wifi status") {
    out.printf("WiFi runtime: %s\n", wifiRuntimeEnabled ? "enabled" : "disabled");
    out.printf("Tiny web runtime: %s\n", tinyWebServerRuntimeEnabled ? "enabled" : "disabled");
    out.printf("WiFi mode text: %s\n", wifiModeText.c_str());
    out.printf("AP running: %d AP IP: %s STA connected: %d STA IP: %s\n",
               apRunning ? 1 : 0,
               WiFi.softAPIP().toString().c_str(),
               (staConnected && WiFi.status() == WL_CONNECTED) ? 1 : 0,
               WiFi.localIP().toString().c_str());
    out.println("Commands: wifi on | wifi off | wifi toggle");
  }
  else if (cmd == "wifi off") {
    out.println("WARNING: wifi off will shut down WiFi and disconnect this Telnet session.");
    out.println("Use USB serial or reboot to restore WiFi if needed.");
    delay(150);
    runtimeWifiOff();
  }
  else if (cmd == "wifi on") {
    runtimeWifiOn();
    out.println("WiFi runtime restore requested.");
  }
  else if (cmd == "wifi toggle") {
    if (wifiRuntimeEnabled) {
      out.println("WARNING: wifi toggle will turn WiFi off and disconnect this Telnet session.");
      out.println("Use USB serial or reboot to restore WiFi if needed.");
      delay(150);
      runtimeWifiOff();
    } else {
      runtimeWifiOn();
      out.println("WiFi runtime restore requested.");
    }
  }
  else if (cmd == "log") {
    out.printf("Log level: %d (%s), mask=0x%04X, systems=%s\n", LOG_LEVEL, logLevelName(LOG_LEVEL), LOG_SUBSYSTEM_MASK, consoleLogMaskNames(LOG_SUBSYSTEM_MASK).c_str());
  }
  else if (cmd.startsWith("log ")) {
    String arg = line.substring(4);
    arg.trim();
    int sp = arg.indexOf(' ');
    String lvlText = (sp >= 0) ? arg.substring(0, sp) : arg;
    lvlText.trim();
    int lvl = lvlText.toInt();
    if (lvl < 0) lvl = 0;
    if (lvl > 5) lvl = 5;
    LOG_LEVEL = lvl;
    if (sp >= 0) {
      String cats = arg.substring(sp + 1);
      cats.trim();
      if (cats.length()) LOG_SUBSYSTEM_MASK = consoleLogMaskFromList(cats);
    }
    out.printf("Log level applied: %d (%s), mask=0x%04X\n", LOG_LEVEL, logLevelName(LOG_LEVEL), LOG_SUBSYSTEM_MASK);
  }
  else if (cmd == "testinit") {
    bool ok = testInit();
    out.printf("testinit result: %s\n", ok ? "OK" : "FAILED");
  }
  else if (cmd == "get") {
    double ra = 0.0, dec = 0.0;
    bool ok = getRaDec(ra, dec, true);
    out.printf("get result: %s RA=%.6f DEC=%.6f\n", ok ? "OK" : "FAILED", ra, dec);
  }
  else if (cmd == "getaltaz") {
    double alt = 0.0, az = 0.0;
    bool ok = getAltAz(alt, az, true);
    out.printf("getaltaz result: %s ALT=%.6f AZ=%.6f\n", ok ? "OK" : "FAILED", alt, az);
  }
  else if (cmd == "rates") {
    out.printf("Nudge rates: %.3f %.3f %.3f %.3f poll=%lu idlePoll=%lu effectivePoll=%lu demand=%s throttle=%lu\n",
               nudgeRateDeg[0], nudgeRateDeg[1], nudgeRateDeg[2], nudgeRateDeg[3],
               pollIntervalMs, idlePollIntervalMs, effectiveMountPollIntervalMs(),
               positionDemandActive() ? "active" : "idle", minClientPollIntervalMs);
  }
  else if (cmd == "mountpoll") {
    printMountPollValue(out);
  }
  else if (cmd.startsWith("mountpoll ")) {
    long p = line.substring(10).toInt();
    bool ok = setMountPollValue(p);
    out.printf("mountpoll set to %lu ms%s\n", pollIntervalMs, ok ? "" : " (save failed)");
  }
  else if (cmd == "idlepoll" || cmd == "poll idle") {
    out.printf("Idle poll interval: %lu ms\n", idlePollIntervalMs);
    out.printf("Effective poll interval: %lu ms (%s)\n",
               effectiveMountPollIntervalMs(),
               positionDemandActive() ? "active demand" : "idle");
  }
  else if (cmd.startsWith("idlepoll ") || cmd.startsWith("poll idle ")) {
    String arg = cmd.startsWith("idlepoll ") ? line.substring(9) : line.substring(10);
    arg.trim();
    long p = arg.toInt();
    if (p < 0) p = 0;
    if (p > 60000) p = 60000;
    idlePollIntervalMs = (unsigned long)p;
    nextMountPollDueMs = 0;
    bool ok = savePersistentSettings();
    out.printf("Idle poll interval set to %lu ms saved=%d\n", idlePollIntervalMs, ok ? 1 : 0);
  }
  else if (cmd == "nudge az+") out.printf("nudge az+ result: %s\n", nudgeRelativeAltAz(0.0, nudgeRateDeg[1]) ? "OK" : "FAILED");
  else if (cmd == "nudge az-") out.printf("nudge az- result: %s\n", nudgeRelativeAltAz(0.0, -nudgeRateDeg[1]) ? "OK" : "FAILED");
  else if (cmd == "nudge alt+") out.printf("nudge alt+ result: %s\n", nudgeRelativeAltAz(nudgeRateDeg[1], 0.0) ? "OK" : "FAILED");
  else if (cmd == "nudge alt-") out.printf("nudge alt- result: %s\n", nudgeRelativeAltAz(-nudgeRateDeg[1], 0.0) ? "OK" : "FAILED");
  else if (cmd == "drain") {
    drainMount();
    out.println("Mount serial drained.");
  }
  else if (cmd == "apdefault") {
    out.println("WARNING: apdefault may restart the AP/radio path and disconnect this Telnet session.");
    apSsid = "NexStar_Bridge";
    apPass = "12345678";
    apIp = "192.168.4.1";
    staRuntimeDisabled = true;
    bool ok = savePersistentSettings();
    scheduleApRestart("telnet apdefault");
    out.printf("AP defaults saved=%d; AP restarting as %s / %s / %s\n",
               ok ? 1 : 0, runtimeApSsid().c_str(), apPass.c_str(), apIp.c_str());
  }
  else if (cmd.startsWith("setsta ")) {
    out.println("WARNING: setsta changes WiFi connection state and may disconnect this Telnet session.");
    String rest = line.substring(7);
    rest.trim();
    int sp = rest.indexOf(' ');
    if (sp <= 0) {
      out.println("Usage: setsta <ssid> <password>");
    } else {
      String ssid = rest.substring(0, sp);
      String pass = rest.substring(sp + 1);
      ssid.trim();
      pass.trim();
      staRuntimeDisabled = false;
      bool saved = saveWiFiConfig(ssid, pass);
      bool connected = false;
      if (saved) {
        WiFi.mode(WIFI_STA);
        startFallbackAP();
        connected = connectConfiguredSTA();
      }
      out.printf("STA save=%d connected=%d ssid=%s ip=%s status=%s\n",
                 saved ? 1 : 0, connected ? 1 : 0, staSsid.c_str(),
                 WiFi.localIP().toString().c_str(), lastWifiStatus.c_str());
    }
  }
  else if (cmd == "modebt" || cmd == "modebt_web") {
    out.println("WARNING: modebt schedules a restart and will disconnect this Telnet session.");
    saveBluetoothLiteWebBootEnabled(true);
    saveBridgeMode(BRIDGE_MODE_BT_MIN_WEB);
    scheduleRestart("telnet modebt web");
    out.println("Saved BT mode with tiny web enabled; restarting soon.");
  }
  else if (cmd == "modebt_telnet" || cmd == "modebt_noweb" || cmd == "modebt_telnetonly") {
    out.println("WARNING: modebt_telnet schedules a restart and will disconnect this Telnet session.");
    out.println("Next boot will be BT with Telnet only; no tiny web page.");
    saveBluetoothLiteWebBootEnabled(false);
    saveBridgeMode(BRIDGE_MODE_BT_MIN_WEB);
    scheduleRestart("telnet modebt telnet-only");
    out.println("Saved BT Telnet-only mode; restarting soon.");
  }
  else if (cmd == "modewifi") {
    out.println("WARNING: modewifi schedules a restart and will disconnect this Telnet session.");
    saveBridgeMode(BRIDGE_MODE_WIFI_FULL);
    scheduleRestart("telnet modewifi");
    out.println("Saved full WiFi web mode; restarting soon.");
  }
  else if (cmd == "telnetlog") {
    Serial.printf("Telnet live logs: %s lines=%lu\n",
                  telnetLiveLogEnabled ? "enabled" : "disabled",
                  (unsigned long)telnetLiveLogLines);
    Serial.println("Commands: telnetlog 0 | telnetlog 1");
  }

  else if (cmd == "telnetlog 0") {
    telnetLiveLogEnabled = false;
    Serial.println("Telnet live logs disabled.");
  }

  else if (cmd == "telnetlog 1") {
    telnetLiveLogEnabled = true;
    Serial.println("Telnet live logs enabled.");
  }

  else if (cmd == "reboot" || cmd == "restart") {
    out.println("WARNING: reboot will disconnect this Telnet session.");
    out.println("Reboot command received; restarting ESP32...");
    delay(250);
    ESP.restart();
  }
  else if (cmd == "exit" || cmd == "quit") {
    out.println("Closing Telnet session by request.");
    if (telnetClient) telnetClient.stop();
  }
  else {
    out.print("Unknown command: ");
    out.println(line);
    out.println("Type help.");
  }
}

void telnetExecuteCommand(String line, Print &out) {
  line.trim();
  if (line.length() == 0) return;

  String cmd = line;
  cmd.toLowerCase();

  if (telnetPendingConfirmCommand.length()) {
    if (consoleConfirmYes(cmd)) {
      String confirmedCommand = telnetPendingConfirmCommand;
      telnetPendingConfirmCommand = "";
      telnetPendingConfirmDescription = "";
      out.println("Confirmed.");
      telnetRunCommand(confirmedCommand, out);
    } else if (consoleConfirmNo(cmd)) {
      telnetPendingConfirmCommand = "";
      telnetPendingConfirmDescription = "";
      out.println("Cancelled.");
    } else {
      out.print("Pending confirmation for '");
      out.print(telnetPendingConfirmCommand);
      out.println("'. Enter y or n.");
    }
    return;
  }

  String description;
  if (commandRequiresDisconnectConfirm(cmd, true, description)) {
    telnetPendingConfirmCommand = line;
    telnetPendingConfirmDescription = description;
    printCommandConfirmPrompt(out, line, description);
    return;
  }

  telnetRunCommand(line, out);
}

void startTelnetConsoleServer(const char* reason) {
#if defined(ESP32)
  if (!telnetServerPtr) telnetServerPtr = new WiFiServer(TELNET_PORT);
  telnetServer.begin();
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
    WiFiClient nc = telnetServer.available();
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

    // Basic Telnet option negotiation: ignore IAC triplets.
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
        continue;  // CRLF already processed on CR.
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

void handleConsole() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  String cmd = line;
  cmd.toLowerCase();

  Serial.print("[console] command echo: ");
  Serial.println(line);

  if (serialPendingConfirmCommand.length()) {
    if (consoleConfirmYes(cmd)) {
      line = serialPendingConfirmCommand;
      cmd = line;
      cmd.toLowerCase();
      serialPendingConfirmCommand = "";
      serialPendingConfirmDescription = "";
      Serial.println("Confirmed.");
    } else if (consoleConfirmNo(cmd)) {
      serialPendingConfirmCommand = "";
      serialPendingConfirmDescription = "";
      Serial.println("Cancelled.");
      return;
    } else {
      Serial.print("Pending confirmation for '");
      Serial.print(serialPendingConfirmCommand);
      Serial.println("'. Enter y or n.");
      return;
    }
  } else {
    String description;
    if (commandRequiresDisconnectConfirm(cmd, false, description)) {
      serialPendingConfirmCommand = line;
      serialPendingConfirmDescription = description;
      printCommandConfirmPrompt(Serial, line, description);
      return;
    }
  }

  if (cmd == "help" || cmd == "?") printHelp();
  else if (cmd.startsWith("help ")) printHelpTopic(line.substring(5));
  else if (cmd == "status") printConsoleStatus();

  else if (cmd == "gpio_startup" || cmd == "gpiostartup" || cmd == "startup_pins") {
    Serial.print(gpioStartupModeText());
  }

  else if (cmd == "current_state" || cmd == "currentstate" || cmd == "state") {
    Serial.println();
    Serial.println("=== Current State ===");
    Serial.print(currentStateText());
    Serial.println("=== End Current State ===");
  }

  else if (cmd == "system_health" || cmd == "systemhealth" || cmd == "health") {
    Serial.println();
    Serial.println("=== System Health ===");
    Serial.print(systemHealthText());
    Serial.println("=== End System Health ===");
  }

  else if (cmd == "tasks") {
    printTaskRuntimeStats(Serial);
  }

  else if (cmd == "reboot" || cmd == "restart") {
    Serial.println("Reboot command received; restarting ESP32...");
    delay(150);
    ESP.restart();
  }

  else if (cmd == "web" || cmd == "web status" || cmd == "webserver" || cmd == "webserver status") {
    Serial.printf("BT tiny web server runtime: %s\n", tinyWebServerRuntimeEnabled ? "enabled" : "disabled");
    Serial.println("Commands: web on | web off | web toggle");
    Serial.println("Note: runtime only; reboot restores enabled.");
  }

  else if (cmd == "web on" || cmd == "webserver on") {
    tinyWebServerRuntimeEnabled = true;
    Serial.println("BT tiny web server runtime enabled.");
  }

  else if (cmd == "web off" || cmd == "webserver off") {
    tinyWebServerRuntimeEnabled = false;
    Serial.println("BT tiny web server runtime disabled. Use serial 'web on' to re-enable, or reboot.");
  }

  else if (cmd == "web toggle" || cmd == "webserver toggle") {
    tinyWebServerRuntimeEnabled = !tinyWebServerRuntimeEnabled;
    Serial.printf("BT tiny web server runtime now: %s\n", tinyWebServerRuntimeEnabled ? "enabled" : "disabled");
  }

  else if (cmd == "wifi" || cmd == "wifi status") {
    Serial.printf("WiFi runtime: %s\n", wifiRuntimeEnabled ? "enabled" : "disabled");
    Serial.printf("Tiny web runtime: %s\n", tinyWebServerRuntimeEnabled ? "enabled" : "disabled");
    Serial.printf("WiFi mode text: %s\n", wifiModeText.c_str());
    Serial.printf("AP running: %d AP IP: %s STA connected: %d STA IP: %s\n",
                  apRunning ? 1 : 0,
                  WiFi.softAPIP().toString().c_str(),
                  (staConnected && WiFi.status() == WL_CONNECTED) ? 1 : 0,
                  WiFi.localIP().toString().c_str());
    Serial.println("Commands: wifi on | wifi off | wifi toggle");
  }

  else if (cmd == "wifi off") {
    runtimeWifiOff();
  }

  else if (cmd == "wifi on") {
    runtimeWifiOn();
  }

  else if (cmd == "wifi toggle") {
    if (wifiRuntimeEnabled) runtimeWifiOff();
    else runtimeWifiOn();
  }

  else if (cmd == "telnet" || cmd == "telnet status") {
    Serial.printf("Telnet console: port=%u password=%s client=%s auth=%s commands=%lu authFailures=%lu\n",
                  TELNET_PORT,
                  telnetMaskedPassword().c_str(),
                  (telnetClient && telnetClient.connected()) ? "connected" : "not connected",
                  telnetAuthenticated ? "yes" : "no",
                  (unsigned long)telnetRxCommands,
                  (unsigned long)telnetAuthFailures);
  }

  else if (cmd == "modebt" || cmd == "modebt_web") {
    saveBluetoothLiteWebBootEnabled(true);
    saveBridgeMode(BRIDGE_MODE_BT_MIN_WEB);
    scheduleRestart("serial modebt web");
    Serial.println("Saved BT mode with tiny web enabled; restarting...");
  }

  else if (cmd == "modebt_telnet" || cmd == "modebt_noweb" || cmd == "modebt_telnetonly") {
    saveBluetoothLiteWebBootEnabled(false);
    saveBridgeMode(BRIDGE_MODE_BT_MIN_WEB);
    scheduleRestart("serial modebt telnet-only");
    Serial.println("Saved BT Telnet-only mode; restarting without tiny web server...");
  }

  else if (cmd == "modewifi") {
    saveBridgeMode(BRIDGE_MODE_WIFI_FULL);
    scheduleRestart("serial modewifi");
    Serial.println("Saved full WiFi web mode; restarting...");
  }

  else if (cmd == "apdefault") {
    apSsid = "NexStar_Bridge";
    apPass = "12345678";
    apIp = "192.168.4.1";
    staRuntimeDisabled = true;
    bool ok = savePersistentSettings();
    scheduleApRestart("serial apdefault");
    Serial.printf("AP defaults saved=%d; soft AP restarting as %s / %s / %s\n",
                  ok ? 1 : 0, runtimeApSsid().c_str(), apPass.c_str(), apIp.c_str());
  }

  else if (cmd.startsWith("setsta ")) {
    String rest = line.substring(7);
    rest.trim();
    int sp = rest.indexOf(' ');
    if (sp <= 0) {
      Serial.println("Usage: setsta <ssid> <password>");
    } else {
      String ssid = rest.substring(0, sp);
      String pass = rest.substring(sp + 1);
      ssid.trim();
      pass.trim();
      staRuntimeDisabled = false;
      bool saved = saveWiFiConfig(ssid, pass);
      bool connected = false;
      if (saved) {
        WiFi.mode(WIFI_STA);
        startFallbackAP();
        connected = connectConfiguredSTA();
      }
      Serial.printf("STA save=%d connected=%d ssid=%s ip=%s status=%s\n",
                    saved ? 1 : 0, connected ? 1 : 0, staSsid.c_str(),
                    WiFi.localIP().toString().c_str(), lastWifiStatus.c_str());
    }
  }

  else if (cmd == "log") {
    Serial.println("[console] log command accepted: current status");
    printConsoleLogStatus();
  }

  else if (cmd.startsWith("log ")) {
    String args = line.substring(4);
    args.trim();

    int sp = args.indexOf(' ');
    String lvlText = (sp >= 0) ? args.substring(0, sp) : args;
    lvlText.trim();

    int lvl = lvlText.toInt();
    if (lvl < 0) lvl = 0;
    if (lvl > 5) lvl = 5;
    LOG_LEVEL = lvl;

    Serial.printf("[console] log level applied: %d (%s)\n", LOG_LEVEL, logLevelName(LOG_LEVEL));

    if (sp >= 0) {
      String cats = args.substring(sp + 1);
      cats.trim();
      if (cats.length() > 0) {
        uint16_t mask = consoleLogMaskFromList(cats);
        LOG_SUBSYSTEM_MASK = mask;

        Serial.print("[console] log systems applied: ");
        Serial.println(cats);
        Serial.printf("[console] log system mask now: 0x%04X\n", LOG_SUBSYSTEM_MASK);
        Serial.print("[console] enabled systems now: ");
        Serial.println(consoleLogMaskNames(LOG_SUBSYSTEM_MASK));
      }
    }

    String msg = "[info][system] Serial console log command applied: level=";
    msg += logLevelName(LOG_LEVEL);
    msg += " mask=0x";
    msg += String(LOG_SUBSYSTEM_MASK, HEX);
    msg += " enabled=";
    msg += consoleLogMaskNames(LOG_SUBSYSTEM_MASK);
    msg += " command='";
    msg += line;
    msg += "'";
    addLogLine(msg);
  }

  else if (cmd == "testinit") {
    bool ok = testInit();
    Serial.printf("testinit result: %s\n", ok ? "OK" : "FAILED");
  }

  else if (cmd == "get") {
    double ra = 0.0;
    double dec = 0.0;
    bool ok = getRaDec(ra, dec, true);
    Serial.printf("get result: %s\n", ok ? "OK" : "FAILED");
  }

  else if (cmd == "getaltaz") {
    double alt = 0.0;
    double az = 0.0;
    bool ok = getAltAz(alt, az, true);
    Serial.printf("getaltaz result: %s\n", ok ? "OK" : "FAILED");
  }

  else if (cmd == "rates") {
    Serial.printf("Nudge rates: %.3f %.3f %.3f %.3f poll=%lu idlePoll=%lu effectivePoll=%lu demand=%s throttle=%lu\n",
                  nudgeRateDeg[0], nudgeRateDeg[1], nudgeRateDeg[2], nudgeRateDeg[3],
                  pollIntervalMs, idlePollIntervalMs, effectiveMountPollIntervalMs(),
                  positionDemandActive() ? "active" : "idle", minClientPollIntervalMs);
  }

  else if (cmd == "mountpoll") {
    printMountPollValue(Serial);
  }

  else if (cmd.startsWith("mountpoll ")) {
    long p = line.substring(10).toInt();
    bool ok = setMountPollValue(p);
    Serial.printf("mountpoll set to %lu ms%s\n", pollIntervalMs, ok ? "" : " (save failed)");
  }

  else if (cmd == "idlepoll" || cmd == "poll idle") {
    Serial.printf("Idle poll interval: %lu ms\n", idlePollIntervalMs);
    Serial.printf("Effective poll interval: %lu ms (%s)\n",
                  effectiveMountPollIntervalMs(),
                  positionDemandActive() ? "active demand" : "idle");
  }

  else if (cmd.startsWith("idlepoll ") || cmd.startsWith("poll idle ")) {
    String arg = cmd.startsWith("idlepoll ") ? line.substring(9) : line.substring(10);
    arg.trim();
    long p = arg.toInt();
    if (p < 0) p = 0;
    if (p > 60000) p = 60000;
    idlePollIntervalMs = (unsigned long)p;
    nextMountPollDueMs = 0;
    bool ok = savePersistentSettings();
    Serial.printf("Idle poll interval set to %lu ms saved=%d\n", idlePollIntervalMs, ok ? 1 : 0);
  }

  else if (cmd == "nudge az+") Serial.printf("nudge az+ result: %s\n", nudgeRelativeAltAz(0.0, nudgeRateDeg[1]) ? "OK" : "FAILED");
  else if (cmd == "nudge az-") Serial.printf("nudge az- result: %s\n", nudgeRelativeAltAz(0.0, -nudgeRateDeg[1]) ? "OK" : "FAILED");
  else if (cmd == "nudge alt+") Serial.printf("nudge alt+ result: %s\n", nudgeRelativeAltAz(nudgeRateDeg[1], 0.0) ? "OK" : "FAILED");
  else if (cmd == "nudge alt-") Serial.printf("nudge alt- result: %s\n", nudgeRelativeAltAz(-nudgeRateDeg[1], 0.0) ? "OK" : "FAILED");
  else if (cmd == "drain") drainMount();
  else if (cmd == "pos") Serial.println(currentStateText());
  else { Serial.print("Unknown command: "); Serial.println(line); Serial.println("Type help."); }
}


void serviceGotoCompletionWatch() {
  if (!gotoCompletionWatchActive && !gotoCompletionVerifyPending) return;

  unsigned long nowMs = millis();

  if (gotoCompletionWatchActive) {
    while (MountSerial.available()) {
      uint8_t b = (uint8_t)MountSerial.read();
      printRxByte(b);
      markMountResponse();

      if (b == '@') {
        LOG_MOUNT_I("SkySafari GOTO completion '@' received; clearing LX200 UI slew state before real E/Z verification reads");

        // The original NexStar is now done with the GOTO.  Do not wait for the
        // slower post-GOTO E/Z verification before telling app clients that the
        // slew is complete.  SkySafari may keep its Goto/Stop button stuck on
        // Stop if :D# continues to imply slewing or if GR/GD still show the old
        // pre-GOTO cached coordinates.
        asyncSlewRunning = false;
        clearLX200GotoUiState("NexStar final '@' received");

        // Prime only the RA/Dec app-facing cache with the accepted GOTO target
        // so SkySafari immediately sees coordinates near the requested target.
        // The real mount-reported E/Z verification below will overwrite this
        // estimate as soon as it succeeds.
        cachedRA_deg = normalizeRA(gotoCompletionTargetRA_deg);
        cachedDec_deg = gotoCompletionTargetDec_deg;
        cacheValid = true;
        mountCurrentRA_deg = cachedRA_deg;
        mountCurrentDec_deg = cachedDec_deg;
        mountCurrentRaDecValid = true;
        mountCurrentRaDecMs = nowMs;
        LOG_MOUNT_I("SkySafari app-facing RA/Dec cache primed at GOTO completion: RA=%.6f Dec=%.6f until E/Z verification",
                    cachedRA_deg, cachedDec_deg);

        gotoCompletionWatchActive = false;
        gotoCompletionVerifyPending = true;
        gotoCompletionVerifyDueMs = nowMs + POST_GOTO_VERIFY_DELAY_MS;
        gotoCompletionWatchLastLogMs = 0;
        return;
      }

      LOG_MOUNT_W("SkySafari GOTO completion watch ignored byte 0x%02X '%c'",
                  b, isPrintableByte(b) ? b : '.');
    }

    if (gotoCompletionWatchLastLogMs == 0 || nowMs - gotoCompletionWatchLastLogMs >= GOTO_COMPLETION_WATCH_LOG_MS) {
      gotoCompletionWatchLastLogMs = nowMs;
      LOG_MOUNT_I("SkySafari GOTO still waiting for NexStar final '@' before post-GOTO E/Z query; elapsed=%lu ms",
                  (unsigned long)(nowMs - gotoCompletionWatchStartMs));
    }

    if (nowMs - gotoCompletionWatchStartMs > GOTO_TIMEOUT_MS) {
      LOG_MOUNT_W("SkySafari GOTO timed out waiting for final '@'; not sending E/Z verification because mount completion is unknown");
      gotoCompletionWatchActive = false;
      gotoCompletionVerifyPending = false;
      asyncSlewRunning = false;
      clearLX200GotoUiState("GOTO completion watch timed out");
      lastMountPollMs = nowMs;
    }
    return;
  }

  if (gotoCompletionVerifyPending) {
    if ((long)(nowMs - gotoCompletionVerifyDueMs) < 0) return;
    if (mountBusy) return;

    gotoCompletionVerifyPending = false;

    LOG_MOUNT_I("Post-GOTO verification: querying actual mount position now with E then Z");
    double verifyRa = 0.0;
    double verifyDec = 0.0;
    double verifyAlt = 0.0;
    double verifyAz = 0.0;

    if (getRaDec(verifyRa, verifyDec, true)) {
      LOG_MOUNT_I("Post-GOTO actual RA/Dec FROM MOUNT: RA_deg=%.6f DEC_deg=%.6f RA_hours=%.6f",
                  verifyRa, verifyDec, verifyRa / 15.0);
    } else {
      LOG_MOUNT_W("Post-GOTO actual RA/Dec read failed; keeping previous cache");
    }

    if (getAltAzFromMount(verifyAlt, verifyAz)) {
      LOG_MOUNT_I("Post-GOTO actual Alt/Az FROM MOUNT: ALT=%.6f AZ=%.6f", verifyAlt, verifyAz);
    } else {
      LOG_MOUNT_W("Post-GOTO actual Alt/Az read failed; keeping previous Alt/Az cache");
    }

    asyncSlewRunning = false;
    lastMountPollMs = millis();
    LOG_MOUNT_I("Post-GOTO verification finished");
  }
}

void serviceAsyncSlew() {
  if (mountBusy || asyncSlewRunning) return;

  if (asyncRaDecReadPending) {
    LOGI("Starting queued RA/Dec read now");
    asyncRaDecReadPending = false;
    double ra = 0.0;
    double dec = 0.0;
    if (getRaDec(ra, dec, true)) {
      // Do not compute/overwrite mount-reported Alt/Az after RA/Dec read.
      LOGI("Queued RA/Dec read complete");
    } else {
      LOGW("Queued RA/Dec read failed");
    }
    return;
  }

  if (asyncAltAzReadPending) {
    LOGI("Starting queued Alt/Az read now");
    asyncAltAzReadPending = false;
    double alt = 0.0;
    double az = 0.0;
    if (getAltAz(alt, az, true)) {
      // Do not compute/overwrite mount-reported RA/Dec after Alt/Az read.
      LOGI("Queued Alt/Az read complete");
    } else {
      LOGW("Queued Alt/Az read failed");
    }
    return;
  }

  if (asyncNudgePending) {
    LOGI("Starting queued nudge request now; it will route through the Alt/Az GOTO queue path");
    asyncNudgePending = false;
    if (nudgeRelativeAltAz(pendingNudgeAltDelta, pendingNudgeAzDelta)) {
      LOGI("Queued nudge converted to Alt/Az GOTO request");
      lastMountPollMs = 0;
    } else {
      LOGW("Queued nudge failed");
    }
    return;
  }

  if (asyncSlewPending) {
    if ((long)(millis() - asyncSlewEarliestMs) < 0) return;
    LOG_MOUNT_I("Starting queued async RA/Dec slew now");
    asyncSlewPending = false;
    if (gotoRaDec(pendingRA_deg, pendingDec_deg)) {
      gotoCompletionWatchActive = true;
      gotoCompletionVerifyPending = false;
      gotoCompletionWatchStartMs = millis();
      gotoCompletionWatchLastLogMs = 0;
      gotoCompletionVerifyDueMs = 0;
      gotoCompletionTargetRA_deg = pendingRA_deg;
      gotoCompletionTargetDec_deg = pendingDec_deg;
      asyncSlewRunning = true;
      LOG_MOUNT_I("SkySafari GOTO payload sent; watching non-blocking for NexStar final '@' before E/Z verification reads");
    } else {
      asyncSlewRunning = false;
      clearLX200GotoUiState("queued RA/Dec slew failed before mount command accepted");
      LOG_MOUNT_W("Queued RA/Dec slew failed before completion watch could start");
    }
    return;
  }

  if (asyncAltAzSlewPending) {
    if ((long)(millis() - asyncAltAzSlewEarliestMs) < 0) return;
    LOGI("Starting queued async Alt/Az slew now");
    asyncAltAzSlewPending = false;
    if (gotoAltAz(pendingAlt_deg, pendingAz_deg)) {
      LOGI("Queued Alt/Az slew accepted; pausing mount polls while SkySafari stays responsive");
      lastMountPollMs = millis();
    }
    return;
  }
}


#if defined(ESP32)
String httpParamValue(const String &url, const String &name) {
  String key = name + "=";
  int q = url.indexOf('?');
  if (q < 0) return "";
  int p = url.indexOf(key, q + 1);
  if (p < 0) return "";
  p += key.length();
  int e = url.indexOf('&', p);
  if (e < 0) e = url.length();
  return urlDecodeSimple(url.substring(p, e));
}

void applyHttpsBrowserTimeLocationFromUrl(const String &url, bool &gotLocation) {
  gotLocation = false;

  String v;
  v = httpParamValue(url, "lat");
  if (v.length()) {
    siteLatitudeDeg = v.toFloat();
    gotLocation = true;
  }
  v = httpParamValue(url, "lon");
  if (v.length()) {
    siteLongitudeDeg = v.toFloat();
    gotLocation = true;
  }
  if (gotLocation) {
    siteValid = true;
    markLocationSource("Browser HTTPS");
  }

  v = httpParamValue(url, "elev");
  if (v.length()) siteElevationMeters = v.toFloat();

  v = httpParamValue(url, "offset");
  if (v.length()) {
    utcOffsetMinutes = v.toInt();
    utcOffsetSource = "browser/https";
  }

  v = httpParamValue(url, "year");
  if (v.length()) localYear = v.toInt();
  v = httpParamValue(url, "month");
  if (v.length()) localMonth = v.toInt();
  v = httpParamValue(url, "day");
  if (v.length()) localDay = v.toInt();
  v = httpParamValue(url, "hour");
  if (v.length()) localHour = v.toInt();
  v = httpParamValue(url, "minute");
  if (v.length()) localMinute = v.toInt();
  v = httpParamValue(url, "second");
  if (v.length()) localSecond = v.toInt();

  timeValid = true;
  markTimeSource(TIME_SRC_WEB);
  timeSetMillis = millis();
  computeAltAzFromRaDec();
  savePersistentSettings();

  LOG_TIME_I("Browser HTTPS site/time update: location=%s lat=%.6f lon=%.6f offset=%d date=%04d-%02d-%02d time=%02d:%02d:%02d",
             gotLocation ? "yes" : "no",
             siteLatitudeDeg,
             siteLongitudeDeg,
             utcOffsetMinutes,
             localYear,
             localMonth,
             localDay,
             localHour,
             localMinute,
             localSecond);
}

String httpsAutoDeviceTimeLocationScript() {
  String js;
  js.reserve(2600);
  String ret = cleanGpsReturnUrl(gpsSyncReturnUrl);
  ret.replace("\\", "\\\\");
  ret.replace("'", "\\'");
  js += "<script>";
  js += "function nxEnc(v){return encodeURIComponent(v)};";
  js += "function nxReturnUrl(){try{let p=new URLSearchParams(location.search);return p.get('return')||'";
  js += ret;
  js += "'}catch(e){return '";
  js += ret;
  js += "'}};";
  js += "function nxMsg(s){let m=document.getElementById('httpsMsg');if(m)m.textContent=s};";
  js += "function nxSendDeviceSite(lat,lon,acc){let d=new Date();let off=-d.getTimezoneOffset();";
  js += "let ret=nxReturnUrl();";
  js += "let u='/set_device_site_time?year='+d.getFullYear()+'&month='+(d.getMonth()+1)+'&day='+d.getDate()+'&hour='+d.getHours()+'&minute='+d.getMinutes()+'&second='+d.getSeconds()+'&offset='+off+'&return='+nxEnc(ret);";
  js += "if(lat!==null&&lon!==null){u+='&lat='+nxEnc(lat)+'&lon='+nxEnc(lon);if(acc!==null)u+='&elev=0'}";
  js += "fetch(u,{cache:'no-store'}).then(r=>r.text()).then(()=>{nxMsg(lat!==null?'Browser time/date/location saved. Returning to main page in 5 seconds...':'Browser time/date saved; location was not provided. Returning to main page in 5 seconds...');setTimeout(()=>{let sep=ret.includes('?')?'&':'?';location.replace(ret+sep+'gpssync_return='+Date.now())},5000)}).catch(e=>{nxMsg('GPS Sync save failed: '+e)});}";
  js += "function nxTryBrowserSiteTime(){nxMsg('Requesting browser location/time...');";
  js += "if(navigator.geolocation){navigator.geolocation.getCurrentPosition(p=>nxSendDeviceSite(p.coords.latitude,p.coords.longitude,p.coords.accuracy),e=>nxSendDeviceSite(null,null,null),{enableHighAccuracy:true,timeout:10000,maximumAge:300000});}";
  js += "else{nxSendDeviceSite(null,null,null)}};";
  js += "window.addEventListener('load',()=>setTimeout(nxTryBrowserSiteTime,800));";
  js += "</script>";
  return js;
}

String buildHttpsMainPage() {
  String page;
  page.reserve(3200);
  String ret = cleanGpsReturnUrl(gpsSyncReturnUrl);
  ret.replace("\\", "\\\\");
  ret.replace("'", "&#39;");
  page += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<meta http-equiv='Cache-Control' content='no-store'>";
  page += "<title>NexStar GPS Sync</title></head><body style='font-family:Arial,sans-serif;background:#111;color:#eee;margin:18px'>";
  page += "<h2>NexStar GPS Sync</h2>";
  page += "<div id='httpsMsg' style='padding:10px;margin:10px 0;border:1px solid #555;background:#222;border-radius:8px'>Preparing GPS Sync...</div>";
  page += "<p>This HTTPS page lets the browser provide location permission. Browser time/date is also copied to the ESP32.</p>";
  page += "<p>Return target: <span style='color:#8cf'>";
  page += ret;
  page += "</span></p>";
  page += "<p><a style='display:inline-block;padding:10px 14px;background:#2d6cdf;color:#fff;text-decoration:none;border-radius:6px' href='";
  page += cleanGpsReturnUrl(gpsSyncReturnUrl);
  page += "'>Return without syncing</a></p>";
  page += "<p><a style='color:#8cf' href='/timeloc_status'>Time/Location Status</a> | <a style='color:#8cf' href='/status'>HTTPS Status</a></p>";
  page += httpsAutoDeviceTimeLocationScript();
  page += "</body></html>";
  return page;
}

void httpsSendText(httpd_req_t *req, const char *type, const String &body) {
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, body.c_str(), body.length());
}

String jsonBool(bool v) {
  return v ? "true" : "false";
}

esp_err_t httpsCatchAllHandler(httpd_req_t *req) {
  String uri = String(req->uri);
  String ret = httpParamValue(uri, "return");
  if (ret.length() && ret.startsWith("http://")) {
    gpsSyncReturnUrl = cleanGpsReturnUrl(ret);
  }

  String path = uri;
  int q = path.indexOf('?');
  if (q >= 0) path = path.substring(0, q);

  if (path == "/" || path == "/index.html" || path == "/gps_sync") {
    httpsSendText(req, "text/html", buildHttpsMainPage());
    return ESP_OK;
  }

  if (path == "/set_device_site_time") {
    bool gotLocation = false;
    applyHttpsBrowserTimeLocationFromUrl(uri, gotLocation);
    scheduleHttpsSetupStop(1800);
    ret = httpParamValue(uri, "return");
    if (ret.length() && ret.startsWith("http://")) gpsSyncReturnUrl = cleanGpsReturnUrl(ret);
    httpsSendText(req, "text/plain", gotLocation ? "HTTPS browser time/location saved" : "HTTPS browser time saved; location not provided");
    return ESP_OK;
  }

  if (path == "/timeloc_status") {
    String s;
    s.reserve(1200);
    s += "Time / Location status " + String(FW_VERSION) + "\n";
    s += "Time valid: " + String(timeValid ? "true" : "false") + "\n";
    s += "Time source: " + String(timeSourceName(currentTimeSource)) + "\n";
    s += "UTC offset minutes: " + String(utcOffsetMinutes) + "\n";
    s += "UTC offset source: " + utcOffsetSource + "\n";
    s += "Local date: " + String(localYear) + "-" + String(localMonth) + "-" + String(localDay) + "\n";
    s += "Local time: " + String(localHour) + ":" + String(localMinute) + ":" + String(localSecond) + "\n";
    s += "Site valid: " + String(siteValid ? "true" : "false") + "\n";
    s += "Location source: " + currentLocationSource + "\n";
    s += "Latitude: " + String(siteLatitudeDeg, 6) + "\n";
    s += "Longitude: " + String(siteLongitudeDeg, 6) + "\n";
    httpsSendText(req, "text/plain", s);
    return ESP_OK;
  }

  if (path == "/status") {
    String body = "{\"ok\":true,\"https\":true,\"timeValid\":" + jsonBool(timeValid) +
                  ",\"siteValid\":" + jsonBool(siteValid) +
                  ",\"apIP\":\"" + WiFi.softAPIP().toString() + "\"" +
                  ",\"staIP\":\"" + WiFi.localIP().toString() + "\"}";
    httpsSendText(req, "application/json", body);
    return ESP_OK;
  }

  if (path == "/mount_test") {
    String s = "Mount test is available on the normal HTTP web UI.\n";
    httpsSendText(req, "text/plain", s);
    return ESP_OK;
  }

  httpsSendText(req, "text/plain", "HTTPS GPS Sync route not implemented in " + String(FW_VERSION) + ": " + uri);
  return ESP_OK;
}

void startHttpsSetupServer() {
  if (httpsSetupServerHandle) return;

  httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
  conf.port_secure = HTTPS_SETUP_PORT;
  conf.servercert = (const unsigned char *)HTTPS_SETUP_CERT;
  conf.servercert_len = strlen(HTTPS_SETUP_CERT) + 1;
  conf.prvtkey_pem = (const unsigned char *)HTTPS_SETUP_KEY;
  conf.prvtkey_len = strlen(HTTPS_SETUP_KEY) + 1;
  conf.httpd.uri_match_fn = httpd_uri_match_wildcard;

  esp_err_t ret = httpd_ssl_start(&httpsSetupServerHandle, &conf);
  if (ret != ESP_OK) {
    Serial.printf("HTTPS web UI server failed to start: err=%d\n", (int)ret);
    httpsSetupServerHandle = NULL;
    return;
  }

  httpd_uri_t root_uri = {};
  root_uri.uri = "/";
  root_uri.method = HTTP_GET;
  root_uri.handler = httpsCatchAllHandler;
  root_uri.user_ctx = NULL;
  httpd_register_uri_handler(httpsSetupServerHandle, &root_uri);

  httpd_uri_t set_uri = {};
  set_uri.uri = "/set_device_site_time*";
  set_uri.method = HTTP_GET;
  set_uri.handler = httpsCatchAllHandler;
  set_uri.user_ctx = NULL;
  httpd_register_uri_handler(httpsSetupServerHandle, &set_uri);

  httpd_uri_t any_uri = {};
  any_uri.uri = "/*";
  any_uri.method = HTTP_GET;
  any_uri.handler = httpsCatchAllHandler;
  any_uri.user_ctx = NULL;
  httpd_register_uri_handler(httpsSetupServerHandle, &any_uri);

  Serial.printf("HTTPS web UI ready: https://%s/\n", WiFi.softAPIP().toString().c_str());
}
#endif

void handleModeRedirectHome() {
  webServer->sendHeader("Location", "/");
  webServer->sendHeader("Cache-Control", "no-store");
  webServer->send(303, "text/plain", "Redirecting to main UI");
}


String modeUrlEncode(const String &s) {
  String out;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}


String currentRequestBaseUrl() {
  String host = webServer->hostHeader();
  host.trim();
  if (host.length() == 0) {
    if (WiFi.status() == WL_CONNECTED) host = WiFi.localIP().toString();
    else host = WiFi.softAPIP().toString();
  }
  return String("http://") + host + "/";
}

String requestedReturnUrl() {
  String r = webServer->arg("return");
  r.trim();
  if (r.startsWith("http://")) return r;
  return currentRequestBaseUrl();
}


String deviceMacSuffix() {
#if defined(ESP32)
  uint64_t mac = ESP.getEfuseMac();
  char buf[5];
  snprintf(buf, sizeof(buf), "%04X", (uint16_t)(mac & 0xFFFF));
  return String(buf);
#else
  return "0000";
#endif
}

String appendMacSuffixToName(const String &base) {
  String suffix = deviceMacSuffix();
  if (base.endsWith("-" + suffix)) return base;
  if (base.endsWith("_" + suffix)) return base;
  return base + "-" + suffix;
}

String runtimeApSsid() {
  return appendMacSuffixToName(apSsid);
}

String runtimeBtName() {
  return appendMacSuffixToName(String(BT_NAME));
}


void handleFullWifiRebootPage() {
  String ret = requestedReturnUrl();
  if (ret.length() == 0 || !ret.startsWith("http://")) ret = currentRequestBaseUrl();

  String page;
  page.reserve(900);
  page += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<meta http-equiv='refresh' content='8;url=");
  page += ret;
  page += F("'><script>setTimeout(function(){location.replace('");
  page += ret;
  page += F("')},8000)</script>");
  page += F("<style>body{font-family:Arial,Helvetica,sans-serif;background:#0b1020;color:#edf2ff;margin:0;padding:20px}.card{max-width:560px;margin:25px auto;background:#151c2f;border:1px solid #2b3654;border-radius:12px;padding:18px}a{color:#8cc7ff}</style>");
  page += F("</head><body><div class='card'><h2>Rebooting</h2><p>The browser should return to the main UI automatically.</p><p><a href='");
  page += ret;
  page += F("'>Open main UI</a></p></div></body></html>");

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", page);
  delay(900);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  
  LOGI("Serial log mirror enabled: Full WiFi log format [level][system]");
delay(500);

  Serial.println();
  Serial.printf("%s %s\n", FW_NAME, FW_VERSION);
#if defined(ESP32)
  Serial.println("ESP32/WROOM build: HardwareSerial UART2 on GPIO16/GPIO17; Stellarium receive-buffer fix active");
#endif
  Serial.printf("Board: %s\n", BOARD_NAME);
  Serial.printf("Mount serial RX=%d TX=%d baud=%lu\n", MOUNT_RX_PIN, MOUNT_TX_PIN, MOUNT_BAUD);

#if defined(ESP8266)
  MountSerial.begin(MOUNT_BAUD);
#else
  MountSerial.begin(MOUNT_BAUD, SERIAL_8N1, MOUNT_RX_PIN, MOUNT_TX_PIN);
#endif
  Serial.println("[BOOT] mount serial started");

  delay(200);

#if defined(ESP32)
  Serial.println("[NVS] ESP32 using Preferences/NVS for settings");
  if (!loadPersistentSettings()) {
    Serial.println("[NVS] No saved NVS settings; using defaults");
  }
  if (ESP32_BOOT_DISABLE_BACKGROUND_POLLING) {
    pollIntervalMs = 0;
    nextMountPollDueMs = 0;
    Serial.println("[BOOT] ESP32 background mount polling disabled at boot");
  }
  if (!ESP32_BOOT_DISABLE_BACKGROUND_POLLING) {
    Serial.printf("[BOOT] ESP32 background mount polling loaded at %lu ms\n", pollIntervalMs);
  }
  if (ESP32_BOOT_AP_ONLY) {
    ntpEnabled = false;
    staConfigured = false;
    staSsid = "";
    staPass = "";
    Serial.println("[BOOT] ESP32 AP-only boot: STA credentials cleared and NTP disabled for this boot");
  }
#else
  {
    if (!LittleFS.begin()) {
      LOG_SET_E("LittleFS mount failed; persistent settings will not be available");
    } else {
      LOG_SET_I("LittleFS mounted for persistent settings");
      if (!loadPersistentSettings()) {
        LOGW("No valid LittleFS persistent settings found. This is normal after first flash, filesystem format change, or Clear Saved. Press Save Site / Time / Polling once to create valid settings.");
      }
    }
  }
#endif

  Serial.println("[BOOT] network/mode setup begin");
  loadBridgeMode();
  applyGpioStartupModeOverride();
  Serial.printf("[BOOT] bridge mode: %s source=%s pin=%d\n", bridgeModeName(), startupModeSource.c_str(), startupModePinUsed);
  if (bridgeMode == BRIDGE_MODE_BT_MIN_WEB) {
    ntpEnabled = false;
    loadBluetoothLitePollInterval();
    if (pollIntervalMs > 60000) pollIntervalMs = 2000;
    nextMountPollDueMs = 0;
    backgroundPollingAutoDisabled = false;
    backgroundPollFailCount = 0;
#if defined(ESP32)
    loadWiFiConfig(); // use saved STA/AP settings in exclusive STA-only or AP-only mode, but do not start the full WebServer.h UI
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    WiFi.softAPdisconnect(true);
    delay(150);
    staRuntimeDisabled = false; // BT should use saved STA when configured, not force AP-only
    setupWiFiFromSavedConfig();
    tinyWebServerRuntimeEnabled = btLiteBootWebEnabled;
    if (btLiteBootWebEnabled) {
      tinySetupServer.begin();
      Serial.println("[BT_MIN] tiny setup web server active");
    } else {
      Serial.println("[BT_MIN] tiny setup web server disabled; Telnet-only BT boot");
    }
    startTelnetConsoleServer("BT boot");
#endif
    wifiModeText = String(btLiteBootWebEnabled ? "BT exclusive WiFi + " : "BT Telnet-only WiFi + ") + wifiModeText;
    lastWifiStatus = String(btLiteBootWebEnabled ? "BT: full WebServer UI skipped; tiny setup server on saved WiFi mode. " : "BT Telnet-only: tiny setup server disabled on saved WiFi mode. ") + lastWifiStatus;
    Serial.printf("[BOOT] BT: tiny web=%s, Telnet active, WebServer.h services skipped, NTP disabled in BT mode, independent interval-only mount polling enabled at %lu ms\n", btLiteBootWebEnabled ? "enabled" : "disabled", pollIntervalMs);
  } else {
    setupWiFiFromSavedConfig();
  }
  Serial.println("[BOOT] network/mode setup done");

#if defined(ESP32)
  ALPACA_PORT = 11111;
  Serial.printf("[ALPACA] Forcing Alpaca HTTP server to port %u\n", ALPACA_PORT);
#endif

  Serial.printf("WiFi mode: %s\n", wifiModeText.c_str());
  Serial.printf("AP SSID: %s\n", apSsid.c_str());
  Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("STA SSID: %s\n", staSsid.c_str());
  Serial.printf("STA IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("Web UI HTTP port: %u\n", HTTP_WEB_PORT);
  if (bridgeMode == BRIDGE_MODE_WIFI_FULL) {
    Serial.printf("Alpaca HTTP port: %u\n", ALPACA_PORT);
    Serial.printf("LX200/SkySafari TCP port: %u\n", LX200_PORT);
    Serial.printf("Stellarium TCP port: %u\n", STELLARIUM_PORT);
    Serial.printf("Telnet console port: %u\n", TELNET_PORT);
    Serial.printf("Alpaca UDP discovery port: %u\n", ALPACA_DISCOVERY_PORT);
  } else {
    Serial.println("Bluetooth minimal mode: Alpaca, Stellarium, SkySafari WiFi, and Alpaca discovery disabled; Telnet console enabled if WiFi is on");
  }

#if defined(ESP32)
  if (ESP32_BOOT_WEB_ONLY || bridgeMode == BRIDGE_MODE_WIFI_FULL) {
    Serial.println("[WEB_ONLY] Bluetooth init skipped");
  } else
#endif
  {
#if HAS_CLASSIC_BT
    bool btOk = SerialBT.begin(runtimeBtName(), false, true);
    if (!btOk) {
      Serial.println("Bluetooth SPP first start failed; retrying once");
      SerialBT.end();
      delay(500);
      btOk = SerialBT.begin(runtimeBtName(), false, true);
    }
    if (btOk) {
      bluetoothRuntimeEnabled = true;
      Serial.printf("Bluetooth SPP name: %s\n", runtimeBtName().c_str());
#if defined(ESP32)
      coexPreferenceResult = (int)esp_coex_preference_set(ESP_COEX_PREFER_BT);
      coexPreferenceText = "prefer_bt";
      Serial.printf("[COEX] preference=%s result=%d version=%s\n",
                    coexPreferenceText.c_str(),
                    coexPreferenceResult,
                    esp_coex_version_get());
#endif
    } else {
      Serial.println("Bluetooth SPP failed to start");
    }
#else
    Serial.println("Bluetooth SPP disabled on this board");
#endif
  }

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

#if defined(ESP32)
  if (ESP32_BOOT_WEB_ONLY || bridgeMode == BRIDGE_MODE_BT_MIN_WEB) {
    Serial.println("[BT_MIN] UDP/LX200 WiFi/Stellarium listeners skipped; Telnet starts separately in BT");
  } else
#endif
  {
    Serial.println("[BOOT] starting UDP/LX200/Stellarium/Telnet listeners");
    discoveryUdp.begin(ALPACA_DISCOVERY_PORT);
    lx200Server.begin();
    stellariumServer.begin();
    startTelnetConsoleServer("Full WiFi boot");
    Serial.printf("[BOOT] TCP/UDP listeners started; Telnet console port %u\n", TELNET_PORT);
  }

  if (bridgeMode == BRIDGE_MODE_WIFI_FULL && webServer) {
    activeServer = webServer;
    LOG_WEB_I("Registering web UI routes on port %u", HTTP_WEB_PORT);
    server.on("/", HTTP_GET, handleRoot);
    server.on("/http_health", HTTP_GET, handleHttpHealthPage);
    server.on("/status", HTTP_GET, handleStatusPage);
    server.on("/btstatus", HTTP_GET, handleBtStatusPage);
    server.on("/bsc5_data", HTTP_GET, handleBsc5DataPage);
    server.on("/status_text", HTTP_GET, handleStatusTextPage);
    server.on("/sys_text", HTTP_GET, handleSystemTextPage);
    server.on("/setwifi", HTTP_GET, handleSetWiFiPage);
    server.on("/clearwifi", HTTP_GET, handleClearWiFiPage);
    server.on("/setap", HTTP_GET, handleSetApPage);
    server.on("/setmode", HTTP_GET, handleSetModePage);
    server.on("/reboot", HTTP_GET, handleFullWifiRebootPage);
    server.on("/setbtpoll", HTTP_GET, handleSetBtPollPage);
    if (bridgeMode == BRIDGE_MODE_WIFI_FULL) {
      server.on("/start_https", HTTP_GET, handleStartHttpsPage);
      server.on("/timeloc_status", HTTP_GET, handleTimeLocationStatusPage);
      server.on("/https_setup", HTTP_GET, handleHttpsSetupRedirectPage);
      server.on("/mount_test", HTTP_GET, handleMountTestPage);
      server.on("/logs", HTTP_GET, handleLogsPage);
      server.on("/setup_action", HTTP_GET, handleSetupActionPage);
      server.on("/setlog", HTTP_GET, handleSetLogPage);
      server.on("/setaltlimits", HTTP_GET, handleSetAltLimitsPage);
      server.on("/setdeclimits", HTTP_GET, handleSetDecLimitsPage);
      server.on("/setrates", HTTP_GET, handleSetRatesPage);
      server.on("/setwebrate", HTTP_GET, handleSetWebRatePage);
      server.on("/set_site_time", HTTP_GET, handleSetSiteTimePage);
      server.on("/set_manual_site_time", HTTP_GET, handleSetManualSiteTimePage);
      server.on("/set_device_site_time", HTTP_GET, handleSetDeviceSiteTimePage);
      server.on("/webnudge", HTTP_GET, handleWebNudgePage);
      server.on("/getradecweb", HTTP_GET, handleGetRaDecWebPage);
      server.on("/getaltazweb", HTTP_GET, handleGetAltAzWebPage);
      server.on("/webgoto_radec", HTTP_GET, handleWebGotoRaDecPage);
      server.on("/webgoto_altaz", HTTP_GET, handleWebGotoAltAzPage);
      server.on("/resetrates", HTTP_GET, handleResetRatesPage);
      server.on("/clearlogs", HTTP_GET, handleClearLogsPage);
      server.on("/clearlogalert", HTTP_GET, handleClearLogAlertPage);
      server.on("/fetch_ip_location", HTTP_GET, handleFetchIpLocationPage);
      server.on("/use_ip_location", HTTP_GET, handleUseIpLocationPage);
      server.on("/setntp", HTTP_GET, handleSetNtpPage);
      server.on("/syncntp", HTTP_GET, handleSyncNtpPage);
      server.on("/clearsettings", HTTP_GET, handleClearSettingsPage);
    }
    server.onNotFound(handleWebNotFound);
    webServer->on("/mode", handleModeRedirectHome);
    const char* headerKeys[] = {"Host"};
    webServer->collectHeaders(headerKeys, 1);

    // Start the Full WiFi HTTP listener exactly once. Starting the same
    // WebServer instance twice can leave the TCP listener in an unreliable
    // state after boot or restart.
    server.begin();
    Serial.printf("[WEB] HTTP server started once on port %u; AP=%s STA=%s heap=%u\n",
                  HTTP_WEB_PORT,
                  WiFi.softAPIP().toString().c_str(),
                  WiFi.localIP().toString().c_str(),
                  ESP.getFreeHeap());

    if (bridgeMode == BRIDGE_MODE_WIFI_FULL) {
      activeServer = alpacaServer;
      LOG_ALP_I("Registering Alpaca API routes on port %u", ALPACA_PORT);
      server.on("/management/apiversions", HTTP_GET, handleManagementApiVersions);
      server.on("/management/v1/description", HTTP_GET, handleManagementDescription);
      server.on("/management/v1/configureddevices", HTTP_GET, handleConfiguredDevices);
      server.onNotFound(handleNotFound);
      Serial.println("[BOOT] alpacaServer.begin");
      server.begin();
      Serial.printf("[ALPACA] HTTP server started on port %u\n", ALPACA_PORT);
    } else {
      Serial.println("[BT_MIN] Alpaca HTTP server skipped");
    }
  } else {
    Serial.println("[BT_MIN] Full Web UI disabled; BT main/status pages are available");
  }

  activeServer = webServer;

#if defined(ESP32)
  if (bridgeMode == BRIDGE_MODE_WIFI_FULL) {
    if (HTTPS_WEB_AUTO_START) {
      startHttpsSetupServer();
    } else {
      Serial.printf("HTTPS GPS Sync auto-start disabled; HTTP UI is active on port %u. Use GPS Sync button to start HTTPS on demand.\n", HTTP_WEB_PORT);
    }
  } else {
    Serial.println("[BT_MIN] HTTPS/GPS Sync disabled because WiFi/web mode is not active");
  }
#endif

  if (bridgeMode == BRIDGE_MODE_WIFI_FULL) {
    LOG_WEB_I("Web UI HTTP server ready AP: http://%s:%u/ STA: http://%s:%u/", WiFi.softAPIP().toString().c_str(), HTTP_WEB_PORT, WiFi.localIP().toString().c_str(), HTTP_WEB_PORT);
    LOG_SKY_I("LX200 SkySafari WiFi server ready AP %s:%u STA %s:%u", WiFi.softAPIP().toString().c_str(), LX200_PORT, WiFi.localIP().toString().c_str(), LX200_PORT);
    LOG_STEL_I("Stellarium server ready AP %s:%u STA %s:%u", WiFi.softAPIP().toString().c_str(), STELLARIUM_PORT, WiFi.localIP().toString().c_str(), STELLARIUM_PORT);
    LOGI("Alpaca discovery listening on UDP %u", ALPACA_DISCOVERY_PORT);
  }
#if HAS_CLASSIC_BT
  if (bluetoothRuntimeEnabled) LOG_SKY_I("Bluetooth LX200 SPP ready as %s", BT_NAME);
#endif

  printHelp();
}

void loop() {
  unsigned long loopStart = millis();

  // Highest-priority task: maintain the configured mount poll interval before
  // servicing console, web, protocol, Telnet, or other lower-priority work.
  serviceMountPolling();

#if defined(ESP32)
  serviceHttpsSetupStop();
#endif

  handleConsole();
  serviceMountPolling();
  serviceHttpServers();
  serviceMountPolling();
  serviceRestart();
  serviceMountPolling();

#if defined(ESP32)
  if (ESP32_BOOT_WEB_ONLY) {
    static unsigned long lastWebOnlyBeatMs = 0;
    if (millis() - lastWebOnlyBeatMs > 5000) {
      lastWebOnlyBeatMs = millis();
      Serial.printf("[WEB_ONLY] alive ip=%s stations=%d heap=%u\n",
                    WiFi.softAPIP().toString().c_str(),
                    WiFi.softAPgetStationNum(),
                    ESP.getFreeHeap());
    }
    lastLoopTimeMs = millis() - loopStart;
    if (lastLoopTimeMs > maxLoopTimeMs) maxLoopTimeMs = lastLoopTimeMs;
    lastLoopLatencyMs = lastLoopTimeMs;
    if (lastLoopLatencyMs > maxLoopLatencyMs) maxLoopLatencyMs = lastLoopLatencyMs;
    yield();
    return;
  }
#endif

  if (bridgeMode == BRIDGE_MODE_WIFI_FULL) {
    handleAlpacaDiscovery();
    serviceMountPolling();
    handleLX200Server();
    serviceMountPolling();
    handleStellariumServer();
    serviceMountPolling();
    serviceTelnetConsole();
    serviceMountPolling();
    serviceNtpSync();
    serviceMountPolling();
    serviceGotoCompletionWatch();
    serviceGotoQueue();
    serviceAsyncSlew();
  } else {
    serviceTinySetupServer();
    serviceMountPolling();
    serviceTelnetConsole();
    serviceMountPolling();
    handleBluetoothLX200();
#if HAS_CLASSIC_BT
    if (BT_AUTO_WIFI_OFF_ON_CLIENT &&
        bridgeMode == BRIDGE_MODE_BT_MIN_WEB &&
        bluetoothRuntimeEnabled &&
        SerialBT.hasClient() &&
        wifiRuntimeEnabled &&
        !btAutoWifiOffDone &&
        btClientConnectedAtMs != 0 &&
        millis() - btClientConnectedAtMs > 1500UL) {
      btAutoWifiOffDone = true;
      Serial.println("[BT_MIN] Bluetooth client active; turning WiFi off for BT stability");
      runtimeWifiOff();
    }
#endif
    serviceMountPolling();
    serviceGotoCompletionWatch();
    serviceGotoQueue();
    serviceAsyncSlew();
  }
  applyPendingSiteTimeUpdate();
  serviceMountPolling();

  lastLoopTimeMs = millis() - loopStart;
  if (lastLoopTimeMs > maxLoopTimeMs) maxLoopTimeMs = lastLoopTimeMs;
  lastLoopLatencyMs = lastLoopTimeMs;
  if (lastLoopLatencyMs > maxLoopLatencyMs) maxLoopLatencyMs = lastLoopLatencyMs;

  yield();
}
