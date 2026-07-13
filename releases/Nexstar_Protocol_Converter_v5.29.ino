#include <Arduino.h>
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
const bool SERIAL_MIRROR_FULL_WIFI_LOGS = true; // mirror the same formatted Full WiFi log lines to USB serial


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

#if defined(ESP32)
const uint16_t HTTPS_SETUP_PORT = 443;
const bool HTTPS_WEB_AUTO_START = false; // v3.30: keep HTTP port 80 alive; GPS Sync starts HTTPS on demand
httpd_handle_t httpsSetupServerHandle = NULL;
unsigned long httpsSetupStopAtMillis = 0;
String gpsSyncReturnUrl = "http://192.168.4.1/";
const char HTTPS_SETUP_CERT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDCDCCAfCgAwIBAgIUIB273RE0jgQCdZtinymIq1x1H68wDQYJKoZIhvcNAQEL
BQAwFjEUMBIGA1UEAwwLMTkyLjE2OC40LjEwHhcNMjYwNzA0MjMyNjIyWhcNMzYw
NzAxMjMyNjIyWjAWMRQwEgYDVQQDDAsxOTIuMTY4LjQuMTCCASIwDQYJKoZIhvcN
AQEBBQADggEPADCCAQoCggEBALZJxI/tOStsYH8b/N0Hkhc8aiUVfXDohF7ahU0p
1wYNzsbpJLWR2wilOQb7dN/vAXibnmR1cej/5YSFKZT77IMnNktJrVQMULeisvFU
CJo49chJdArmhG1qz43dRKpx7XZN895CcsJWTybs7iJPDkeadEMWh+pxw5UQP0z2
jjoEY79YoK3uKmRiq3ff3WXf3sJUKUBonXRvQbaFBcqNmEizFPrkFs/UHhZVIhRY
e4B6nl8Goluntflu3fIHFB51a7zDsGoYx529roRMu/kFY+a0RlqshivfmwclSOFt
/vHrviwEQ1wXPb6fh36TDyx3X+BnSRVgTkyre28ijY6fFt0CAwEAAaNOMEwwKwYD
VR0RBCQwIocEwKgEAYINbmV4c3Rhci5sb2NhbIILZXNwMzIubG9jYWwwHQYDVR0O
BBYEFHnCedu2IdUMUYOW0/eUafTC7ivHMA0GCSqGSIb3DQEBCwUAA4IBAQACq3cB
tPOUJRvOpEjH0eGQtYMPXTsjBSYmS93KVaZ7EHYSMOSlIdvfCOnkYB11jC2PNHLo
OIPMumWyxrwXzu8wYU1ycz9Tb1iK/naLqqvMhKTiLe/FHW+C3rsRs+J8emH/c3XC
Z4EAsgshdn25E0zBEmliUKqjTzCEIkefGr3MLZ+8zbvhq1ztA+NeFtfiHx+x7cm6
B85wld3aUh5np4+60sMyv6XBNO8a4gVQHLAXZNzXyKepLp3E1ccN32ebHygKvfw/
UMfnUUQQsPRoJi3nGO3FzF4mjOGE/f3k/gFr949jS879rn8hgx3zm8/cepW+OAhO
VYgIyaiofucDv5bH
-----END CERTIFICATE-----

)EOF";
const char HTTPS_SETUP_KEY[] PROGMEM = R"EOF(
-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQC2ScSP7TkrbGB/
G/zdB5IXPGolFX1w6IRe2oVNKdcGDc7G6SS1kdsIpTkG+3Tf7wF4m55kdXHo/+WE
hSmU++yDJzZLSa1UDFC3orLxVAiaOPXISXQK5oRtas+N3USqce12TfPeQnLCVk8m
7O4iTw5HmnRDFofqccOVED9M9o46BGO/WKCt7ipkYqt3391l397CVClAaJ10b0G2
hQXKjZhIsxT65BbP1B4WVSIUWHuAep5fBqJbp7X5bt3yBxQedWu8w7BqGMedva6E
TLv5BWPmtEZarIYr35sHJUjhbf7x674sBENcFz2+n4d+kw8sd1/gZ0kVYE5Mq3tv
Io2OnxbdAgMBAAECggEAAaZdm5SLlGuI3LhS9miCTNM0qLzu443kiNE/LJ7I00Ve
Y5QqS2Rpuu8ikCrII4YPxC4wZaYJz7s6h2kHInyR5vFSFKYIO2mtygO0d+peoOVh
ascowDfW51X0pqA5O2EIQ65amzPhwwWHS4m7HAoNeIjFgvKaF1KhRmDybw66OdHa
xSByZB3xESB3F+Hi1ya6r6jkjq2XTzyQJCgAD3/ViNafjQ6k+Qko7yiz5//VE6WA
XsFTwcYYLhkZEIlqnIyj0JuTTPHGWwHh/o7I3N78C6Hgi+eWhTmssgfCe7yyPSTw
Cyops5s1wsFtl6BKkFFUvTR4aOT8yDgk2cEOWKapwQKBgQDiORmiGKhqFoEWbqPD
G47UPw4H/MtS98LjTlQlOv4/pqVF4D4MAkQ+5CAP6Pr1IVKA7v6omjLeTn0DEWqn
sBcv2SYeoe8u8PMe8vO7r2E09rYkhrq2AiNH7eb72OJIsgxIV1uhAEexbX6+hvHE
9gwUbnrtLEcsg1Zfz1lVQIVHvQKBgQDOSDhw7L1vkaAsqLaTZ6JSDzdcyrUa8YsL
vIEOz+k+l3vo/jlFRsd8qGbRj4iBoil5eDfN1ak6RegS/zCJN87vdtURO92nAtxm
FvIaZzLbcqbNsRKAeRADhvTaqmIlhcjv2i4fZrXavF3Y+/8BTNW6IVfF/oF/o2KZ
wxJ5PZ/toQKBgFOqZo6KrA7AT/Gp7asFECfzQg82MUR4GX3TxE8YqFuGGG3lZ00t
sWvJFwqLUfVC466HtWtJzDJnuNhfoqBuAcVSfESsAzfLKT9y/y2UyVC7RdXwdjFG
TSIXHGxcZCQapWxD0sGSxvEZ29w/MD91+DW+Pnxk+dW+pT0+BH4BJMnJAoGAUJPl
eDcByJMZ/lfo+auBvIw1FAoatGul5O+9egu9ELYbsOedd3IueoNNpo5qxDiT+t76
7WyIrjqgbMtCKleifeftUs4Pxy1W6ooMCERHmXEvtyl0ELs5hicxfjkQHZgk5YxU
d++nGcp63keRVPCujAZ6Qt0nuLQZz/ZjQPjRgAECgYA+WvF88RrUndnspVowD1RZ
AxQzZx9TY0L8Gvs9BO485SxbIfP1YHP/AW/RDbyAGDE1xwKacBw1CL/HjCS23uqD
hn0xtU+c3FAyxInsmgsbUNjjj/0oe7xelSps/s3sk0GCsZoZf9UNJyU81Q/aRcyj
+ctPZV64I7RxyooyHcUguw==
-----END PRIVATE KEY-----

)EOF";
#endif

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

enum LogLevel {
  LOG_NONE  = 0,
  LOG_ERROR = 1,
  LOG_WARN  = 2,
  LOG_INFO  = 3,
  LOG_DEBUG = 4,
  LOG_TRACE = 5
};

enum LogCategory {
  LOG_CAT_MOUNT      = 1 << 0,
  LOG_CAT_SKYSAFARI  = 1 << 1,
  LOG_CAT_ALPACA     = 1 << 2,
  LOG_CAT_STELLARIUM = 1 << 3,
  LOG_CAT_WEB        = 1 << 4,
  LOG_CAT_WIFI       = 1 << 5,
  LOG_CAT_TIMELOC    = 1 << 6,
  LOG_CAT_SETTINGS   = 1 << 7,
  LOG_CAT_SYSTEM     = 1 << 8,
  LOG_CAT_BLUETOOTH  = 1 << 9
};

const uint16_t LOG_CAT_ALL =
  LOG_CAT_MOUNT | LOG_CAT_SKYSAFARI | LOG_CAT_ALPACA | LOG_CAT_STELLARIUM |
  LOG_CAT_WEB | LOG_CAT_WIFI | LOG_CAT_TIMELOC | LOG_CAT_SETTINGS | LOG_CAT_SYSTEM | LOG_CAT_BLUETOOTH;

enum LX200Source {
  LX_SRC_WIFI = 0,
  LX_SRC_BT   = 1
};

int LOG_LEVEL = LOG_WARN;
uint16_t LOG_SUBSYSTEM_MASK = LOG_CAT_ALL;
bool logAlertActive = false;
String logAlertText = "";
unsigned long logAlertMs = 0;
const char* currentWebRequestPath = "";

const int LOG_BUFFER_LINES = 100;
String logBuffer[LOG_BUFFER_LINES];
int logWriteIndex = 0;
bool logWrapped = false;

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

const char* logLevelName(int level) {
  switch (level) {
    case LOG_ERROR: return "error";
    case LOG_WARN:  return "warn";
    case LOG_INFO:  return "info";
    case LOG_DEBUG: return "debug";
    case LOG_TRACE: return "trace";
    default:        return "none";
  }
}

const char* logCategoryName(uint16_t cat) {
  switch (cat) {
    case LOG_CAT_MOUNT:      return "mount";
    case LOG_CAT_SKYSAFARI:  return "skysafari";
    case LOG_CAT_ALPACA:     return "alpaca";
    case LOG_CAT_STELLARIUM: return "stellarium";
    case LOG_CAT_WEB:        return "web";
    case LOG_CAT_WIFI:       return "wifi";
    case LOG_CAT_TIMELOC:    return "time_location";
    case LOG_CAT_SETTINGS:   return "settings";
    case LOG_CAT_BLUETOOTH:  return "bluetooth";
    default:                 return "system";
  }
}

// Full WiFi log lines are mirrored to serial by addLogLine().
// Use LOG_* / LOG_*_CAT macros for formatted serial-visible logging.

String shortLogTimestamp() {
  unsigned long total = millis() / 1000UL;
  unsigned long hh = (total / 3600UL) % 100UL;
  unsigned long mm = (total / 60UL) % 60UL;
  unsigned long ss = total % 60UL;
  char buf[12];
  snprintf(buf, sizeof(buf), "[%02lu:%02lu:%02lu]", hh, mm, ss);
  return String(buf);
}

bool logLineAlreadyTimestamped(const String &line) {
  return line.length() >= 10 &&
         line.charAt(0) == '[' &&
         line.charAt(3) == ':' &&
         line.charAt(6) == ':' &&
         line.charAt(9) == ']';
}

void addLogLine(const String &line) {
  String stamped = line;
  if (!logLineAlreadyTimestamped(stamped)) {
    stamped = shortLogTimestamp() + " " + stamped;
  }

  logBuffer[logWriteIndex] = stamped;
  logWriteIndex++;
  if (logWriteIndex >= LOG_BUFFER_LINES) {
    logWriteIndex = 0;
    logWrapped = true;
  }

#if defined(ESP32) || defined(ESP8266)
  if (SERIAL_MIRROR_FULL_WIFI_LOGS) {
    Serial.println(stamped);
  }
#endif

#if defined(ESP32)
  // Live Telnet log mirror. The log level/category filters are already applied
  // before addLogLine() is called, so Telnet sees the same filtered stream as
  // the COM-port console. Use CRLF so Telnet clients do not stair-step lines.
  if (telnetLiveLogEnabled && telnetAuthenticated && telnetClient && telnetClient.connected()) {
    telnetClient.print("\r\n");
    telnetClient.print(stamped);
    telnetClient.print("\r\n> ");
    telnetLiveLogLines++;
  }
#endif
}

void logPrintfCat(int level, uint16_t cat, const char* fmt, ...) {
  if (LOG_LEVEL < level) return;
  if ((LOG_SUBSYSTEM_MASK & cat) == 0) return;

  char msg[220];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  String line;
  line.reserve(260);
  line += "[";
  line += logLevelName(level);
  line += "][";
  line += logCategoryName(cat);
  line += "] ";
  line += msg;
  if (level <= LOG_WARN && level > LOG_NONE) {
    logAlertActive = true;
    logAlertText = line;
    logAlertMs = millis();
  }
  addLogLine(line);
}

// Default legacy logs go to the system bucket.
// Critical existing logs still appear unless system is unchecked.
void logPrintf(int level, const char* tag, const char* fmt, ...) {
  if (LOG_LEVEL < level) return;
  if ((LOG_SUBSYSTEM_MASK & LOG_CAT_SYSTEM) == 0) return;

  char msg[220];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  String line;
  line.reserve(260);
  line += "[";
  line += logLevelName(level);
  line += "][system] ";
  line += msg;
  if (level <= LOG_WARN && level > LOG_NONE) {
    logAlertActive = true;
    logAlertText = line;
    logAlertMs = millis();
  }
  addLogLine(line);
}

#define LOGE(fmt, ...) logPrintf(LOG_ERROR, "ERROR", fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) logPrintf(LOG_WARN,  "WARN",  fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) logPrintf(LOG_INFO,  "INFO",  fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) logPrintf(LOG_DEBUG, "DEBUG", fmt, ##__VA_ARGS__)
#define LOGT(fmt, ...) logPrintf(LOG_TRACE, "TRACE", fmt, ##__VA_ARGS__)

#define LOG_MOUNT_E(fmt, ...) logPrintfCat(LOG_ERROR, LOG_CAT_MOUNT, fmt, ##__VA_ARGS__)
#define LOG_MOUNT_W(fmt, ...) logPrintfCat(LOG_WARN,  LOG_CAT_MOUNT, fmt, ##__VA_ARGS__)
#define LOG_MOUNT_I(fmt, ...) logPrintfCat(LOG_INFO,  LOG_CAT_MOUNT, fmt, ##__VA_ARGS__)
#define LOG_MOUNT_D(fmt, ...) logPrintfCat(LOG_DEBUG, LOG_CAT_MOUNT, fmt, ##__VA_ARGS__)
#define LOG_MOUNT_T(fmt, ...) logPrintfCat(LOG_TRACE, LOG_CAT_MOUNT, fmt, ##__VA_ARGS__)

#define LOG_SKY_E(fmt, ...) logPrintfCat(LOG_ERROR, LOG_CAT_SKYSAFARI, fmt, ##__VA_ARGS__)
#define LOG_SKY_W(fmt, ...) logPrintfCat(LOG_WARN,  LOG_CAT_SKYSAFARI, fmt, ##__VA_ARGS__)
#define LOG_SKY_I(fmt, ...) logPrintfCat(LOG_INFO,  LOG_CAT_SKYSAFARI, fmt, ##__VA_ARGS__)
#define LOG_SKY_D(fmt, ...) logPrintfCat(LOG_DEBUG, LOG_CAT_SKYSAFARI, fmt, ##__VA_ARGS__)
#define LOG_SKY_T(fmt, ...) logPrintfCat(LOG_TRACE, LOG_CAT_SKYSAFARI, fmt, ##__VA_ARGS__)

#define LOG_ALP_E(fmt, ...) logPrintfCat(LOG_ERROR, LOG_CAT_ALPACA, fmt, ##__VA_ARGS__)
#define LOG_ALP_W(fmt, ...) logPrintfCat(LOG_WARN,  LOG_CAT_ALPACA, fmt, ##__VA_ARGS__)
#define LOG_ALP_I(fmt, ...) logPrintfCat(LOG_INFO,  LOG_CAT_ALPACA, fmt, ##__VA_ARGS__)
#define LOG_ALP_D(fmt, ...) logPrintfCat(LOG_DEBUG, LOG_CAT_ALPACA, fmt, ##__VA_ARGS__)
#define LOG_ALP_T(fmt, ...) logPrintfCat(LOG_TRACE, LOG_CAT_ALPACA, fmt, ##__VA_ARGS__)

#define LOG_STEL_E(fmt, ...) logPrintfCat(LOG_ERROR, LOG_CAT_STELLARIUM, fmt, ##__VA_ARGS__)
#define LOG_STEL_W(fmt, ...) logPrintfCat(LOG_WARN,  LOG_CAT_STELLARIUM, fmt, ##__VA_ARGS__)
#define LOG_STEL_I(fmt, ...) logPrintfCat(LOG_INFO,  LOG_CAT_STELLARIUM, fmt, ##__VA_ARGS__)
#define LOG_STEL_D(fmt, ...) logPrintfCat(LOG_DEBUG, LOG_CAT_STELLARIUM, fmt, ##__VA_ARGS__)
#define LOG_STEL_T(fmt, ...) logPrintfCat(LOG_TRACE, LOG_CAT_STELLARIUM, fmt, ##__VA_ARGS__)

#define LOG_WEB_E(fmt, ...) logPrintfCat(LOG_ERROR, LOG_CAT_WEB, fmt, ##__VA_ARGS__)
#define LOG_WEB_W(fmt, ...) logPrintfCat(LOG_WARN,  LOG_CAT_WEB, fmt, ##__VA_ARGS__)
#define LOG_WEB_I(fmt, ...) logPrintfCat(LOG_INFO,  LOG_CAT_WEB, fmt, ##__VA_ARGS__)
#define LOG_WEB_D(fmt, ...) logPrintfCat(LOG_DEBUG, LOG_CAT_WEB, fmt, ##__VA_ARGS__)
#define LOG_WEB_T(fmt, ...) logPrintfCat(LOG_TRACE, LOG_CAT_WEB, fmt, ##__VA_ARGS__)

#define LOG_WIFI_E(fmt, ...) logPrintfCat(LOG_ERROR, LOG_CAT_WIFI, fmt, ##__VA_ARGS__)
#define LOG_WIFI_W(fmt, ...) logPrintfCat(LOG_WARN,  LOG_CAT_WIFI, fmt, ##__VA_ARGS__)
#define LOG_WIFI_I(fmt, ...) logPrintfCat(LOG_INFO,  LOG_CAT_WIFI, fmt, ##__VA_ARGS__)
#define LOG_WIFI_D(fmt, ...) logPrintfCat(LOG_DEBUG, LOG_CAT_WIFI, fmt, ##__VA_ARGS__)
#define LOG_WIFI_T(fmt, ...) logPrintfCat(LOG_TRACE, LOG_CAT_WIFI, fmt, ##__VA_ARGS__)

#define LOG_TIME_E(fmt, ...) logPrintfCat(LOG_ERROR, LOG_CAT_TIMELOC, fmt, ##__VA_ARGS__)
#define LOG_TIME_W(fmt, ...) logPrintfCat(LOG_WARN,  LOG_CAT_TIMELOC, fmt, ##__VA_ARGS__)
#define LOG_TIME_I(fmt, ...) logPrintfCat(LOG_INFO,  LOG_CAT_TIMELOC, fmt, ##__VA_ARGS__)
#define LOG_TIME_D(fmt, ...) logPrintfCat(LOG_DEBUG, LOG_CAT_TIMELOC, fmt, ##__VA_ARGS__)
#define LOG_TIME_T(fmt, ...) logPrintfCat(LOG_TRACE, LOG_CAT_TIMELOC, fmt, ##__VA_ARGS__)

#define LOG_SET_E(fmt, ...) logPrintfCat(LOG_ERROR, LOG_CAT_SETTINGS, fmt, ##__VA_ARGS__)
#define LOG_SET_W(fmt, ...) logPrintfCat(LOG_WARN,  LOG_CAT_SETTINGS, fmt, ##__VA_ARGS__)
#define LOG_SET_I(fmt, ...) logPrintfCat(LOG_INFO,  LOG_CAT_SETTINGS, fmt, ##__VA_ARGS__)
#define LOG_SET_D(fmt, ...) logPrintfCat(LOG_DEBUG, LOG_CAT_SETTINGS, fmt, ##__VA_ARGS__)
#define LOG_SET_T(fmt, ...) logPrintfCat(LOG_TRACE, LOG_CAT_SETTINGS, fmt, ##__VA_ARGS__)

#define LOG_BT_E(fmt, ...) logPrintfCat(LOG_ERROR, LOG_CAT_BLUETOOTH, fmt, ##__VA_ARGS__)
#define LOG_BT_W(fmt, ...) logPrintfCat(LOG_WARN,  LOG_CAT_BLUETOOTH, fmt, ##__VA_ARGS__)
#define LOG_BT_I(fmt, ...) logPrintfCat(LOG_INFO,  LOG_CAT_BLUETOOTH, fmt, ##__VA_ARGS__)
#define LOG_BT_D(fmt, ...) logPrintfCat(LOG_DEBUG, LOG_CAT_BLUETOOTH, fmt, ##__VA_ARGS__)
#define LOG_BT_T(fmt, ...) logPrintfCat(LOG_TRACE, LOG_CAT_BLUETOOTH, fmt, ##__VA_ARGS__)

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


// Full BSC5 catalog is served on demand so the main web page stays small.
static const uint16_t BSC5_ROW_COUNT = 9096;
static const uint16_t BSC5_CHUNK_COUNT = 97;
static const char BSC5_CHUNK_0[] PROGMEM = R"BSC5(1|HR1|861|452292|670^2|HR2|844|-5031|629^3|33 Psc|889|-57075|461^4|86 Peg|950|133961|551^5|HR5|1044|584367|596^6|HR6|1053|-490750|570^7|10 Cas|1074|641961|559^8|HR8|1102|290214|613^9|HR9|1139|-231075|618^10|HR10|1217|-173864|619^11|HR11|1289|-25489|643^12|HR12|1297|-225089|594^13|HR13|1343|-335294|568^14|HR14|1367|-24478|607^15|Alpheratz|1398|290906|206^16|HR16|1382|-88239|599^17|HR17|1447|366267|619^18|HR18|1426|-175775|606^19|HR19|1478|254628|623^20|HR20|1556|797147|601^21|Caph|1530|591497|227^22|87 Peg|1507|182119|553^23|HR23|1507|-540019|633^24|κ¹ Scl|1558|-279878|542^25|ε Phe|1569|-457475|388^26|34 Psc|1673|111456|551^27|22 And|1720|460722|503^28|HR28|1749|571656|674^29|HR29|1719|-52486|584^30|γ³ Oct|1672|-822239|528^31|HR31|1786|-125800|585^32|HR32|1774|-732244|664^33|6 Cet|1878|-154681|489^34|κ² Scl|1929|-277997|541^35|θ Scl|1956|-351331|525^36|HR36|1997|481525|616^37|HR37|2028|-179383|525^38|HR38|2140|376933|673^39|Algenib|2206|151836|283^40|HR40|2233|269872|630^41|23 And|2252|410353|572^42|HR42|2284|-260219|594^43|HR43|2289|-262847|631^44|HR44|2340|332061|625^45|χ Peg|2434|202067|480^46|HR46|2410|-77806|512^47|HR47|2221|-849942|577^48|7 Cet|2440|-189328|444^49|HR49|2489|222842|624^50|35 Psc|2497|88208|579^51|HR51|2485|-95697|575^52|HR52|2519|315358|645^53|HR53|2529|272831|635^54|HR54|2495|-349044|617^55|HR55|2706|769508|635^56|HR56|2727|435947|615^57|HR57|2691|-314464|567^58|HR58|2653|-759114|649^59|36 Psc|2761|82400|611^60|HR60|2825|615333|574^61|HR61|2785|-202106|647^62|HR62|2859|479475|589^63|θ And|2849|386817|461^64|HR64|2803|-787806|677^65|HR65|2953|514331|614^66|HR66|2924|-190511|645^67|HR67|2966|16889|617^68|σ And|3055|367853|452^69|HR69|3048|112058|605^70|26 And|3117|437911|611^71|HR71|3106|315172|587^72|HR72|3116|-80528|646^73|HR73|3118|-432353|633^74|Deneb Kaitos Schemali|3238|-88239|356^75|HR75|3282|407297|633^76|HR76|3348|488653|652^77|ζ Tuc|3345|-648747|423^78|HR78|3401|309358|590^79|HR79|3460|329114|579^80|41 Psc|3433|81903|537^81|HR81|3485|109769|656^82|ρ And|3520|379686|518^83|π Tuc|3442|-696250|551^84|ι Scl|3587|-289817|518^85|HR85|3629|-200578|512^86|42 Psc|3737|134825|623^87|HR87|3579|-774269|597^88|9 Cet|3811|-122094|639^89|HR89|3868|-310361|655^90|HR90|4006|385772|739^91|HR91|4043|520200|557^93|12 Cas|4132|618311|540^94|HR94|4082|-22192|607^96|HR96|4184|530469|574^97|44 Psc|4234|19397|577^98|β Hyi|4292|-772542|280^99|Ankaa|4381|-423061|239^100|κ Phe|4367|-436800|394^101|10 Cet|4437|-497|619^102|HR102|4541|-255472|598^103|47 Psc|4675|178931|506^104|HR104|4705|443944|517^105|η Scl|4655|-330072|481^106|48 Psc|4702|164450|606^107|HR107|4722|101897|604^108|HR108|4725|-203350|643^109|HR109|4740|-399150|543^110|HR110|4824|369000|626^111|HR111|4786|-505328|626^112|HR112|5153|770194|621^113|HR113|5055|599772|594^114|28 And|5020|2)BSC5";
static const char BSC5_CHUNK_1[] PROGMEM = R"BSC5(97517|523^115|HR115|4978|-148642|614^116|HR116|4969|-321167|657^117|12 Cet|5007|-39572|572^118|HR118|5063|-237878|519^119|HR119|5077|-409394|619^120|HR120|5072|-482150|569^121|13 Cas|5237|665194|618^122|HR122|5238|335817|587^123|λ Cas|5296|545222|473^124|HR124|5281|528394|560^125|λ¹ Phe|5236|-488036|477^126|β¹ Tuc|5258|-629581|437^127|β² Tuc|5260|-629658|454^128|HR128|5408|434947|670^129|HR129|5554|709817|642^130|κ Cas|5500|629317|416^131|52 Psc|5432|202944|538^132|51 Psc|5399|69556|567^133|HR133|5429|275806|667^134|HR134|5470|282803|630^135|HR135|5529|548950|593^136|β³ Tuc|5455|-630311|509^137|16 Cas|5736|667503|648^138|HR138|5614|-295583|555^139|θ Tuc|5565|-712661|613^140|HR140|5744|-523731|557^141|HR141|5820|133711|640^142|13 Cet|5875|-35928|520^143|14 Cet|5924|-5056|593^144|HR144|6023|541686|508^145|HR145|5985|132067|641^146|HR146|6076|603261|579^147|λ² Phe|5948|-480008|551^148|HR148|5926|-548219|606^149|HR149|6056|272547|650^150|HR150|6008|-149736|645^151|HR151|6019|-228425|606^152|HR152|6129|444886|513^153|ζ Cas|6162|538969|366^154|π And|6147|337194|436^155|53 Psc|6131|152317|589^156|HR156|6187|240142|647^157|HR157|6225|353994|548^158|HR158|6631|824939|640^159|HR159|6224|-247672|557^160|HR160|6104|-651247|642^161|HR161|6251|31353|639^162|HR162|6217|-543942|641^163|ε And|6426|293117|437^164|HR164|6528|493544|543^165|δ And|6555|308608|327^166|54 Psc|6561|212506|587^167|55 Psc|6654|214383|536^168|Schedar|6751|565372|223^169|HR169|6447|-731372|685^170|HR170|6661|-339617|669^171|HR171|6644|-447967|601^172|HR172|6746|-165169|649^173|HR173|6758|-238044|614^174|HR174|6784|-43519|591^175|32 And|6853|394586|533^176|HR176|6740|-594544|589^177|HR177|7009|661475|583^178|HR178|6933|246292|604^179|ξ Cas|7011|505125|480^180|μ Phe|6888|-460850|459^181|HR181|7086|587533|617^183|ξ Phe|6962|-565017|570^184|π Cas|7245|470247|494^185|λ¹ Scl|7119|-384633|606^186|HR186|7116|-602625|598^187|ρ Tuc|7079|-654681|539^188|Diphda|7265|-179867|204^189|HR189|7407|478642|567^190|HR190|7306|-120117|602^191|η Phe|7226|-574631|436^192|21 Cas|7608|749881|566^193|ο Cas|7454|482844|454^194|φ¹ Cet|7365|-106094|476^195|λ² Scl|7367|-384217|590^196|HR196|7548|552217|542^197|HR197|7457|-220061|524^198|HR198|7492|-426767|594^199|HR199|7423|-624978|607^200|HR200|7775|693250|633^201|HR201|7567|-46292|615^202|HR202|7500|-537150|615^203|18 Cet|7580|-128808|615^204|HR204|7709|553053|652^205|HR205|7697|448614|605^206|HR206|7616|-164242|647^207|HR207|7784|595744|639^208|23 Cas|7961|748475|541^209|HR209|7627|-475517|580^210|HR210|7699|-225219|550^211|57 Psc|7758|154756|538^212|HR212|8025|726750|587^213|58 Psc|7837|119739|550^214|59 Psc|7871|195789|613^215|ζ And|7890|242672|406^216|60 Psc|7899|67408|599^217|61 Psc|7986|209253|654^218|HR218|7954|-180614|570^219|Achird|8183|578158|344^220|HR220|8003|)BSC5";
static const char BSC5_CHUNK_2[] PROGMEM = R"BSC5(-217225|557^221|62 Psc|8048|73000|593^222|HR222|8064|52806|575^223|ν Cas|8139|509683|489^224|δ Psc|8114|75850|443^225|64 Psc|8163|169406|507^226|ν And|8302|410789|453^227|HR227|8238|-135614|559^228|HR228|8205|-241364|590^229|HR229|8158|-466978|627^230|65 Psc|8313|277108|700^231|65 Psc|8314|277103|710^232|HR232|8259|-233617|628^233|HR233|8454|642475|539^234|HR234|8384|450022|615^235|φ² Cet|8354|-106444|519^236|λ Hyi|8098|-749233|507^237|HR237|8546|618058|607^238|HR238|8493|515081|639^239|HR239|8344|-433947|648^240|HR240|9148|837072|562^241|HR241|8594|515711|621^242|ρ Phe|8448|-509869|522^243|HR243|8551|33850|637^244|HR244|8845|611242|482^245|HR245|8645|-437092|690^246|HR246|8815|385486|669^247|HR247|8779|-240058|546^248|20 Cet|8835|-11442|477^249|HR249|8912|374181|606^250|HR250|8966|526892|627^251|HR251|8868|-247769|646^252|λ¹ Tuc|8734|-695044|622^253|υ¹ Cas|9167|589728|483^254|66 Psc|9098|191883|574^255|21 Cet|9049|-87408|616^256|HR256|9181|486786|627^257|HR257|8939|-628714|570^258|36 And|9161|236283|547^259|HR259|9208|245569|620^260|HR260|9369|579967|621^261|HR261|9488|687761|637^262|67 Psc|9329|272094|609^263|HR263|9284|-73472|585^264|Navi|9451|607167|247^265|υ² Cas|9444|591811|463^266|HR266|9464|603628|555^267|φ³ Cet|9338|-112667|531^268|HR268|9321|-277756|610^269|μ And|9459|384994|387^270|λ² Tuc|9168|-695269|545^271|η And|9534|234175|442^272|HR272|9610|458397|612^273|HR273|9753|663522|597^274|68 Psc|9639|289922|542^275|HR275|9706|339508|598^276|HR276|9651|136958|632^277|HR277|9719|214044|637^278|HR278|10086|709831|639^279|φ⁴ Cet|9789|-113800|561^280|α Scl|9768|-293575|431^281|HR281|9729|-606964|623^282|HR282|10009|447111|684^283|HR283|10010|447133|604^284|HR284|9971|64831|611^285|HR285|11458|862569|425^286|HR286|15640|890156|646^287|HR287|10384|510350|647^288|ξ Scl|10218|-389167|559^289|HR289|10504|473761|645^290|39 And|10484|413450|598^291|σ Psc|10470|318044|550^292|HR292|10603|610750|592^293|σ Scl|10407|-315519|550^294|ε Psc|10491|78900|428^295|ω Phe|10338|-570025|611^296|25 Cet|10507|-48367|543^297|HR297|10721|615803|584^298|HR298|10673|525022|599^299|HR299|10470|-463975|536^300|HR300|10549|-295258|629^301|26 Cet|10636|13667|604^302|HR302|10797|510100|654^303|HR303|10743|296586|619^304|HR304|10452|-654561|621^305|HR305|10768|399911|672^306|HR306|12704|871453|625^307|73 Psc|10813|56564|600^308|72 Psc|10848|149461|568^309|HR309|11063|627617|654^310|ψ¹ Psc|10947|214733|534^311|ψ¹ Psc|10949|214653|556^312|HR312|11534|800117|629^313|77 Psc|10970|49083|635^314|77 Psc|10976|49094|725^315|27 Cet|10936|-99792|612^316|HR316|11167|569350|643^317|28 Cet|11014|-98394|558^318|HR318|11193|534983|638^319|75 Psc|11093|129561|612^320|HR320|11021|-239925|614^321|Marfak|11379|549203|517^322|β Phe|11014|-467186|331^323|HR323|11074|-356608|661^324|41 And|11336|4394)BSC5";
static const char BSC5_CHUNK_3[] PROGMEM = R"BSC5(19|503^325|HR325|11203|-239964|637^326|HR326|11426|582636|579^327|78 Psc|11337|320122|625^328|ψ² Psc|11326|207392|555^329|30 Cet|11295|-97856|582^330|80 Psc|11395|56497|552^331|υ Phe|11300|-414869|521^332|ι Tuc|11219|-617753|537^333|HR333|12046|796739|564^334|Dheneb|11432|-101822|345^335|φ And|11584|472419|425^336|31 Cas|11776|687786|529^337|Mirach|11622|356206|206^338|ζ Phe|11398|-552458|392^339|ψ³ Psc|11637|196586|555^340|44 And|11719|420814|565^341|HR341|11721|254578|580^342|HR342|11904|642028|555^343|Marfark|11851|551497|433^344|HR344|11699|156742|606^345|32 Cas|11948|650189|557^346|32 Cet|11700|-89061|640^347|33 Cet|11760|24456|595^348|45 And|11862|377242|581^349|82 Psc|11852|314247|516^350|HR350|11687|-576944|641^351|χ Psc|11909|210347|466^352|τ Psc|11943|300897|451^353|34 Cet|11954|-22511|594^354|HR354|12194|617058|641^355|HR355|12094|453378|611^356|HR356|12165|300642|619^357|HR357|12752|799100|626^358|HR358|12065|-308022|652^359|HR359|12126|-378564|592^360|φ Psc|12291|245836|465^361|ζ Psc|12289|75753|524^362|ζ Psc|12293|75783|630^363|HR363|12347|285297|643^364|87 Psc|12354|161336|598^365|HR365|12700|717439|783^366|37 Cet|12400|-79231|513^367|88 Psc|12451|69953|603^368|38 Cet|12470|-9739|570^369|HR369|12735|480822|661^370|ν Phe|12531|-455314|496^371|HR371|12719|331147|602^372|HR372|12847|449019|634^373|39 Cet|12767|-25003|541^374|HR374|12900|317447|673^375|HR375|13387|775706|631^376|HR376|13028|474197|625^377|κ Tuc|12628|-688761|486^378|89 Psc|12967|36144|516^379|HR379|13131|373861|646^380|HR380|12843|-663981|624^381|HR381|13664|762389|638^382|φ Cas|13347|582317|498^383|υ Psc|13244|272642|476^384|35 Cas|13514|646583|634^385|42 Cet|13301|-5089|587^386|HR386|13963|787258|607^387|HR387|13429|-32469|623^388|HR388|13411|-112389|615^389|91 Psc|13521|287381|523^390|Adhil|13723|455289|488^391|HR391|13893|581431|645^392|HR392|13769|17264|620^393|43 Cet|13763|-4497|649^394|HR394|13751|-190814|635^395|47 And|13946|377150|558^396|HR396|13938|342458|629^397|HR397|13902|204689|597^398|HR398|14296|709800|649^399|ψ Cas|14322|681300|474^400|HR400|13919|-309456|584^401|44 Cet|14007|-80075|621^402|θ Cet|14004|-81833|360^403|Ruchbah|14303|602353|268^404|HR404|14057|-69147|591^405|HR405|14111|-156603|614^406|HR406|14135|-28486|615^407|HR407|14266|235117|618^408|HR408|14113|-414925|542^409|HR409|14385|434578|596^410|HR410|14358|345797|631^411|HR411|14116|-445283|626^412|46 Cet|14270|-145989|490^413|ρ Psc|14376|191722|538^414|94 Psc|14449|192403|550^415|HR415|14517|343775|627^416|HR416|14409|-3986|641^417|ω And|14609|454067|483^418|HR418|14574|411006|646^419|HR419|14482|35353|658^420|HR420|14181|-643694|593^421|47 Cet|14477|-130567|566^422|HR422|14631|403356|660^423|HR423|14495|-325431|579^424|Polaris|25302|892642|202^425|HR425|14629|-109017|613^426|HR426|14730|7961)BSC5";
static const char BSC5_CHUNK_4[] PROGMEM = R"BSC5(4|620^427|38 Cas|15205|702647|581^428|HR428|15145|660981|614^429|γ Phe|14728|-433183|341^430|49 And|15017|470072|527^431|HR431|14787|-337636|658^432|97 Psc|14980|183556|602^433|48 Cet|14934|-216294|512^434|μ Psc|15031|61439|484^435|HR435|14918|-467564|631^436|HR436|15064|-262078|593^437|Kullat Nunu|15247|153458|362^438|HR438|15354|348000|639^439|HR439|15572|583275|570^440|δ Phe|15209|-490728|395^441|HR441|15287|-302833|582^442|χ Cas|15655|592319|471^443|HR443|15275|-455756|617^444|HR444|15510|-90147|659^445|HR445|15489|-368653|551^446|HR446|15713|372372|588^447|HR447|15434|-497278|628^448|HR448|15619|-70253|576^449|HR449|16229|743008|658^450|HR450|15803|184606|589^451|49 Cet|15772|-156761|563^452|HR452|15979|410764|638^453|HR453|15807|-318922|612^454|HR454|16076|487228|592^455|101 Psc|15962|146614|622^456|40 Cas|16419|730400|528^457|HR457|15986|174336|580^458|υ And|16133|414056|409^459|50 Cet|15997|-154003|542^460|HR460|15876|-581394|601^461|HR461|16354|579775|556^462|τ Scl|16023|-299075|569^463|π Psc|16183|121417|557^464|Nembus|16332|486283|357^465|HR465|16421|454000|636^466|HR466|16271|-94039|624^467|HR467|15609|-785047|611^468|HR468|16124|-582708|618^469|χ And|16558|443861|498^470|HR470|16703|538683|639^471|HR471|16409|-365283|594^472|Achernar|16286|-572367|46^473|HR473|16477|-212753|558^474|HR474|16472|-250219|670^475|105 Psc|16613|164058|597^476|HR476|16777|432978|561^477|τ And|16763|405769|494^478|43 Cas|17057|680431|559^479|HR479|16467|-534389|684^480|42 Cas|17155|706225|518^481|HR481|17008|610383|671^482|HR482|17049|586278|637^483|HR483|16964|426136|495^484|HR484|16884|257458|617^485|HR485|16942|300472|599^486|HR486|16632|-561981|587^487|HR487|16633|-561947|576^488|HR488|17162|614217|634^489|ν Psc|16905|54875|444^490|HR490|17010|352456|564^491|44 Cas|17221|605511|578^492|HR492|16958|-113247|575^493|107 Psc|17083|202686|524^494|HR494|16909|-381331|617^495|HR495|17212|453222|634^496|φ Per|17277|506886|407^497|π Scl|17024|-323269|525^498|HR498|17008|-368325|572^499|HR499|17383|575364|621^500|HR500|17121|-36903|499^501|HR501|16948|-500389|664^502|HR502|17461|570892|625^503|HR503|17306|321917|634^504|HR504|17407|461397|635^505|HR505|16967|-607894|571^506|HR506|17081|-537406|552^507|HR507|17319|-47656|619^508|109 Psc|17488|200831|627^509|τ Cet|17345|-159375|350^510|Torcularis Septentrionalis|17566|91578|426^511|HR511|17958|638522|563^512|HR512|16321|-829750|587^513|HR513|17665|-57333|534^514|ε Scl|17608|-250525|531^515|HR515|17765|174131|655^516|τ¹ Hyi|16892|-791483|633^517|HR517|17669|-273492|639^518|HR518|17967|462297|632^519|HR519|17683|-508164|549^520|HR520|17684|-535219|504^521|HR521|18108|379528|594^522|4 Ari|18030|169556|584^523|HR523|18116|326903|579^524|HR524|17880|-417600|618^525|HR525|16244|-847697|569^526|HR526|18210|478969|582^527|HR52)BSC5";
static const char BSC5_CHUNK_5[] PROGMEM = R"BSC5(7|18072|36856|591^528|HR528|17966|-371597|632^529|HR529|18492|519333|590^530|1 Ari|18357|222753|586^531|χ Cet|18264|-106864|467^532|HR532|18221|-310728|634^533|1 Per|18665|551475|552^534|HR534|18478|110433|594^535|HR535|18302|-384039|637^536|2 Per|18693|507928|579^537|HR537|18389|-478164|614^538|HR538|18808|514747|626^539|Baten Kaitos|18577|-103350|373^540|HR540|18968|555981|645^541|HR541|18485|-502061|594^542|Segin|19066|636700|338^543|55 And|18881|407297|540^544|Mothallah|18847|295789|341^545|Mesarthim|18922|192958|483^546|Mesarthim|18922|192936|475^547|HR547|18811|-169292|580^548|ω Cas|19333|686853|499^549|ξ Psc|18926|31875|462^550|τ² Hyi|17962|-801767|606^551|HR551|19149|407019|624^552|HR552|19160|371283|626^553|Sheratan|19107|208081|264^554|HR554|18898|-385947|610^555|ψ Phe|18941|-463025|441^556|HR556|19318|372778|589^557|56 And|19359|372517|567^558|φ Phe|19061|-424969|511^559|7 Ari|19308|235772|574^560|HR560|19316|18497|601^561|HR561|19759|616981|602^562|HR562|19657|416944|678^563|ι Ari|19559|178175|510^564|HR564|19622|278044|582^565|56 Cet|19445|-225269|485^566|χ Eri|19326|-516089|370^567|HR567|19939|646214|526^568|3 Per|19760|492042|569^569|λ Ari|19655|235961|479^570|η² Hyi|19156|-676472|469^571|HR571|19296|-608614|606^572|HR572|20492|779164|604^573|HR573|19500|-517661|610^574|HR574|19528|-473850|483^575|48 Cas|20326|709069|454^576|HR576|19741|-330667|635^577|HR577|19932|210583|587^578|HR578|19905|122947|609^579|HR579|20529|738506|623^580|50 Cas|20572|724214|398^581|47 Cas|20854|772814|538^582|112 Psc|20026|30972|588^583|57 Cet|19961|-208244|541^584|HR584|19649|-654247|637^585|υ Cet|20001|-210778|400^586|52 Cas|20480|649014|600^587|HR587|20075|-85236|551^588|HR588|19941|-420306|557^589|53 Cas|20501|643900|558^590|4 Per|20384|544875|504^591|Head of Hydrus|19795|-615697|286^592|49 Cas|20920|761150|522^593|σ Hyi|19307|-783483|616^594|π For|20208|-300017|535^595|Alrescha|20341|27636|523^596|Alrescha|20341|27636|433^597|HR597|21570|812958|605^598|HR598|20778|651033|652^599|ε Tri|20494|332839|550^600|HR600|19948|-660664|610^601|HR601|20431|134767|594^602|χ Phe|20284|-447136|514^603|Almach|20650|423297|226^604|γ² And|20652|423308|484^605|10 Ari|20609|259356|563^606|HR606|20411|-296650|642^607|60 Cet|20532|1283|543^608|HR608|20496|-153058|586^609|HR609|20618|182533|621^610|61 Cet|20634|-3403|593^611|HR611|20612|-41036|562^612|ν For|20748|-292969|469^613|κ Ari|21094|226483|503^614|HR614|21034|82475|631^615|11 Ari|21137|257047|615^616|HR616|21081|350|628^617|Hamal|21196|234625|200^618|HR618|21446|584236|567^619|HR619|21427|444594|642^620|58 And|21415|378592|482^621|HR621|21688|538431|631^622|β Tri|21591|349872|300^623|14 Ari|21570|259397|498^624|HR624|21564|172244|643^625|HR625|21460|-177794|610^626|HR626|22226|740278|629^627|5 Per|21914|576458|636^628)BSC5";
static const char BSC5_CHUNK_6[] PROGMEM = R"BSC5(|59 And|21813|390394|563^629|59 And|21816|390431|610^630|HR630|21597|-243458|648^631|15 Ari|21771|195003|570^632|HR632|21526|-435167|585^633|16 Ari|21867|259369|602^634|5 Tri|21903|315264|623^635|64 Cet|21892|85697|563^636|HR636|21680|-438156|632^637|HR637|21738|-508244|612^638|HR638|21895|-100522|601^639|63 Cet|21933|-18253|593^640|55 Cas|22414|665244|607^641|HR641|22282|585611|644^642|6 Tri|22062|303031|494^643|60 And|22204|442317|483^644|HR644|22104|241678|596^645|HR645|22268|510658|531^646|η Ari|22134|212108|527^647|HR647|22341|474842|606^648|19 Ari|22176|152797|571^649|ξ¹ Cet|22167|88467|437^650|66 Cet|22132|-23936|554^651|HR651|22169|-210003|586^652|μ For|22151|-307239|528^653|HR653|22661|478114|633^654|HR654|22810|570553|648^655|7 Tri|22656|333589|528^656|20 Ari|22628|257831|579^657|21 Ari|22619|250431|558^658|HR658|22579|-94656|655^659|HR659|22422|-411667|591^660|δ Tri|22842|342242|487^661|8 Per|23000|578997|575^662|7 Per|23012|575167|598^663|HR663|22926|443069|670^664|γ Tri|22886|338472|401^665|HR665|22862|237678|655^666|67 Cet|22831|-64222|551^667|π¹ Hyi|22374|-678417|555^668|HR668|23369|643372|660^669|θ Ari|23021|199011|562^670|62 And|23213|473800|530^671|HR671|23197|464725|621^672|HR672|23004|17578|558^673|HR673|23230|489553|637^674|φ Eri|22752|-515122|356^675|10 Tri|23158|286425|503^676|HR676|23161|231678|646^677|HR677|23270|398350|663^678|π² Hyi|22579|-677464|569^679|HR679|23448|473108|611^680|HR680|23346|301883|647^681|Mira|23224|-29775|304^682|63 And|23495|501514|559^683|HR683|23162|-259456|634^684|HR684|23280|-43456|650^685|9 Per|23726|558456|517^686|HR686|23235|-418483|637^687|HR687|23806|413964|582^688|HR688|23317|-559447|581^689|69 Cet|23657|3958|528^690|HR690|23977|553644|628^691|70 Cet|23701|-8850|542^692|HR692|23671|-107778|546^693|HR693|23681|-176622|587^694|64 And|24069|500067|519^695|κ For|23757|-238164|520^696|10 Per|24211|566100|625^697|HR697|23827|-183544|622^698|HR698|23699|-432000|631^699|65 And|24271|502786|471^700|HR700|23851|-375764|653^701|HR701|23818|-510922|592^702|ξ Ari|24136|106106|547^703|HR703|24056|-258475|644^704|71 Cet|24162|-27800|633^705|δ Hyi|23625|-686594|409^706|HR706|24094|-408406|618^707|ι Cas|24844|674025|452^708|ρ Cet|24325|-122906|489^709|66 And|24644|505697|612^710|HR710|24334|-153411|583^711|HR711|24519|270133|618^712|11 Tri|24577|318014|554^713|HR713|24431|-200428|588^714|λ Hor|24150|-603119|535^715|κ Hyi|23812|-736458|501^716|HR716|24903|555364|651^717|12 Tri|24694|296694|529^718|ξ² Cet|24693|84600|428^719|HR719|24667|19608|645^720|13 Tri|24801|299319|589^721|κ Eri|24498|-477039|425^722|HR722|24240|-664947|641^723|HR723|24871|234689|619^724|φ For|24671|-338111|514^725|HR725|24931|95658|607^726|HR726|25046|338339|625^727|HR727|24765|-311025|611^728|HR728|25090|252350|592^729|26 Ari|25107|198553)BSC5";
static const char BSC5_CHUNK_7[] PROGMEM = R"BSC5(|615^730|HR730|24987|-226828|677^731|27 Ari|25151|177039|623^732|HR732|25126|2553|600^733|HR733|25038|-251864|651^734|HR734|24679|-642997|637^735|HR735|25091|-225456|610^736|14 Tri|25351|361472|515^737|HR737|25250|22672|525^738|HR738|25479|345425|583^739|75 Cet|25359|-10350|535^740|σ Cet|25348|-152447|475^741|29 Ari|25484|150347|604^742|HR742|25374|-364275|630^743|HR743|26339|728183|516^744|λ¹ For|25519|-346500|590^745|HR745|25612|-200019|621^746|HR746|25911|396644|636^747|HR747|26267|657456|578^748|HR748|25941|373122|571^749|ω For|25641|-282325|490^750|15 Tri|25963|346875|535^751|HR751|25845|74714|618^752|77 Cet|25785|-78594|575^753|HR753|26014|68869|582^754|ν Cet|25979|55933|486^755|HR755|25652|-510936|624^756|HR756|26159|387328|590^757|HR757|26119|316075|610^758|HR758|26174|342639|530^759|80 Cet|26000|-78317|553^760|HR760|26224|398958|654^761|HR761|26184|328919|625^762|HR762|25593|-625869|677^763|31 Ari|26105|124475|568^764|30 Ari|26160|246483|709^765|30 Ari|26168|246475|650^766|HR766|26098|77297|581^767|ι¹ For|26026|-300447|575^768|HR768|26383|377267|618^769|HR769|26411|380894|630^770|HR770|26336|76953|639^771|81 Cet|26283|-33961|565^772|λ² For|26163|-345783|579^773|ν Ari|26469|219614|543^774|HR774|27966|814483|578^775|HR775|26436|34431|621^776|μ Hyi|25279|-791094|528^777|ι² For|26385|-301942|583^778|η Hor|26234|-525431|531^779|δ Cet|26581|3286|407^780|HR780|26402|-379906|649^781|ε Cet|26594|-118722|484^782|33 Ari|26781|270608|530^783|HR783|26710|61119|625^784|HR784|26701|-94531|578^785|11 Per|27174|551058|577^786|HR786|26674|-306339|652^787|HR787|27166|535261|584^788|12 Per|27041|401939|491^789|HR789|26633|-428917|475^790|84 Cet|26872|-6958|571^791|HR791|27471|678247|595^792|HR792|27172|482656|648^793|μ Ari|27061|200117|569^794|ι Eri|26778|-398556|411^795|HR795|26968|-32133|605^796|HR796|26928|-145494|598^797|HR797|27080|107417|630^798|HR798|26588|-642819|655^799|θ Per|27367|492283|412^800|14 Per|27348|442969|543^801|35 Ari|27242|277072|466^802|ζ Hor|26777|-545500|521^803|HR803|27309|256381|635^804|Kaffaljidhma|27217|32358|347^805|HR805|27018|-383839|601^806|ε Hyi|26598|-682669|411^807|HR807|27024|-465244|610^808|36 Ari|27386|177639|646^809|ο Ari|27425|153117|577^810|ι Hor|27093|-508003|541^811|π Cet|27354|-138586|425^812|38 Ari|27493|124458|518^813|μ Cet|27490|101142|427^814|HR814|27223|-405275|636^815|HR815|28154|696342|618^816|HR816|27558|47117|603^817|HR817|27390|-325250|622^818|τ¹ Eri|27517|-185725|447^819|HR819|27829|359836|625^820|HR820|27843|355550|630^821|HR821|27363|-525706|615^822|HR822|27546|-462872|685^823|HR823|27241|-667144|626^824|39 Ari|27985|292472|451^825|HR825|28252|570842|625^826|HR826|27792|-216397|649^827|HR827|27864|-224856|647^828|40 Ari|28089|182836|582^829|HR829|28663|688886|580^830|HR830|28128|251881|586^831|HR831|282)BSC5";
static const char BSC5_CHUNK_8[] PROGMEM = R"BSC5(42|373261|645^832|HR832|27989|-124606|690^833|γ Hor|27576|-637044|574^834|Miram|28449|558956|376^835|η¹ For|27927|-355508|651^836|π Ari|28215|174642|522^837|ζ Hyi|27591|-676167|484^838|41 Ari|28331|272606|363^839|HR839|28626|583147|645^840|16 Per|28431|383186|423^841|β For|28182|-324058|446^842|HR842|28616|468419|588^843|17 Per|28586|350597|453^844|γ¹ For|28308|-245603|614^845|γ² For|28317|-279417|539^846|HR846|28811|529978|636^847|σ Ari|28582|150819|549^848|η² For|28374|-358436|592^849|HR849|28892|485694|626^850|Angetenar|28506|-210042|475^851|η³ For|28446|-356761|547^852|ν Hor|28171|-628067|526^853|HR853|28466|-399317|636^854|τ Per|29043|527625|395^855|20 Per|28952|383375|533^856|HR856|28866|164833|631^857|HR857|28756|-127694|604^858|HR858|28654|-308144|640^859|HR859|28807|-94411|632^860|HR860|29325|615211|559^861|HR861|29402|643325|624^862|HR862|28931|-223764|595^863|ψ For|28929|-384369|592^864|HR864|29474|512608|622^865|HR865|29426|471639|602^866|HR866|28720|-629097|603^867|ρ² Ari|29301|183317|591^868|HR868|28980|-498903|400^869|ρ³ Ari|29406|180231|563^870|HR870|29372|83817|597^871|HR871|29018|-508714|621^872|ν Hyi|28412|-750669|475^873|21 Per|29548|319342|511^874|Azha|29405|-88981|389^875|HR875|29437|-37122|517^876|HR876|29673|386150|604^877|HR877|29513|45011|611^878|47 Ari|29681|206686|580^879|π Per|29794|396628|470^880|HR880|29058|-644356|656^881|HR881|31022|794186|549^882|24 Per|29844|351831|493^883|4 Eri|29566|-238619|545^884|HR884|29536|-298553|629^885|HR885|29972|472208|547^886|HR886|29944|410331|589^887|ε Ari|29869|213403|463^888|ε Ari|29869|213403|463^889|6 Eri|29683|-236061|584^890|HR890|30145|523517|528^891|HR891|30148|523522|674^892|HR892|29783|-27825|523^893|HR893|29591|-381911|641^894|HR894|30033|381317|611^895|HR895|29798|-97764|614^896|λ Cet|29952|89075|470^897|Acamar|29710|-403047|324^898|θ² Eri|29712|-403044|435^899|5 Eri|29948|-24650|556^900|HR900|29852|-289069|614^901|ζ For|29934|-252742|571^902|HR902|30122|108703|595^903|HR903|29940|-325072|631^904|7 Eri|30142|-28786|611^905|49 Ari|30317|264622|590^906|HR906|31952|814706|595^907|ρ¹ Eri|30194|-76628|575^908|HR908|30312|53361|625^909|β Hor|29799|-640714|499^910|93 Cet|30396|43528|561^911|Menkar|30380|40897|253^912|HR912|30322|-99614|583^913|HR913|30359|-64947|619^914|ε For|30271|-280917|589^915|γ Per|30799|535064|293^916|HR916|30584|282697|636^917|ρ² Eri|30451|-76853|532^918|HR918|30923|567058|476^919|τ³ Eri|30399|-236244|409^920|HR920|30944|560686|611^921|Gorgonea Tertia|30863|388403|339^922|HR922|31219|640578|589^923|HR923|30891|405825|605^924|HR924|30780|158561|649^925|ρ³ Eri|30712|-76008|526^926|HR926|30773|18636|605^927|52 Ari|30908|252553|680^928|52 Ari|30908|252553|700^929|HR929|30489|-469750|582^930|HR930|31344|522133|631^931|HR931|31066|131872|562^932|HR932|31990|74393)BSC5";
static const char BSC5_CHUNK_9[] PROGMEM = R"BSC5(6|487^933|HR933|31298|473083|641^934|μ Hor|30602|-597378|511^935|HR935|31093|-60886|527^936|Algol|31361|409556|212^937|ι Per|31511|496133|405^938|53 Ari|31238|178800|611^939|θ Hyi|30376|-719025|553^940|54 Ari|31392|187950|627^941|Misam|31583|448572|380^942|HR942|31441|84708|628^943|HR943|31308|-278311|619^944|55 Ari|31602|290769|572^945|HR945|31691|278200|642^946|HR946|31742|268964|602^947|ω Per|31882|396117|463^948|HR948|31775|118725|598^949|HR949|32073|477258|633^950|HR950|32027|423761|615^951|Botein|31938|197267|435^952|HR952|31894|130478|612^953|HR953|31765|-237383|638^954|56 Ari|32039|272569|579^955|HR955|31886|-38117|605^956|HR956|32233|481769|590^957|HR957|31880|-160253|626^958|HR958|32073|66608|556^959|HR959|31303|-692656|615^960|HR960|31743|-487342|612^961|HR961|33388|777347|545^962|94 Cet|32129|-11961|506^963|Fornacis|32012|-289869|387^964|HR964|32633|571408|579^965|HR965|35389|849111|561^966|HR966|32491|425039|607^967|HR967|32921|656586|636^968|HR968|32072|-444197|593^969|HR969|32701|509378|503^970|HR970|32171|-359439|627^971|HR971|32557|305567|552^972|ζ Ari|32484|210444|489^973|HR973|32680|453458|616^974|HR974|32272|-298042|616^975|HR975|32631|328564|631^976|HR976|32672|346886|625^977|HR977|32092|-573217|574^978|HR978|32764|321836|606^979|HR979|32865|404833|645^980|HR980|32501|-261003|625^981|HR981|31256|-789894|557^982|30 Per|32965|440250|547^983|HR983|32669|-59186|617^984|Zibal|32639|-88197|480^985|HR985|33332|656522|484^986|HR986|32961|392833|596^987|29 Per|33105|502222|515^988|14 Eri|32766|-91544|614^989|31 Per|33188|500950|503^990|HR990|32698|-308275|665^991|HR991|33122|342228|482^992|95 Cet|33062|-9303|538^993|HR993|33008|-287969|591^994|15 Eri|33061|-225114|488^995|59 Ari|33322|270711|590^996|κ¹ Cet|33227|33703|483^997|HR997|33114|-185597|571^998|HR998|32907|-477517|585^999|HR999|33390|290483|447^1000|60 Ari|33404|256628|612^1001|HR1001|33646|490708|593^1002|32 Per|33574|433294|495^1003|τ⁴ Eri|33253|-217578|369^1004|HR1004|33263|-241231|561^1005|τ¹ Ari|33538|211469|528^1006|ζ¹ Ret|32962|-625753|554^1007|κ² Cet|33519|36756|569^1008|HR1008|33321|-430697|427^1009|HR1009|34112|645861|523^1010|ζ² Ret|33036|-625064|524^1011|HR1011|33870|492133|529^1012|62 Ari|33700|276075|552^1013|HR1013|33459|-266064|639^1014|HR1014|32997|-669269|605^1015|τ² Ari|33792|207419|509^1016|HR1016|33567|-236353|552^1017|Mirfak|34054|498611|179^1018|HR1018|33712|-255878|635^1019|HR1019|34082|335358|561^1020|HR1020|34301|539217|651^1021|HR1021|33592|-477769|639^1022|64 Ari|34051|247242|550^1023|HR1023|33942|48819|638^1024|HR1024|33883|-77942|620^1025|ι Hyi|32660|-773883|552^1026|HR1026|34193|412572|651^1027|65 Ari|34072|208036|608^1028|HR1028|34028|126294|604^1029|HR1029|34326|491208|609^1030|ο Tau|34136|90289|360^1031|HR1031|33957|-327072|650^1032|HR1032|35054|)BSC5";
static const char BSC5_CHUNK_10[] PROGMEM = R"BSC5(718639|632^1033|HR1033|34732|602556|649^1034|HR1034|34675|490628|498^1035|HR1035|34845|599403|421^1036|HR1036|34509|187564|657^1037|HR1037|34812|498483|558^1038|ξ Tau|34528|97328|374^1039|HR1039|34552|127350|628^1040|HR1040|34986|588786|454^1041|HR1041|34724|338075|561^1042|χ¹ For|34322|-359208|639^1043|HR1043|35031|593661|613^1044|34 Per|34895|495089|467^1045|HR1045|34396|-273175|593^1046|HR1046|35001|554519|509^1047|HR1047|34906|469378|624^1048|66 Ari|34741|228042|603^1049|HR1049|34366|-416369|632^1050|HR1050|34669|-112867|573^1051|HR1051|35103|481036|582^1052|σ Per|35096|479953|436^1053|HR1053|34007|-696247|615^1054|χ² For|34593|-356814|571^1055|HR1055|35868|733469|657^1056|HR1056|35248|492097|629^1058|χ³ For|34699|-358533|650^1059|HR1059|35303|494008|639^1060|HR1060|34942|-68050|599^1061|4 Tau|35068|113364|514^1062|HR1062|34933|-126747|559^1063|HR1063|35357|480236|547^1064|HR1064|34267|-693364|596^1065|HR1065|35224|275719|596^1066|5 Tau|35146|129367|411^1067|HR1067|35126|61886|594^1068|HR1068|35589|587650|640^1069|36 Per|35406|460569|531^1070|17 Eri|35103|-50753|473^1071|HR1071|35614|578689|637^1072|HR1072|35442|448556|641^1073|HR1073|35608|549747|598^1074|HR1074|35444|354617|590^1075|HR1075|34986|-426342|578^1076|HR1076|35038|-413700|612^1077|HR1077|35836|600411|646^1078|HR1078|35598|398994|581^1079|6 Tau|35433|93736|577^1080|HR1080|36569|757397|627^1081|HR1081|35103|-473753|599^1082|HR1082|35316|-256142|638^1083|κ Ret|34896|-629375|472^1084|Ran|35488|-94583|373^1085|HR1085|35690|178328|617^1086|7 Tau|35741|244644|592^1087|ψ Per|36082|481928|423^1088|τ⁵ Eri|35631|-216328|427^1089|HR1089|35803|64178|649^1090|HR1090|35430|-503786|568^1091|HR1091|35771|-98686|625^1092|HR1092|35143|-664897|583^1093|HR1093|35658|-310803|620^1094|HR1094|36388|569328|630^1095|HR1095|35760|-318747|640^1096|HR1096|35477|-610169|641^1097|HR1097|36334|425831|642^1098|HR1098|35994|-111936|557^1099|HR1099|36131|5878|571^1100|20 Eri|36048|-174669|523^1101|10 Tau|36146|4017|428^1102|HR1102|36299|154308|639^1103|HR1103|36500|209158|650^1104|HR1104|35735|-657644|675^1105|HR1105|37026|632167|510^1106|HR1106|36182|-402747|458^1107|HR1107|41671|866261|586^1108|HR1108|36414|-73917|585^1109|HR1109|34997|-783519|570^1110|HR1110|36571|165367|616^1111|21 Eri|36503|-56261|596^1112|HR1112|37119|599694|576^1113|HR1113|36855|375800|557^1114|τ For|36466|-279431|601^1115|12 Tau|36642|30569|557^1116|HR1116|36606|-33931|623^1117|HR1117|36571|-104372|619^1118|11 Tau|36795|253294|611^1119|HR1119|36665|-11206|612^1120|HR1120|36698|-152267|633^1121|22 Eri|36773|-52106|553^1122|δ Per|37154|477875|301^1123|40 Per|37062|339650|497^1124|HR1124|37669|672017|580^1125|HR1125|36872|-118031|649^1126|13 Tau|37053|197003|569^1127|HR1127|37351|485236|606^1128|HR1128|36896|-195847|659^1129|HR1129|37673|633450|4)BSC5";
static const char BSC5_CHUNK_11[] PROGMEM = R"BSC5(80^1130|HR1130|37447|460997|611^1131|Atik|37386|322883|383^1132|14 Tau|37298|196650|614^1133|HR1133|37421|364600|559^1134|δ For|37041|-319383|500^1135|ν Per|37532|425786|377^1136|Rana|37208|-97633|354^1137|HR1137|37411|209286|610^1138|HR1138|38205|708711|544^1139|HR1139|37261|-104856|560^1140|Celaeno|37467|242894|546^1141|HR1141|37665|456819|566^1142|Electra|37479|241133|370^1143|HR1143|37139|-373136|459^1144|18 Tau|37527|248392|564^1145|Taygeta|37535|244672|430^1146|24 Eri|37418|-11631|525^1147|HR1147|37923|559225|610^1148|γ Cam|38393|713322|463^1149|Maia|37638|243678|387^1150|25 Eri|37490|-2967|555^1151|Asterope|37651|245547|576^1152|Sterope|37675|245281|643^1153|29 Tau|37612|60500|535^1154|HR1154|36083|-783231|629^1155|HR1155|38253|655261|447^1156|Merope|37721|239483|418^1157|HR1157|37351|-406603|645^1158|HR1158|38269|632972|585^1159|HR1159|37693|68031|591^1160|HR1160|38051|507367|614^1161|HR1161|38221|571181|646^1162|π Eri|37690|-121017|442^1163|HR1163|37979|336000|657^1164|HR1164|37969|321950|625^1165|Alcyone|37914|241050|287^1166|HR1166|38616|685075|632^1167|HR1167|37474|-480614|649^1168|HR1168|37427|-542739|630^1169|HR1169|37544|-473597|573^1170|HR1170|38189|439631|602^1171|σ For|37743|-293381|590^1172|HR1172|38058|234211|545^1173|τ⁶ Eri|37808|-232497|423^1174|30 Tau|38045|111433|507^1175|β Ret|37367|-648069|385^1176|HR1176|38346|449678|566^1177|42 Per|38257|330914|511^1178|Atlas|38194|240533|363^1179|HR1179|37889|-299019|655^1180|Pleione|38198|241367|509^1181|τ⁷ Eri|37943|-238747|524^1182|HR1182|38108|2278|591^1183|HR1183|38288|237117|617^1184|ρ For|37989|-301678|554^1185|HR1185|38319|222444|607^1186|HR1186|37971|-361058|621^1187|HR1187|38099|-209031|581^1188|HR1188|38386|255794|526^1189|HR1189|38098|-376222|540^1190|HR1190|38100|-376206|473^1191|HR1191|38649|343592|577^1192|HR1192|38954|579750|580^1193|HR1193|38602|220317|683^1194|HR1194|38544|130458|630^1195|HR1195|38242|-362003|417^1196|HR1196|39417|718217|634^1197|HR1197|38679|311683|625^1198|HR1198|38941|486506|576^1199|31 Tau|38667|65347|567^1200|HR1200|38438|-364253|686^1201|HR1201|38861|173269|597^1202|30 Eri|38782|-53614|548^1203|ζ Per|39022|318836|285^1204|HR1204|39571|630722|503^1205|HR1205|39523|611089|500^1206|HR1206|38869|-184344|622^1207|HR1207|39328|478714|537^1208|γ Hyi|37873|-742389|324^1209|HR1209|39231|310458|610^1210|43 Per|39435|506953|528^1211|32 Eri|39048|-29528|614^1212|32 Eri|39049|-29547|479^1213|τ⁸ Eri|38952|-246125|465^1214|HR1214|38941|-347322|511^1215|HR1215|39413|350811|549^1216|HR1216|38926|-468936|593^1217|HR1217|39212|-120992|600^1218|32 Tau|39478|224781|563^1219|HR1219|39064|-403572|571^1220|ε Per|39642|400103|289^1221|33 Tau|39511|231756|606^1222|HR1222|39573|244619|616^1223|HR1223|39675|348144|653^1224|HR1224|39505|60400|609^1225|HR1225|39439|-97508|619^1)BSC5";
static const char BSC5_CHUNK_12[] PROGMEM = R"BSC5(226|HR1226|39748|388403|630^1227|HR1227|39094|-526906|646^1228|Menkib|39828|357911|404^1229|HR1229|39944|388206|638^1230|HR1230|41674|806986|510^1231|Zaurak|39672|-135086|295^1232|HR1232|39812|-54700|583^1233|HR1233|39946|103308|637^1234|HR1234|40207|369900|641^1235|HR1235|39917|-125742|560^1236|HR1236|39344|-634636|614^1237|HR1237|40102|172967|632^1238|HR1238|40136|181939|589^1239|λ Tau|40113|124903|347^1240|τ⁹ Eri|39988|-240164|466^1241|HR1241|41009|686800|587^1242|HR1242|40742|591556|506^1243|HR1243|40295|99978|567^1244|35 Eri|40256|-15497|528^1245|HR1245|39786|-571025|605^1246|HR1246|40113|-304908|593^1247|δ Ret|39791|-614003|456^1248|HR1248|41108|655208|617^1249|HR1249|40435|-2689|538^1250|HR1250|40044|-515647|651^1251|ν Tau|40526|59892|391^1252|36 Tau|40727|241058|547^1253|40 Tau|40624|54356|533^1254|HR1254|40657|81972|546^1255|HR1255|41102|540086|631^1256|37 Tau|40782|220819|436^1257|HR1257|40694|28269|536^1258|HR1258|40569|-201442|646^1259|HR1259|40602|-201583|701^1260|HR1260|41309|623300|699^1261|λ Per|41097|503514|429^1262|39 Tau|40889|220089|590^1263|HR1263|40691|-165889|639^1264|γ Ret|40149|-621594|451^1265|HR1265|40730|-127925|561^1266|ι Ret|40217|-610789|497^1267|HR1267|40781|-203817|613^1268|41 Tau|41101|276000|520^1269|ψ Tau|41168|290014|523^1270|HR1270|41577|599081|628^1271|HR1271|37089|-852622|641^1272|HR1272|40990|-88561|626^1273|48 Per|41444|477125|404^1274|HR1274|40963|-205122|634^1275|HR1275|40937|-276517|559^1276|HR1276|41562|548289|618^1277|49 Per|41376|377278|609^1278|50 Per|41435|380397|551^1279|HR1279|41283|151628|601^1280|HR1280|41332|173397|589^1281|HR1281|42291|721264|603^1282|HR1282|42143|685017|632^1283|ω¹ Tau|41528|196092|550^1284|HR1284|41504|133983|595^1285|HR1285|41236|-429169|659^1286|HR1286|41831|335867|572^1287|44 Tau|41805|264808|541^1288|HR1288|41549|-163858|537^1289|HR1289|44703|838078|557^1290|37 Eri|41729|-69239|544^1291|HR1291|41428|-458647|659^1292|45 Tau|41890|55231|572^1293|HR1293|41799|-88197|570^1294|HR1294|41227|-642225|638^1295|HR1295|42087|172775|609^1296|HR1296|42505|574603|608^1297|HR1297|42142|224136|612^1298|Beid|41978|-68375|404^1299|HR1299|41794|-352739|644^1300|HR1300|41934|-203561|579^1301|HR1301|42332|379669|645^1302|δ Hor|41807|-419936|493^1303|μ Per|42483|484094|414^1304|HR1304|45000|833406|546^1305|HR1305|42816|618503|570^1306|52 Per|42481|404836|471^1307|HR1307|42263|102122|623^1308|HR1308|42254|88906|651^1309|46 Tau|42259|77161|529^1310|HR1310|42305|127533|625^1311|47 Tau|42323|92636|484^1312|HR1312|42273|-11497|644^1313|HR1313|42856|578606|571^1314|HR1314|42786|536119|519^1315|HR1315|42434|100111|522^1316|HR1316|42088|-443683|671^1317|HR1317|44508|808242|543^1318|39 Eri|42399|-102564|487^1319|48 Tau|42629|154006|632^1320|μ Tau|42589|88922|429^1321|HR1321|42572|61997|693^1322|HR1322|4)BSC5";
static const char BSC5_CHUNK_13[] PROGMEM = R"BSC5(2581|61867|631^1323|HR1323|42266|-403578|637^1324|HR1324|43041|502956|461^1325|Keid|42545|-76528|443^1326|α Hor|42334|-422944|386^1327|HR1327|43445|651406|527^1328|HR1328|43023|421411|622^1329|ω² Tau|42877|205786|494^1330|HR1330|43204|500489|545^1331|51 Tau|43064|215792|565^1332|HR1332|42887|-64719|594^1333|HR1333|43365|509208|555^1334|HR1334|43068|94867|654^1335|HR1335|43632|607356|539^1336|α Ret|42404|-624739|335^1337|HR1337|43373|418081|592^1338|γ Dor|42671|-514867|425^1339|53 Tau|43239|211422|535^1340|HR1340|42469|-621919|545^1341|56 Tau|43269|217736|538^1342|HR1342|43644|565067|588^1343|54 Per|43402|345667|493^1344|HR1344|43361|319533|616^1345|HR1345|43044|-207156|600^1346|Hyadum I|43299|156275|365^1347|υ⁴ Eri|42982|-337983|356^1348|φ Tau|43392|273508|495^1349|HR1349|43271|101214|631^1350|53 Per|43592|464989|485^1351|57 Tau|43327|140353|559^1352|HR1352|43828|596164|619^1353|HR1353|43104|-229703|607^1354|HR1354|43403|187425|612^1355|ε Ret|42747|-593019|444^1356|58 Tau|43434|150953|526^1357|HR1357|42725|-609486|637^1358|HR1358|43480|138642|617^1359|HR1359|43175|-339050|637^1360|HR1360|43448|61308|577^1361|HR1361|43469|92256|653^1362|HR1362|43441|-62456|627^1363|HR1363|43452|-75925|585^1364|HR1364|43213|-442681|534^1365|HR1365|43111|-528600|609^1366|HR1366|43575|-981|586^1367|HR1367|43442|-206397|538^1368|60 Tau|43676|140772|572^1369|χ Tau|43764|256292|537^1370|HR1370|43730|208214|591^1371|HR1371|43933|424281|623^1372|θ Ret|42945|-632556|587^1373|Hyadum II|43822|175425|376^1374|HR1374|43587|-257283|601^1375|HR1375|43923|209822|599^1376|63 Tau|43903|167772|564^1377|55 Per|44081|341306|573^1378|62 Tau|43999|243011|636^1379|56 Per|44104|339597|576^1380|δ² Tau|44016|174439|480^1381|66 Tau|43978|94608|512^1382|HR1382|44503|575853|632^1383|ξ Eri|43947|-37456|517^1384|HR1384|43849|-248922|583^1385|HR1385|44159|190417|598^1386|HR1386|43855|-355450|639^1387|κ¹ Tau|44228|222939|422^1388|κ² Tau|44236|221997|528^1389|δ³ Tau|44248|179281|429^1390|HR1390|44351|314389|528^1391|70 Tau|44270|159408|646^1392|υ Tau|44385|228136|428^1393|43 Eri|44006|-340169|396^1394|71 Tau|44391|156183|449^1395|η Ret|43648|-633864|524^1396|π Tau|44434|147136|469^1397|HR1397|44392|85903|606^1398|HR1398|44157|-347578|655^1399|72 Tau|44549|229964|553^1400|HR1400|44502|20794|623^1401|HR1401|45585|725286|594^1402|HR1402|44580|112125|588^1403|HR1403|44669|216200|572^1404|HR1404|44220|-441608|639^1405|HR1405|44034|-570714|629^1406|HR1406|44811|303614|640^1407|75 Tau|44740|163597|497^1408|76 Tau|44732|147408|590^1409|Ain|44769|191803|353^1410|HR1410|44492|-240814|611^1411|θ¹ Tau|44762|159622|384^1412|θ² Tau|44777|158708|340^1413|HR1413|44677|18586|615^1414|79 Tau|44806|130475|503^1415|HR1415|44756|13808|555^1416|HR1416|44181|-612383|594^1417|1 Cam|45338|539108|577^1418|HR1418|44517|-469475|6)BSC5";
static const char BSC5_CHUNK_14[] PROGMEM = R"BSC5(10^1419|HR1419|45106|324581|621^1420|HR1420|44953|105217|679^1421|HR1421|44775|-194583|596^1422|80 Tau|45024|156381|558^1423|HR1423|44852|-130483|560^1424|HR1424|45233|400103|626^1425|HR1425|45007|102625|648^1426|δ Men|42997|-802139|569^1427|HR1427|45094|161939|478^1428|81 Tau|45108|156919|548^1429|HR1429|44693|-419600|644^1430|83 Tau|45104|137244|540^1431|HR1431|45027|-135922|624^1432|85 Tau|45311|158517|602^1433|HR1433|44889|-465153|616^1434|57 Per|45569|430639|609^1435|HR1435|44628|-625208|575^1436|HR1436|45347|54100|639^1437|45 Eri|45313|-439|491^1438|HR1438|45239|-136450|621^1439|HR1439|45112|-356536|596^1440|HR1440|46067|642617|594^1441|HR1441|45437|-32092|581^1442|HR1442|45591|180167|625^1443|δ Cae|45139|-449539|507^1444|ρ Tau|45641|148444|465^1445|HR1445|45772|289611|588^1446|HR1446|45634|94131|601^1447|HR1447|45561|-107858|606^1448|HR1448|45690|55686|568^1449|46 Eri|45652|-67389|572^1450|HR1450|45706|-68378|609^1451|47 Eri|45699|-82314|511^1452|HR1452|45699|-89703|526^1453|υ¹ Eri|45585|-297667|451^1454|58 Per|46115|412647|425^1455|HR1455|45952|198817|636^1456|ν Men|43494|-815800|579^1457|Aldebaran|45987|165092|85^1458|88 Tau|45942|101608|425^1459|HR1459|46081|233408|602^1460|HR1460|45872|-97367|637^1461|HR1461|45835|-199206|613^1462|HR1462|46004|-36119|633^1463|ν Eri|46053|-33525|393^1464|Theemin|45925|-305622|382^1465|α Dor|45666|-550450|327^1466|2 Cam|46661|534731|535^1467|3 Cam|46652|530797|505^1468|HR1468|47668|766111|649^1469|HR1469|46205|9983|531^1470|HR1470|46416|269400|647^1471|HR1471|46377|206847|592^1472|89 Tau|46359|160333|579^1473|90 Tau|46360|125108|427^1474|51 Eri|46267|-24733|523^1475|HR1475|45594|-628236|579^1476|HR1476|46141|-307167|630^1477|HR1477|46564|252183|622^1478|σ¹ Tau|46526|157997|507^1479|σ² Tau|46546|159181|469^1480|HR1480|46517|78708|539^1481|Sceptrum|46363|-143039|387^1482|HR1482|46900|483008|567^1483|HR1483|46482|-121231|501^1484|93 Tau|46676|121978|546^1485|HR1485|43808|-828992|676^1486|HR1486|47217|595208|650^1487|HR1487|46555|-143592|545^1488|HR1488|46631|-10528|610^1489|HR1489|46973|382803|599^1490|HR1490|46888|286150|578^1491|HR1491|48140|759411|606^1492|HR1492|46127|-620775|540^1493|HR1493|47227|499739|587^1494|59 Per|47151|433650|529^1495|HR1495|46686|-244825|558^1496|54 Eri|46740|-196717|432^1497|τ Tau|47041|229569|428^1498|HR1498|46512|-516728|644^1499|95 Tau|47205|240889|613^1500|HR1500|47369|407869|608^1501|HR1501|47301|328653|645^1502|α Cae|46760|-418639|445^1503|β Cae|47010|-371444|505^1504|HR1504|46718|-589436|653^1505|55 Eri|47263|-87936|682^1506|55 Eri|47264|-87961|670^1507|HR1507|47405|111461|540^1508|56 Eri|47348|-85036|590^1509|HR1509|47192|-307656|568^1510|HR1510|48434|709417|637^1511|4 Cam|48001|567572|530^1512|HR1512|47618|236281|635^1513|HR1513|47356|-186667|553^1514|HR1514|47790|403128|5)BSC5";
static const char BSC5_CHUNK_15[] PROGMEM = R"BSC5(97^1515|HR1515|48019|556025|626^1516|λ Pic|47129|-504814|531^1517|HR1517|47713|187350|601^1518|HR1518|47289|-410647|625^1519|HR1519|47671|117056|537^1520|μ Eri|47584|-32547|402^1521|HR1521|47512|-212836|572^1522|HR1522|47734|-29544|633^1523|HR1523|50058|811939|507^1524|HR1524|47638|-340050|686^1525|HR1525|47738|-280875|619^1526|HR1526|47654|-393567|605^1527|HR1527|48681|635053|544^1528|HR1528|48219|325883|586^1529|HR1529|48202|314372|558^1530|κ Dor|47392|-597328|527^1531|HR1531|46394|-776561|605^1532|58 Eri|47934|-169344|551^1533|HR1533|48318|374883|488^1534|HR1534|48124|35883|603^1535|HR1535|48526|487408|566^1536|HR1536|48101|-56739|578^1537|96 Tau|48289|159042|608^1538|59 Eri|48090|-163294|577^1539|ζ Cae|47971|-300203|637^1540|HR1540|47494|-632297|646^1541|μ Men|47178|-709311|554^1542|α Cam|49008|663428|429^1543|Tabit|48307|69614|319^1544|π² Ori|48435|89003|436^1545|HR1545|48284|-137697|626^1546|HR1546|48861|528408|641^1547|97 Tau|48562|188397|510^1548|HR1548|48094|-439800|672^1549|60 Eri|48366|-162172|503^1550|HR1550|48799|425867|571^1551|2 Aur|48772|367031|478^1552|π⁴ Ori|48534|56050|369^1553|HR1553|48621|99750|611^1554|HR1554|48798|278975|597^1555|5 Cam|49175|552592|552^1556|ο¹ Ori|48756|142506|474^1557|HR1557|48378|-413208|607^1558|HR1558|49142|440608|608^1559|HR1559|48578|-349064|586^1560|ω Eri|48816|-54528|439^1561|HR1561|49353|528694|575^1562|5 Ori|48897|25081|533^1563|ι Pic|48487|-534614|561^1564|ι Pic|48490|-534597|642^1565|HR1565|48988|15694|661^1566|HR1566|49162|194853|637^1567|π⁵ Ori|49042|24406|372^1568|7 Cam|49548|537522|447^1569|6 Ori|49130|114261|519^1570|π¹ Ori|49149|101508|465^1571|HR1571|49133|77792|533^1572|HR1572|50389|742692|606^1573|HR1573|49389|361689|607^1574|HR1574|49141|4675|599^1575|HR1575|49377|245922|637^1576|HR1576|49306|150400|581^1577|Kabdhilinan|49499|331661|269^1578|HR1578|49329|53992|650^1579|HR1579|49186|-167406|570^1580|ο² Ori|49395|135144|407^1581|HR1581|49218|-164178|572^1582|62 Eri|49401|-51714|551^1583|HR1583|49251|-257278|672^1584|HR1584|49152|-396286|610^1585|HR1585|49562|171536|548^1586|99 Tau|49635|239486|579^1587|HR1587|50703|737639|666^1588|8 Cam|49962|531556|608^1589|HR1589|50777|740669|596^1590|98 Tau|49693|250503|581^1591|HR1591|49548|-10672|623^1592|ω Aur|49876|378903|494^1593|HR1593|50266|610781|603^1594|HR1594|50473|668228|619^1595|HR1595|49624|-142314|615^1596|HR1596|49697|-22125|635^1597|HR1597|49147|-585472|612^1598|HR1598|48918|-666756|641^1599|5 Aur|50051|393947|595^1600|HR1600|49832|145428|609^1601|π⁶ Ori|49758|17142|447^1602|6 Aur|50064|396550|658^1603|β Cam|50570|604422|403^1604|HR1604|49837|-163758|566^1605|Haldus|50328|438233|299^1606|HR1606|48849|-724075|628^1607|HR1607|49935|-148058|771^1608|63 Eri|49973|-102633|538^1609|HR1609|50091|36153|703^1610|HR1610|50094|36161|666^1611|64 Eri|)BSC5";
static const char BSC5_CHUNK_16[] PROGMEM = R"BSC5(49988|-125375|479^1612|Haedus|50413|410758|375^1613|HR1613|50111|-20656|632^1614|HR1614|50136|-57533|622^1615|HR1615|50552|414417|614^1616|HR1616|55300|859386|651^1617|ψ Eri|50240|-71739|481^1618|HR1618|50306|7222|592^1619|HR1619|50333|16089|624^1620|ι Tau|50516|215900|464^1621|HR1621|50238|-200519|491^1622|11 Cam|51024|589725|508^1623|12 Cam|51034|590211|608^1624|HR1624|51082|611700|604^1625|HR1625|50460|-42097|585^1626|HR1626|50707|304947|614^1627|HR1627|50769|323203|662^1628|HR1628|50361|-262750|502^1629|η Men|49198|-749369|547^1630|HR1630|51061|544058|724^1631|HR1631|50262|-397181|603^1632|HR1632|50772|276961|660^1633|HR1633|50727|212781|619^1634|1 Lep|50458|-227950|575^1635|HR1635|50397|-317714|594^1636|HR1636|51602|696394|641^1637|9 Aur|51113|515978|500^1638|11 Ori|50761|154042|468^1639|HR1639|51002|359364|652^1640|HR1640|50644|-143694|641^1641|Hoedus II|51086|412344|317^1642|HR1642|50922|198064|644^1643|HR1643|52062|739467|543^1644|HR1644|51138|431747|620^1645|HR1645|50648|-243878|561^1646|HR1646|50818|-30397|605^1647|HR1647|51624|649194|641^1648|HR1648|50899|11775|617^1649|η¹ Pic|50468|-491514|538^1650|HR1650|52432|764728|637^1651|HR1651|50650|-417450|631^1652|γ¹ Cae|50734|-354833|455^1653|γ² Cae|50739|-357053|634^1654|ε Lep|50910|-223711|319^1655|HR1655|50878|-261525|573^1656|104 Tau|51242|186450|500^1657|66 Eri|51127|-46550|512^1658|106 Tau|51301|204183|530^1659|103 Tau|51352|242653|550^1660|105 Tau|51321|217047|589^1661|HR1661|51102|-131219|605^1662|13 Ori|51273|94719|617^1663|η² Pic|50828|-495778|503^1664|14 Ori|51314|84983|534^1665|HR1665|51236|-124906|597^1666|Cursa|51308|-50864|279^1667|HR1667|50835|-544075|627^1668|HR1668|51786|469622|568^1669|HR1669|51719|373019|602^1670|HR1670|51625|280306|601^1671|HR1671|51389|-86650|578^1672|16 Ori|51554|98294|543^1673|68 Eri|51454|-44561|512^1674|ζ Dor|50918|-574728|472^1675|HR1675|52176|618500|617^1676|15 Ori|51617|155972|482^1677|β Men|50453|-713144|531^1678|14 Cam|52254|626914|650^1679|λ Eri|51524|-87542|427^1680|HR1680|51374|-357183|652^1681|HR1681|51676|-5653|610^1682|HR1682|50037|-783003|629^1683|HR1683|53037|732681|574^1684|HR1684|51949|160456|518^1685|HR1685|51828|-22539|625^1686|HR1686|53760|792311|505^1687|HR1687|51887|-24908|590^1688|HR1688|52531|594056|615^1689|μ Aur|52238|384844|486^1690|HR1690|51948|5147|667^1691|HR1691|51959|10369|589^1692|HR1692|52456|532139|620^1693|HR1693|51897|-118492|568^1694|HR1694|51790|-259094|641^1695|HR1695|51261|-633997|520^1696|ι Lep|52050|-118692|445^1697|HR1697|52134|-60572|591^1698|ρ Ori|52215|28611|446^1699|HR1699|51933|-373953|657^1700|HR1700|51026|-730378|627^1701|HR1701|52254|19681|609^1702|μ Lep|52155|-162056|331^1703|HR1703|52298|5603|632^1704|HR1704|52259|-81478|637^1705|κ Lep|52205|-129414|436^1706|14 Aur|52568|326878|502^1707|HR1707|52883|53)BSC5";
static const char BSC5_CHUNK_17[] PROGMEM = R"BSC5(5861|650^1708|Capella|52782|459981|8^1709|HR1709|52456|51561|550^1710|HR1710|52333|-146067|621^1711|108 Tau|52577|222847|627^1712|HR1712|52717|343119|596^1713|Rigel|52423|-82017|12^1714|HR1714|57302|856681|660^1715|HR1715|52296|-358256|698^1716|ξ Men|49808|-824706|585^1717|HR1717|52551|-14092|615^1718|18 Ori|52678|113414|556^1719|15 Cam|53244|581172|613^1720|HR1720|53396|626536|561^1721|HR1721|52413|-359772|576^1722|HR1722|53044|427922|548^1723|HR1723|52568|-269433|507^1724|HR1724|52781|19472|642^1725|HR1725|53112|404650|618^1726|16 Aur|53030|333717|454^1727|HR1727|52315|-520311|605^1728|17 Aur|53052|337672|614^1729|λ Aur|53190|400992|471^1730|HR1730|52631|-349267|666^1731|HR1731|52800|-171417|656^1732|HR1732|53167|337483|541^1733|HR1733|53340|444256|662^1734|18 Aur|53232|339856|649^1735|τ Ori|52934|-68444|360^1736|HR1736|53442|469639|654^1737|HR1737|52945|-135197|550^1738|HR1738|53374|410861|552^1739|109 Tau|53213|220964|494^1740|19 Aur|53336|339581|503^1741|HR1741|53207|201347|608^1742|HR1742|52608|-521819|649^1743|ο Col|52914|-348953|483^1744|θ Dor|52293|-671853|483^1745|HR1745|54905|779775|656^1746|21 Ori|53198|25958|534^1747|HR1747|53140|-181300|596^1748|HR1748|53264|-14122|634^1749|ρ Aur|53634|418044|523^1750|HR1750|53498|279572|633^1751|16 Cam|53911|575444|528^1752|HR1752|53535|295700|576^1753|HR1753|53215|-185200|636^1754|HR1754|53218|-185097|654^1755|HR1755|53491|198142|618^1756|λ Lep|53262|-131767|429^1757|ν Lep|53331|-123156|530^1758|HR1758|53232|-273689|599^1759|HR1759|53407|-53667|639^1760|HR1760|53806|410294|554^1761|HR1761|53554|40119|657^1762|HR1762|53408|-212394|471^1763|HR1763|53621|84286|580^1764|HR1764|53588|-4164|568^1765|22 Ori|53627|-3825|473^1766|HR1766|53391|-346989|634^1767|ζ Pic|53228|-506061|545^1768|22 Aur|53897|289367|646^1769|HR1769|53642|-137561|656^1770|23 Ori|53806|35444|500^1771|HR1771|53628|-247731|506^1772|HR1772|53547|-343453|609^1773|σ Aur|54109|373856|499^1774|110 Tau|53938|166994|608^1775|HR1775|54106|312239|628^1776|HR1776|54107|311431|594^1777|HR1777|53920|53225|635^1778|HR1778|53885|-84158|590^1779|HR1779|54203|348553|655^1780|111 Tau|54071|173833|499^1781|HR1781|53951|-1597|570^1782|HR1782|53976|-8672|611^1783|8 Lep|53917|-139272|525^1784|29 Ori|53991|-78081|414^1785|HR1785|53867|-267058|649^1786|HR1786|54101|23528|632^1787|27 Ori|54080|-8914|508^1788|η Ori|54079|-23969|336^1789|ψ¹ Ori|54124|18464|495^1790|Bellatrix|54189|63497|164^1791|Elnath|54382|286075|165^1792|HR1792|54079|-169761|565^1793|HR1793|53900|-396786|571^1794|HR1794|54484|354572|615^1795|HR1795|54469|343917|594^1796|HR1796|54476|332628|615^1797|HR1797|53942|-373367|682^1798|113 Tau|54349|167003|625^1799|HR1799|54171|-103292|561^1800|HR1800|54253|-5442|657^1801|κ Pic|53728|-561344|611^1802|17 Cam|55028|630672|542^1803|HR1803|54297|5208|616^18)BSC5";
static const char BSC5_CHUNK_18[] PROGMEM = R"BSC5(04|HR1804|54523|302086|574^1805|φ Aur|54608|344758|507^1806|HR1806|54340|-55183|623^1807|HR1807|54441|68692|642^1808|115 Tau|54528|179622|542^1809|HR1809|54538|152578|616^1810|114 Tau|54606|219369|488^1811|ψ² Ori|54473|30956|459^1812|HR1812|54333|-196956|589^1813|HR1813|54154|-442258|608^1814|116 Tau|54627|158742|550^1815|HR1815|52071|-815417|651^1816|117 Tau|54671|172389|577^1817|HR1817|54513|-119008|635^1818|θ Pic|54128|-523164|627^1819|HR1819|54763|136789|635^1820|HR1820|54671|12983|641^1821|118 Tau|54879|251506|547^1822|HR1822|54946|291864|624^1823|HR1823|54601|-213756|607^1824|HR1824|55135|414619|600^1825|HR1825|55125|398258|637^1826|HR1826|54824|-33075|639^1827|HR1827|54515|-409436|587^1828|18 Cam|55427|572211|648^1829|Nihal|54708|-207594|284^1830|HR1830|54899|-34464|579^1831|HR1831|55121|224625|629^1832|HR1832|55072|153603|594^1833|HR1833|54986|17892|578^1834|31 Ori|54956|-10922|471^1835|HR1835|54709|-372306|557^1836|λ Dor|54387|-589125|514^1837|HR1837|55055|42042|621^1838|HR1838|54852|-301167|675^1839|32 Ori|55131|59481|420^1840|HR1840|55058|-74347|633^1842|33 Ori|55207|32922|546^1843|χ Aur|55455|321919|476^1844|HR1844|56621|750439|617^1845|119 Tau|55369|185944|438^1846|HR1846|55580|421089|655^1847|HR1847|55373|170581|546^1848|HR1848|55225|-67083|622^1849|10 Lep|55188|-208636|555^1850|HR1850|55576|328011|648^1851|Mintaka|55335|-2844|685^1852|Mintaka|55334|-2992|223^1853|HR1853|56212|666961|626^1854|HR1854|55606|347258|627^1855|Thabit|55322|-73014|462^1856|HR1856|55026|-470778|546^1857|19 Cam|56209|641547|615^1858|120 Tau|55588|185403|569^1859|HR1859|54500|-686228|603^1860|HR1860|55608|204742|618^1861|HR1861|55448|-15919|535^1862|ε Col|55202|-354706|387^1863|HR1863|55520|-17183|646^1864|35 Ori|55651|143056|564^1865|Arneb|55455|-178222|258^1866|HR1866|56098|544289|573^1867|HR1867|54882|-623144|659^1868|HR1868|55587|-11561|534^1869|HR1869|56044|477153|611^1870|HR1870|55267|-459253|586^1871|HR1871|55660|14078|659^1872|38 Ori|55713|37669|536^1873|HR1873|55678|-10356|622^1874|HR1874|55678|-14706|593^1875|121 Tau|55909|240394|538^1876|φ¹ Ori|55803|94894|441^1877|HR1877|55476|-385131|548^1878|HR1878|55988|276622|627^1879|Meissa|55856|99342|354^1880|Meissa|55857|99350|561^1881|HR1881|55521|-351397|578^1882|HR1882|55044|-639278|619^1883|HR1883|55871|102400|560^1884|HR1884|56146|401822|609^1885|HR1885|60223|851822|611^1886|HR1886|55836|-60092|567^1887|HR1887|55841|-60019|478^1888|HR1888|55645|-298489|653^1889|HR1889|56084|259394|649^1890|HR1890|55894|-44933|656^1891|HR1891|55896|-44253|624^1892|42 Ori|55898|-48383|459^1893|θ¹ Ori|55878|-53872|673^1894|θ¹ Ori|55878|-53853|796^1895|θ¹ Ori|55879|-53897|513^1896|θ¹ Ori|55881|-53878|670^1897|θ² Ori|55897|-54161|508^1898|HR1898|55920|-43647|638^1899|Nair Al Saif|55906|-59100|277^1900|HR1900|55933|-32528|640^1)BSC5";
static const char BSC5_CHUNK_19[] PROGMEM = R"BSC5(901|45 Ori|55943|-48558|526^1902|HR1902|56191|269242|583^1903|Alnilam|56036|-12019|170^1904|HR1904|56294|335592|633^1905|122 Tau|56177|170403|554^1906|HR1906|56042|-56481|654^1907|φ² Ori|56151|92906|409^1908|HR1908|56179|110350|594^1909|HR1909|55876|-330800|578^1910|Tien Kwan|56274|211425|300^1911|HR1911|56099|-60650|572^1912|HR1912|55623|-549022|643^1913|HR1913|56220|89519|612^1914|26 Aur|56439|304925|540^1915|HR1915|56029|-287078|626^1916|HR1916|57073|656978|560^1917|HR1917|55499|-642275|534^1918|HR1918|56243|-59383|605^1919|HR1919|56191|-117756|611^1920|HR1920|56336|75414|588^1921|HR1921|56493|266178|637^1922|β Dor|55604|-624897|376^1923|HR1923|56315|-48136|619^1924|HR1924|56551|292153|596^1925|HR1925|56890|534811|623^1926|ν¹ Col|56213|-278714|616^1927|HR1927|56008|-473139|611^1928|125 Tau|56623|258969|518^1929|HR1929|56575|217628|634^1930|HR1930|55840|-588711|675^1931|σ Ori|56458|-26000|381^1932|HR1932|56464|-25942|665^1933|HR1933|56438|-65739|596^1934|ω Ori|56531|41214|457^1935|ν² Col|56291|-286894|531^1936|HR1936|55827|-611758|632^1937|49 Ori|56481|-72131|480^1938|HR1938|56766|313581|604^1939|HR1939|56784|319208|611^1940|HR1940|56587|-35647|600^1941|24 Cam|57171|565817|605^1942|HR1942|56586|-97067|650^1943|23 Cam|57357|614767|615^1944|HR1944|56545|-178494|638^1945|HR1945|56892|294875|643^1946|126 Tau|56882|165339|486^1947|HR1947|56454|-407075|582^1948|Alnitak|56793|-19428|205^1949|Alnitak|56793|-19428|421^1950|HR1950|56770|-28250|622^1951|HR1951|56985|233264|659^1952|HR1952|56807|-11289|495^1953|γ Men|55314|-763411|519^1954|HR1954|57011|226603|636^1955|HR1955|56849|3378|593^1956|Phact|56608|-340742|264^1957|HR1957|56794|-104094|652^1958|HR1958|56638|-326292|545^1959|HR1959|56945|-28961|642^1960|HR1960|56152|-665603|631^1961|HR1961|57221|232042|621^1962|HR1962|56949|-167256|621^1963|51 Ori|57079|14747|491^1964|HR1964|55791|-737414|578^1965|HR1965|57040|-175303|615^1966|HR1966|56908|-334006|634^1967|HR1967|57149|-67961|602^1968|12 Lep|57039|-223736|587^1969|26 Cam|57751|561156|594^1970|HR1970|57192|-16131|631^1971|ο Aur|57650|498264|547^1972|HR1972|57032|-305356|619^1973|HR1973|57042|-346678|529^1974|HR1974|57638|405072|658^1975|HR1975|57227|-185575|573^1976|HR1976|58180|628081|613^1977|HR1977|57609|206950|695^1978|HR1978|57505|40081|609^1979|HR1979|57874|425267|629^1980|HR1980|57412|-201264|634^1981|HR1981|57251|-394069|625^1982|HR1982|57407|-224217|615^1983|γ Lep|57411|-224483|360^1984|HR1984|57281|-458331|639^1985|129 Tau|57793|158225|600^1986|HR1986|57674|-42683|634^1987|HR1987|57811|95222|579^1988|HR1988|57764|11681|595^1989|131 Tau|57870|144883|572^1990|130 Tau|57906|177292|549^1991|ι Men|55934|-788208|605^1992|29 Cam|58428|569189|654^1993|133 Tau|57952|138997|529^1994|HR1994|58821|684714|620^1995|τ Aur|58196|391811|452^1996|μ Col|57666|-32)BSC5";
static const char BSC5_CHUNK_20[] PROGMEM = R"BSC5(3064|517^1997|HR1997|58062|208694|607^1998|ζ Lep|57826|-148219|355^1999|52 Ori|58001|64542|527^2000|HR2000|57854|-162378|617^2001|HR2001|57908|-105331|603^2002|132 Tau|58169|245675|486^2003|HR2003|58490|515147|629^2004|Saiph|57959|-96697|206^2005|HR2005|57846|-286392|622^2006|30 Cam|58715|589642|614^2007|HR2007|58097|-40947|597^2008|HR2008|57743|-465972|531^2009|HR2009|57885|-356747|632^2010|134 Tau|58258|126511|491^2011|υ Aur|58507|373056|474^2012|ν Aur|58582|391486|397^2013|HR2013|58495|279678|556^2014|HR2014|58341|98711|580^2015|δ Dor|57462|-657356|435^2016|135 Tau|58414|143056|552^2017|HR2017|57995|-406525|661^2018|HR2018|58571|321250|625^2019|HR2019|58370|44233|597^2020|β Pic|57881|-510664|385^2021|HR2021|58268|-144836|549^2022|π Men|56194|-804692|565^2023|HR2023|57869|-543608|618^2024|HR2024|58417|20244|598^2025|HR2025|58776|395744|645^2026|HR2026|58315|-229717|587^2027|31 Cam|59161|598883|520^2028|HR2028|58778|339175|598^2029|ξ Aur|59141|557069|499^2030|HR2030|58732|198681|606^2031|55 Ori|58561|-75181|535^2032|HR2032|58261|-448753|638^2033|137 Tau|58729|141717|559^2034|136 Tau|58888|276122|458^2035|δ Lep|58554|-208792|381^2036|HR2036|58579|-229261|617^2037|56 Ori|58740|18550|478^2038|HR2038|58886|202992|671^2039|HR2039|58688|-90414|597^2040|Wazn|58493|-357683|312^2041|HR2041|59597|660961|625^2042|γ Pic|58305|-561667|451^2043|HR2043|58665|-294486|645^2044|HR2044|58413|-527678|635^2045|HR2045|59373|518039|649^2046|HR2046|59164|317017|590^2047|χ¹ Ori|59064|202761|441^2048|HR2048|59037|105869|612^2049|HR2049|58481|-521089|517^2050|HR2050|59089|117625|659^2051|HR2051|59044|32253|631^2052|57 Ori|59158|197497|592^2053|HR2053|58759|-376311|563^2054|HR2054|59514|490294|647^2055|HR2055|58799|-385258|670^2056|λ Col|58852|-338014|487^2057|HR2057|59122|9683|600^2058|HR2058|59096|-40639|657^2059|HR2059|55039|-847850|620^2060|HR2060|58993|-196383|669^2061|Betelgeuse|59195|74069|50^2062|λ Men|57967|-727022|653^2063|HR2063|59304|201750|540^2064|ε Dor|58316|-669011|511^2065|HR2065|59121|-117739|566^2066|HR2066|59427|289422|632^2067|HR2067|59343|139253|660^2068|HR2068|59039|-291478|636^2069|HR2069|58897|-429214|655^2070|HR2070|59251|-46164|587^2071|HR2071|59265|-47883|628^2072|HR2072|58723|-571561|594^2073|HR2073|58564|-640336|636^2074|HR2074|59489|242497|602^2075|HR2075|59411|95097|599^2076|HR2076|59471|115211|587^2077|δ Aur|59921|542847|372^2078|HR2078|60859|755858|640^2079|HR2079|59960|553208|644^2080|HR2080|59967|545472|614^2081|HR2081|59894|499244|589^2082|HR2082|59146|-399581|557^2083|HR2083|59030|-503619|652^2084|139 Tau|59666|259539|482^2085|η Lep|59401|-141678|371^2086|HR2086|59373|-228403|596^2087|ξ Col|59250|-371208|497^2088|Menkalinan|59921|449475|190^2089|HR2089|59114|-496269|610^2090|HR2090|59429|-232156|636^2091|π Aur|59989|459369|426^2092|σ Col|5)BSC5";
static const char BSC5_CHUNK_21[] PROGMEM = R"BSC5(9391|-313825|550^2093|HR2093|59651|12244|622^2094|HR2094|59139|-526353|529^2095|Mahasim|59954|372125|262^2096|HR2096|60053|445919|622^2097|HR2097|59699|-9942|622^2098|HR2098|59469|-319761|644^2099|HR2099|59814|128086|570^2100|59 Ori|59734|18369|590^2101|36 Aur|60163|479019|573^2102|HR2102|59017|-630900|465^2103|60 Ori|59804|5531|522^2104|HR2104|59033|-644822|663^2105|HR2105|60286|489594|596^2106|γ Col|59589|-352833|436^2107|1 Mon|59836|-93822|612^2108|2 Mon|59845|-95583|503^2109|HR2109|59938|-14444|663^2110|HR2110|60195|310347|598^2111|HR2111|60168|275722|605^2112|HR2112|60469|499056|605^2113|HR2113|60009|-30742|453^2114|HR2114|59540|-534261|645^2115|HR2115|60482|433786|642^2116|HR2116|60282|224008|637^2117|HR2117|59771|-440344|581^2118|HR2118|60049|-128997|622^2119|38 Aur|60550|429117|610^2120|η Col|59858|-428153|396^2121|HR2121|60856|593931|634^2122|HR2122|60486|326358|624^2123|HR2123|60748|515733|645^2124|μ Ori|60397|96475|412^2125|κ Men|58380|-793614|547^2126|HR2126|61109|634536|639^2127|HR2127|60381|16944|659^2128|3 Mon|60307|-105981|495^2129|HR2129|60203|-254178|605^2130|64 Ori|60576|196906|514^2131|HR2131|60212|-339117|555^2132|39 Aur|60843|429817|587^2133|HR2133|60569|116808|608^2134|1 Gem|60687|232633|416^2135|χ² Ori|60653|201383|463^2136|HR2136|60427|-144972|620^2137|HR2137|60841|379642|634^2138|HR2138|60137|-512164|567^2139|HR2139|60928|335992|623^2140|HR2140|60543|-262844|504^2141|HR2141|61024|353875|612^2142|HR2142|60704|-67092|521^2143|40 Aur|61098|384828|536^2144|63 Ori|60828|54200|567^2145|66 Ori|60829|41586|563^2146|HR2146|61062|295125|608^2147|HR2147|61241|418542|612^2148|17 Lep|60831|-164844|493^2149|HR2149|60723|-321725|565^2150|HR2150|60908|-102428|587^2151|HR2151|60359|-600969|645^2152|37 Cam|61664|589358|536^2153|HR2153|61397|410556|636^2154|HR2154|61107|-41939|538^2155|θ Lep|61026|-149353|467^2156|HR2156|60960|-241956|695^2157|HR2157|60746|-450367|635^2158|HR2158|60778|-450789|593^2159|ν Ori|61262|147683|442^2160|HR2160|60909|-355136|580^2161|HR2161|61144|-111736|666^2162|HR2162|60797|-484586|658^2163|HR2163|61089|-231106|547^2164|HR2164|61015|-297586|581^2165|36 Cam|62142|657183|532^2166|HR2166|61160|-218128|578^2167|HR2167|61464|86700|655^2168|19 Lep|61282|-191658|531^2169|HR2169|61590|221900|593^2170|HR2170|61177|-343119|583^2171|π¹ Col|61114|-422986|612^2172|HR2172|61961|526472|630^2173|3 Gem|61622|231133|575^2174|HR2174|61494|24994|573^2175|41 Aur|61935|487131|682^2176|41 Aur|61935|487111|609^2177|Al Kurud|61254|-372531|502^2178|HR2178|61172|-450914|651^2179|HR2179|61601|-57111|617^2180|HR2180|61494|-224275|550^2181|π² Col|61314|-421539|550^2182|HR2182|61556|-181261|635^2183|HR2183|61595|-145842|556^2184|HR2184|61838|181294|633^2185|5 Gem|61923|244203|580^2186|HR2186|61633|-227742|571^2187|HR2187|61429|-443561|627^2188|HR)BSC5";
static const char BSC5_CHUNK_22[] PROGMEM = R"BSC5(2188|62293|511725|604^2189|HR2189|62056|326933|578^2190|HR2190|61976|218686|656^2191|HR2191|61911|136386|604^2192|HR2192|61631|-267008|627^2193|68 Ori|62004|197906|575^2194|η¹ Dor|61026|-660397|571^2195|HR2195|61837|-67542|615^2196|HR2196|61176|-621547|505^2197|6 Gem|62053|229083|639^2198|69 Ori|62009|161306|495^2199|ξ Ori|61990|142089|448^2200|HR2200|61763|-271542|572^2201|40 Cam|62613|599992|535^2202|HR2202|61955|-46656|618^2203|HR2203|61695|-403536|558^2204|HR2204|61565|-495628|649^2205|HR2205|61977|-65503|505^2206|HR2206|61871|-264822|609^2207|HR2207|62259|186803|658^2208|HR2208|62202|106275|645^2209|HR2209|63141|693197|480^2210|HR2210|62123|-25044|662^2211|HR2211|61778|-452819|631^2212|δ Pic|61716|-549686|481^2213|HR2213|62129|-177631|652^2214|HR2214|62413|179064|588^2215|1 Lyn|62986|615153|498^2216|Propus|62479|225067|328^2217|HR2217|62608|361486|692^2218|HR2218|62318|-37414|583^2219|κ Aur|62563|294981|435^2220|71 Ori|62475|191564|520^2221|ν Dor|61456|-688433|506^2222|HR2222|62524|138511|591^2223|72 Ori|62570|161431|530^2224|HR2224|62435|-45683|583^2225|HR2225|62292|-238619|639^2226|HR2226|62259|-293961|654^2227|γ Mon|62476|-62747|398^2228|42 Aur|62930|464242|652^2229|73 Ori|62625|125511|533^2230|8 Gem|62719|239700|608^2231|HR2231|62611|60661|607^2232|HR2232|62631|42836|664^2233|HR2233|62595|-5122|565^2234|HR2234|62582|-49153|599^2235|HR2235|62733|171814|639^2236|HR2236|62650|11692|637^2237|HR2237|62572|-90353|610^2238|2 Lyn|63271|590108|448^2239|43 Aur|63047|463606|638^2240|9 Gem|62830|237408|625^2241|74 Ori|62741|122722|504^2242|HR2242|62523|-202722|591^2243|HR2243|62549|-184767|599^2244|HR2244|62625|-137183|501^2245|η² Dor|61875|-655894|501^2246|HR2246|62726|10803|663^2247|75 Ori|62852|99425|539^2248|HR2248|62829|70531|657^2249|HR2249|62688|-166178|592^2250|HR2250|62926|140583|659^2251|HR2251|62878|51003|571^2252|HR2252|62659|-297883|667^2253|HR2253|63016|143828|616^2254|HR2254|62843|-227150|607^2255|6 Mon|62931|-107253|675^2256|κ Col|62759|-351406|437^2257|4 Lyn|63677|593722|594^2258|HR2258|63172|173250|632^2259|HR2259|63112|90472|624^2260|HR2260|62949|-168158|514^2261|α Men|61707|-747531|509^2262|HR2262|62766|-392644|600^2263|HR2263|62837|-377375|553^2264|45 Aur|63628|534522|536^2265|HR2265|62860|-372531|587^2266|HR2266|63038|-199669|552^2267|HR2267|63141|-93903|536^2268|HR2268|63136|-150247|606^2269|HR2269|63345|146511|569^2270|HR2270|63189|-85864|622^2271|HR2271|63164|-209261|581^2272|HR2272|63534|295411|643^2273|7 Mon|63286|-78231|527^2274|HR2274|62718|-592136|643^2275|HR2275|63332|-29444|490^2276|HR2276|63479|117564|654^2277|HR2277|63572|177636|635^2278|HR2278|62977|-527331|641^2279|HR2279|63281|-343967|578^2280|HR2280|63572|22686|631^2281|HR2281|63130|-503592|704^2282|Furud|63386|-300633|302^2283|HR2283|62516|-717028|664^2284|HR2284|6)BSC5";
static const char BSC5_CHUNK_23[] PROGMEM = R"BSC5(3569|-117733|564^2285|HR2285|64707|705356|597^2286|Tejat Posterior|63827|225136|288^2287|HR2287|63768|125700|600^2288|HR2288|63434|-341439|553^2289|ψ¹ Aur|64150|492881|491^2290|HR2290|63350|-487411|660^2291|HR2291|64405|562850|564^2292|HR2292|63885|37644|640^2293|5 Lyn|64469|584172|521^2294|Mirzam|63783|-179558|198^2295|HR2295|63896|-46872|667^2296|δ Col|63686|-334364|385^2297|HR2297|64146|297072|671^2298|ε Mon|63961|45928|444^2299|HR2299|63962|45956|672^2300|HR2300|64006|88847|626^2301|HR2301|63933|-98744|619^2302|HR2302|64147|160569|633^2303|HR2303|63961|-150717|624^2304|HR2304|64258|233269|606^2305|HR2305|64029|-115303|522^2306|HR2306|63966|-197853|660^2307|HR2307|63873|-317900|634^2308|HR2308|64245|147219|624^2309|HR2309|64057|-129625|612^2310|HR2310|64203|70858|598^2311|HR2311|63989|-255778|563^2312|HR2312|64218|15011|666^2313|HR2313|64212|-9461|587^2314|HR2314|64642|474053|656^2315|HR2315|64296|22722|651^2316|HR2316|64003|-367078|562^2317|HR2317|64298|-38892|635^2318|HR2318|64122|-287800|639^2319|HR2319|64599|325631|643^2320|ν Pic|63822|-563700|561^2321|HR2321|64330|-78942|640^2322|HR2322|63938|-521811|598^2323|HR2323|64124|-402842|631^2324|HR2324|64443|-15072|587^2325|HR2325|64429|-45972|615^2326|Canopus|63992|-526958|-72^2327|HR2327|64496|8408|671^2328|HR2328|64458|-75114|627^2329|HR2329|64250|-350639|625^2330|16 Gem|64657|204961|622^2331|6 Lyn|65131|581628|588^2332|48 Aur|64761|304931|555^2333|HR2333|64557|29081|555^2334|HR2334|64538|2992|520^2335|HR2335|64543|-2761|555^2336|HR2336|63964|-585439|648^2337|HR2337|63837|-636831|627^2338|47 Aur|65008|466856|590^2339|HR2339|64824|269678|647^2340|HR2340|64744|162383|623^2341|HR2341|64134|-528064|651^2342|HR2342|64719|103039|615^2343|ν Gem|64827|202122|415^2344|10 Mon|64660|-47622|506^2345|HR2345|64038|-602811|580^2346|HR2346|66713|795994|654^2347|HR2347|64713|19122|648^2348|HR2348|64288|-481772|576^2349|HR2349|64531|-258567|607^2350|HR2350|67417|821153|665^2351|HR2351|64833|110194|659^2352|π¹ Dor|63773|-699842|556^2353|HR2353|64521|-378956|648^2354|HR2354|64073|-634289|646^2355|HR2355|64875|26461|616^2356|β Mon|64803|-70328|460^2357|β Mon|64804|-70344|540^2358|β Mon|64804|-70344|560^2359|HR2359|64770|-174661|577^2360|HR2360|64154|-638278|627^2361|λ CMa|64695|-325800|448^2362|HR2362|65015|90292|657^2363|HR2363|66747|779958|573^2364|HR2364|64776|-323711|574^2365|HR2365|66319|736956|624^2366|HR2366|65195|169386|620^2367|HR2367|65031|-100814|593^2368|HR2368|64784|-410744|632^2369|HR2369|64511|-580022|582^2370|HR2370|65193|112508|614^2371|19 Gem|65271|159033|640^2372|HR2372|65409|324547|587^2373|HR2373|65096|-131481|616^2374|HR2374|65276|117922|665^2375|HR2375|65301|115444|523^2376|7 Lyn|65758|553531|645^2377|π² Dor|64246|-696903|538^2378|HR2378|65398|116736|603^2379|HR2379|65231|-123917|515^2380|HR238)BSC5";
static const char BSC5_CHUNK_24[] PROGMEM = R"BSC5(0|65129|-277694|593^2381|HR2381|65306|-81581|543^2382|12 Mon|65387|48558|584^2383|HR2383|65619|330242|642^2384|HR2384|64970|-502392|527^2385|13 Mon|65484|73331|450^2386|HR2386|65398|-58689|560^2387|ξ¹ CMa|65309|-234183|433^2388|HR2388|65203|-352592|584^2389|HR2389|64912|-568528|522^2390|HR2390|65166|-409164|620^2391|HR2391|65600|141553|553^2392|HR2392|65464|-111664|624^2393|HR2393|65264|-369400|634^2394|8 Lyn|66282|614811|594^2395|HR2395|65605|-12203|510^2396|HR2396|66756|717489|592^2397|HR2397|65442|-320306|569^2398|49 Aur|65867|280222|527^2399|HR2399|65392|-376967|524^2400|HR2400|65218|-518261|560^2401|HR2401|67706|795647|545^2402|11 Lyn|66274|568575|585^2403|HR2403|65574|-209239|640^2404|14 Mon|65796|75725|645^2405|HR2405|66091|384453|529^2406|HR2406|65882|99883|588^2407|HR2407|65529|-386253|644^2408|HR2408|65008|-655683|629^2409|HR2409|65877|8900|580^2410|HR2410|65196|-618797|615^2411|HR2411|65638|-362322|542^2412|μ Pic|65329|-587542|570^2413|HR2413|66000|44975|655^2414|ξ² CMa|65843|-229647|454^2415|HR2415|65765|-327164|562^2416|HR2416|65572|-523289|619^2417|HR2417|66242|245908|644^2418|HR2418|66098|-52111|552^2419|51 Aur|66443|393908|569^2420|ψ³ Aur|66470|399025|520^2421|Alhena|66285|163992|193^2422|HR2422|66234|61353|606^2423|ν¹ CMa|66063|-186600|570^2424|HR2424|65900|-367800|559^2425|53 Aur|66397|289842|579^2426|HR2426|66269|108531|638^2427|ψ² Aur|66555|424889|479^2428|HR2428|66130|-133208|597^2429|ν² CMa|66114|-192558|395^2430|HR2430|66279|27042|617^2431|HR2431|65983|-360889|635^2432|HR2432|66313|49572|615^2433|HR2433|66114|-226150|635^2434|HR2434|66661|440139|641^2435|HR2435|65829|-529756|439^2436|HR2436|66515|220308|604^2437|HR2437|66280|-129850|612^2438|54 Aur|66592|282631|603^2439|HR2439|66587|246000|638^2440|HR2440|66390|-25436|614^2441|HR2441|66471|47006|657^2442|HR2442|66439|16136|621^2443|ν³ CMa|66315|-182375|443^2444|HR2444|66172|-381467|604^2445|HR2445|66142|-415569|634^2446|HR2446|66205|-369906|571^2447|HR2447|66299|-323397|527^2448|HR2448|66432|-168736|603^2449|HR2449|66632|129831|597^2450|HR2450|66546|-141458|482^2451|ν Pup|66294|-431961|317^2452|HR2452|66938|359319|646^2453|25 Gem|66892|281964|642^2454|HR2454|66755|63717|651^2455|HR2455|66601|-236956|605^2456|15 Mon|66830|98956|466^2457|HR2457|66894|163975|628^2458|HR2458|66881|110033|611^2459|ψ⁴ Aur|67181|445244|502^2460|HR2460|66619|-304703|571^2461|HR2461|66848|4953|579^2462|HR2462|66438|-482203|493^2463|HR2463|67366|532964|627^2464|HR2464|67205|371469|619^2465|HR2465|66658|-381589|658^2466|26 Gem|67068|176453|521^2467|HR2467|66998|63450|637^2468|HR2468|66335|-615331|618^2469|HR2469|66990|-91672|519^2470|12 Lyn|67706|594417|487^2471|HR2471|67368|361097|631^2473|Mebsuta|67322|251311|298^2474|HR2474|67185|30333|619^2475|HR2475|66872|-403497|612^2476|HR2476|66804|-476747|665^247)BSC5";
static const char BSC5_CHUNK_25[] PROGMEM = R"BSC5(7|13 Lyn|67804|571692|535^2478|30 Gem|67331|132278|449^2479|HR2479|67274|39322|590^2480|28 Gem|67460|289708|544^2481|HR2481|67127|-224492|613^2482|HR2482|67046|-383986|629^2483|ψ⁵ Aur|67790|435775|525^2484|Alzir|67548|128956|336^2485|HR2485|68036|557044|633^2486|HR2486|68034|557044|628^2487|ψ⁶ Aur|67943|487894|522^2488|HR2488|67231|-391933|630^2489|32 Gem|67651|126936|646^2490|42 Cam|68492|675719|514^2491|Sirius|67525|-167161|-146^2492|10 CMa|67412|-310706|520^2493|HR2493|67478|-273414|645^2494|16 Mon|67757|85872|593^2495|HR2495|67565|-234619|605^2497|HR2497|67507|-305861|654^2498|HR2498|67665|-147961|532^2499|HR2499|67899|181933|620^2500|HR2500|67563|-317936|592^2501|HR2501|67587|-309489|580^2502|HR2502|67775|-101072|566^2503|17 Mon|67888|80372|477^2504|11 CMa|67809|-144258|529^2505|HR2505|66827|-717756|651^2506|18 Mon|67977|24122|447^2507|HR2507|67676|-395400|662^2508|HR2508|67936|-89983|507^2509|12 CMa|67838|-210156|608^2510|HR2510|67700|-377756|621^2511|43 Cam|68951|688883|512^2512|HR2512|68281|326067|571^2513|HR2513|67572|-522011|657^2514|HR2514|68053|-13192|575^2515|HR2515|67649|-524100|580^2516|ψ⁷ Aur|68461|417814|502^2517|HR2517|68177|10019|615^2518|HR2518|67893|-379297|526^2519|33 Gem|68305|162028|585^2520|14 Lyn|68848|594486|533^2521|HR2521|68212|-22719|574^2522|HR2522|68161|-151447|539^2523|HR2523|67813|-512658|540^2524|HR2524|67782|-546947|646^2525|35 Gem|68404|134133|565^2526|HR2526|67885|-555400|561^2527|HR2527|70011|769775|455^2528|HR2528|68289|-240758|633^2529|36 Gem|68592|217611|527^2530|HR2530|68472|-5408|577^2531|HR2531|67269|-731181|637^2532|HR2532|68854|448394|626^2533|HR2533|68667|236017|565^2534|HR2534|68451|-80411|629^2535|HR2535|68394|-170839|579^2536|HR2536|67489|-704339|611^2537|HR2537|68350|-273339|704^2538|κ CMa|68307|-325086|396^2539|59 Aur|68837|388692|612^2540|θ Gem|68798|339611|360^2541|60 Aur|68871|384381|630^2542|HR2542|68843|357886|601^2543|HR2543|68609|30417|638^2544|HR2544|68436|-257781|633^2545|HR2545|68398|-317061|570^2546|HR2546|68327|-454500|655^2547|ψ⁸ Aur|68992|385050|648^2548|HR2548|68318|-466147|514^2549|HR2549|68479|-343672|499^2550|α Pic|68032|-619414|327^2551|HR2551|68804|83803|577^2552|HR2552|68730|-53161|630^2553|τ Pup|68323|-506147|293^2554|HR2554|68309|-536222|440^2555|HR2555|68896|109964|624^2556|HR2556|69209|458264|634^2557|HR2557|69208|439100|613^2558|HR2558|68618|-362303|596^2559|ζ Men|66674|-808136|564^2560|15 Lyn|69546|584225|435^2561|HR2561|69537|575633|605^2562|HR2562|68336|-602492|611^2563|HR2563|68591|-482925|642^2564|38 Gem|69108|131778|465^2565|HR2565|68886|-190328|564^2566|HR2566|68894|-189333|614^2567|HR2567|68834|-269575|640^2568|ψ⁹ Aur|69423|462742|587^2569|37 Gem|69218|253756|573^2570|HR2570|69024|-58525|641^2571|15 CMa|68925|-202242|483^2572|HR2572|69068|-11269|545^2573|HR2573|694)BSC5";
static const char BSC5_CHUNK_26[] PROGMEM = R"BSC5(89|467050|586^2574|θ CMa|69032|-120386|407^2575|HR2575|68777|-425044|652^2576|HR2576|68928|-285397|604^2577|HR2577|69117|-17564|621^2578|HR2578|68987|-245392|621^2579|HR2579|68798|-439758|646^2580|ο¹ CMa|69022|-241839|387^2581|HR2581|70226|708081|568^2582|HR2582|69163|-28036|604^2583|HR2583|69036|-239283|691^2584|HR2584|69263|83247|629^2585|16 Lyn|69603|450942|490^2586|HR2586|69501|336811|589^2587|HR2587|68797|-540900|657^2588|17 CMa|69174|-204047|574^2589|HR2589|69405|99564|592^2590|π CMa|69271|-201364|468^2591|HR2591|69074|-423656|632^2592|HR2592|68793|-593411|641^2593|μ CMa|69352|-140436|500^2594|HR2594|69006|-506117|626^2595|HR2595|69297|-229414|530^2596|ι CMa|69356|-170542|437^2597|HR2597|69571|119075|627^2598|HR2598|69319|-317900|636^2599|HR2599|69500|-81789|634^2600|62 Aur|69841|380522|600^2601|39 Gem|69798|260811|610^2602|ι Vol|68575|-709633|540^2603|HR2603|69541|-222033|661^2604|HR2604|69460|-353417|629^2605|40 Gem|69911|259142|640^2606|HR2606|69775|76219|627^2607|HR2607|69594|-246306|546^2608|HR2608|69378|-487211|495^2609|HR2609|76751|870200|507^2610|HR2610|69825|36022|597^2611|HR2611|69618|-275375|623^2612|HR2612|69549|-355075|623^2613|HR2613|69889|73169|635^2614|HR2614|69688|-271647|637^2615|41 Gem|70044|160789|568^2616|HR2616|69766|-254139|559^2617|HR2617|70977|707319|650^2618|Adhara|69771|-289722|150^2619|HR2619|69736|-341117|506^2620|HR2620|70214|324144|659^2621|HR2621|69788|-309978|642^2622|HR2622|70050|-53669|630^2623|HR2623|69942|-216033|626^2624|HR2624|70066|-84069|596^2625|HR2625|70022|-201589|631^2626|HR2626|69783|-457681|622^2627|HR2627|70109|-92031|649^2628|HR2628|70054|-221194|653^2629|HR2629|70282|48181|663^2630|ω Gem|70402|242153|518^2631|HR2631|70404|177556|594^2632|HR2632|70382|153361|574^2633|HR2633|70319|55575|659^2634|HR2634|69777|-557294|627^2635|HR2635|70426|166742|582^2636|HR2636|70314|-13456|617^2637|HR2637|70118|-284894|627^2638|HR2638|69767|-563947|645^2639|HR2639|70323|-57222|520^2640|HR2640|70183|-252153|563^2641|HR2641|70138|-334653|640^2642|HR2642|71004|598019|644^2643|HR2643|70584|293372|593^2644|HR2644|70944|527581|612^2645|HR2645|70858|477750|638^2646|σ CMa|70286|-279347|347^2647|HR2647|70550|91383|597^2648|19 Mon|70486|-42392|499^2649|HR2649|70606|109517|513^2650|Mekbuda|70685|205703|379^2651|HR2651|70643|125944|598^2652|HR2652|70143|-514025|514^2653|ο² CMa|70504|-238333|302^2654|HR2654|70723|14883|657^2655|HR2655|70681|-53236|562^2656|HR2656|70659|-101242|645^2657|Muliphein|70626|-156333|412^2658|HR2658|70377|-434042|643^2659|44 Gem|70884|226372|602^2660|HR2660|71032|344739|555^2661|HR2661|70181|-589400|602^2662|HR2662|69974|-679161|517^2663|HR2663|70942|91858|578^2664|HR2664|70798|-220322|609^2665|HR2665|71229|340094|591^2666|HR2666|70674|-423372|520^2667|HR2667|70659|-436081|554^2668|HR2668|70663|-43611)BSC5";
static const char BSC5_CHUNK_27[] PROGMEM = R"BSC5(7|679^2669|HR2669|71236|281772|648^2670|HR2670|70971|-106611|649^2671|HR2671|71226|227036|768^2672|HR2672|70649|-495839|493^2673|HR2673|71370|338322|628^2674|HR2674|70543|-591781|550^2675|HR2675|71434|374450|616^2676|HR2676|71184|49103|611^2677|HR2677|70922|-347778|614^2678|HR2678|71113|-112942|539^2679|HR2679|71100|-123939|648^2680|HR2680|71002|-306556|634^2681|HR2681|72328|718167|635^2682|HR2682|71304|74711|575^2683|HR2683|70718|-567497|517^2684|45 Gem|71394|159308|544^2685|HR2685|71006|-383828|611^2686|HR2686|71145|-249606|608^2687|HR2687|70879|-503603|646^2688|HR2688|71167|-266578|662^2689|θ Men|69429|-794203|545^2690|HR2690|71229|-238403|571^2691|HR2691|71186|-408933|579^2692|HR2692|71685|212469|643^2693|Wezen|71399|-263933|184^2694|HR2694|71556|-103472|621^2695|HR2695|71470|-240442|665^2696|63 Aur|71942|393206|490^2697|τ Gem|71857|302453|441^2698|HR2698|71204|-519678|596^2699|HR2699|71592|-162347|603^2700|47 Gem|71898|268567|578^2701|20 Mon|71705|-42372|492^2702|HR2702|71475|-396558|483^2703|HR2703|72232|514289|547^2704|HR2704|71619|-252311|569^2705|HR2705|71692|-186853|623^2706|48 Gem|72073|241283|585^2707|21 Mon|71899|-3019|545^2708|HR2708|71721|-274914|546^2709|HR2709|74228|812575|631^2710|HR2710|71976|56547|609^2711|HR2711|72136|272247|643^2712|HR2712|71039|-688372|647^2713|HR2713|72021|54747|616^2714|δ Mon|71978|-4928|415^2715|18 Lyn|72652|596375|520^2716|HR2716|71949|-208831|584^2717|51 Gem|72229|161589|500^2718|26 CMa|72034|-259425|592^2719|HR2719|71799|-489322|514^2720|HR2720|72011|-308217|610^2721|HR2721|72639|472400|558^2722|HR2722|72407|247108|689^2723|HR2723|72187|-112514|578^2724|HR2724|72067|-274742|659^2725|52 Gem|72450|248850|582^2726|HR2726|72071|-365444|596^2727|HR2727|72044|-404989|531^2728|HR2728|72424|121158|562^2729|HR2729|72389|31114|535^2730|HR2730|72233|-226733|601^2731|HR2731|72364|-39014|575^2732|HR2732|72376|-99475|590^2733|HR2733|72301|-229064|636^2734|HR2734|72268|-273564|612^2735|γ¹ Vol|71451|-704972|569^2736|γ² Vol|71458|-704989|378^2737|HR2737|72927|521308|592^2738|53 Gem|72659|278975|571^2739|HR2739|72412|-103167|603^2740|HR2740|72093|-467594|449^2741|HR2741|72298|-310839|660^2742|HR2742|75179|824114|496^2743|HR2743|72326|-303400|633^2744|24 Mon|72554|-1614|641^2745|27 CMa|72376|-263525|466^2746|HR2746|72204|-451831|489^2747|HR2747|72609|79778|582^2748|HR2748|72257|-446397|510^2749|ω CMa|72469|-267728|385^2750|HR2750|72475|-270381|558^2751|HR2751|73089|494650|505^2752|HR2752|72620|-105839|595^2753|64 Aur|73006|408833|578^2754|HR2754|72006|-631900|602^2755|HR2755|72632|-237406|632^2756|HR2756|72558|-306864|536^2757|HR2757|73011|309558|624^2758|HR2758|72707|-155858|546^2759|HR2759|72492|-414258|594^2760|HR2760|72883|66806|665^2761|HR2761|72461|-468497|572^2762|HR2762|72439|-482717|476^2763|λ Gem|73016|165403|358^)BSC5";
static const char BSC5_CHUNK_28[] PROGMEM = R"BSC5(2764|HR2764|72769|-233156|479^2765|HR2765|72921|-66800|629^2766|HR2766|72764|-278811|464^2767|HR2767|72558|-524997|597^2768|HR2768|72826|-308967|632^2769|HR2769|72755|-383189|580^2770|HR2770|72804|-365928|503^2771|HR2771|72710|-467744|566^2772|47 Cam|73714|599019|635^2773|π Pup|72857|-370975|270^2774|HR2774|72966|-267975|646^2775|HR2775|73509|426556|635^2776|HR2776|73549|452281|577^2777|Wasat|73354|219822|353^2778|HR2778|73229|27406|589^2779|HR2779|73299|71428|591^2780|HR2780|73352|151428|645^2781|29 CMa|73112|-245589|498^2782|τ CMa|73118|-249542|440^2783|19 Lyn|73808|552844|653^2784|19 Lyn|73811|552814|545^2785|HR2785|73172|-192803|609^2786|HR2786|73142|-265858|528^2787|HR2787|73051|-367342|466^2788|HR2788|73245|-163950|570^2789|HR2789|73012|-439867|585^2790|HR2790|73106|-367428|511^2791|HR2791|73093|-392103|525^2792|HR2792|73704|389961|640^2793|65 Aur|73674|367606|513^2794|HR2794|73205|-337272|630^2795|56 Gem|73658|204436|510^2796|HR2796|73495|-143600|545^2797|HR2797|75777|808967|641^2798|HR2798|73547|-88783|655^2799|HR2799|73481|-228517|661^2800|HR2800|73486|-269636|601^2801|HR2801|73676|1772|599^2802|HR2802|73512|-258914|587^2803|δ Vol|72805|-679572|398^2804|HR2804|74159|518872|580^2805|66 Aur|74024|406722|519^2806|HR2806|73672|-89792|643^2807|HR2807|73718|-29789|623^2808|57 Gem|73912|250506|503^2809|HR2809|74572|663317|647^2810|58 Gem|73912|229453|602^2811|HR2811|73737|-59828|582^2812|HR2812|73704|-190167|496^2813|HR2813|73393|-523117|605^2814|HR2814|73394|-523097|660^2815|HR2815|73441|-520861|539^2816|59 Gem|74093|276381|576^2817|HR2817|74077|155172|641^2818|21 Lyn|74452|492114|464^2819|HR2819|73835|-319239|543^2820|1 CMi|74162|116697|530^2821|ι Gem|74288|277981|379^2822|HR2822|73914|-278342|538^2823|HR2823|73922|-322022|539^2824|HR2824|73984|-302169|660^2825|HR2825|74111|-162011|533^2826|HR2826|74048|-229128|619^2827|Aludra|74016|-293031|245^2828|ε CMi|74275|92761|499^2829|HR2829|73995|-358378|631^2830|HR2830|75146|684656|564^2831|HR2831|74141|-190122|624^2832|HR2832|74190|-137519|578^2833|HR2833|74308|-57750|597^2834|HR2834|74122|-318089|535^2835|HR2835|74473|215358|654^2836|HR2836|74411|106083|637^2837|61 Gem|74490|202572|593^2838|HR2838|74343|-45375|676^2839|HR2839|74222|-219828|605^2840|HR2840|74448|110092|641^2841|HR2841|74237|-252178|578^2842|HR2842|74131|-372900|697^2843|HR2843|74131|-372908|684^2844|HR2844|74810|481839|572^2845|Gomeisa|74525|82894|290^2846|63 Gem|74623|214450|522^2847|HR2847|74286|-317386|631^2848|HR2848|67830|-870250|647^2849|22 Lyn|74989|496725|536^2850|HR2850|74446|-237122|656^2851|η CMi|74672|69419|525^2852|ρ Gem|74852|317844|418^2853|HR2853|74522|-178644|563^2854|γ CMi|74694|89256|432^2855|HR2855|74498|-230861|561^2856|HR2856|74451|-341408|590^2857|64 Gem|74890|281181|505^2858|HR2858|74798|151094|622^2859|HR2859|7)BSC5";
static const char BSC5_CHUNK_29[] PROGMEM = R"BSC5(4644|-115569|579^2860|HR2860|74619|-228597|595^2861|65 Gem|74969|279161|501^2862|HR2862|74394|-510183|510^2863|HR2863|74664|-291558|554^2864|6 CMi|74966|120067|454^2865|HR2865|74885|-19053|559^2866|HR2866|74905|-75511|586^2867|HR2867|74895|-103267|575^2868|HR2868|74894|-149992|605^2869|HR2869|74730|-378103|658^2870|HR2870|74809|-318483|638^2871|HR2871|74810|-318469|713^2872|HR2872|75321|388964|654^2873|HR2873|74847|-314564|577^2874|HR2874|74976|-230244|485^2875|HR2875|74849|-388122|543^2876|HR2876|75142|-52264|624^2877|HR2877|75301|170861|542^2878|σ Pup|74872|-433014|325^2879|HR2879|75474|228878|654^2880|δ¹ CMi|75350|19144|525^2881|HR2881|75118|-309622|465^2882|HR2882|75118|-373397|665^2883|HR2883|75349|-88808|590^2884|HR2884|74999|-526511|587^2885|HR2885|75238|-361528|668^2886|68 Gem|75601|158267|525^2887|δ² CMi|75532|32903|559^2888|HR2888|74809|-645100|639^2889|HR2889|75286|-358878|661^2890|Castor|75767|318886|288^2891|Castor|75767|318883|198^2892|HR2892|75086|-543994|596^2893|HR2893|75681|105683|628^2894|HR2894|76131|557553|592^2895|HR2895|75395|-359614|630^2896|HR2896|75858|309611|533^2897|HR2897|75561|-143383|621^2898|HR2898|75989|430311|630^2899|HR2899|75554|-194125|566^2900|HR2900|75527|-247108|585^2901|δ³ CMi|75711|33714|581^2902|HR2902|75633|-145239|497^2903|HR2903|76088|461803|565^2904|HR2904|75794|27250|655^2905|υ Gem|75987|268958|406^2906|HR2906|75676|-222961|445^2907|HR2907|75538|-400589|626^2908|HR2908|75537|-430864|652^2909|HR2909|75718|-234736|583^2910|HR2910|75720|-234747|587^2911|HR2911|75642|-363383|554^2912|HR2912|75747|-261167|665^2913|HR2913|75702|-334633|611^2914|HR2914|76316|487736|592^2915|HR2915|76216|400253|638^2916|HR2916|75764|-270119|577^2917|HR2917|75662|-399058|676^2918|HR2918|76096|58617|591^2919|ε Men|74272|-790942|553^2920|HR2920|76046|-83114|627^2921|HR2921|76011|-144928|570^2922|HR2922|75897|-283694|464^2923|HR2923|76022|-221606|634^2924|70 Gem|76424|350486|556^2925|HR2925|75776|-514747|628^2926|HR2926|76374|243603|627^2927|25 Mon|76213|-41111|513^2928|HR2928|76114|-197022|574^2929|23 Lyn|76804|570828|606^2930|ο Gem|76528|345842|490^2931|HR2931|76533|242225|617^2932|HR2932|76275|-144411|653^2933|HR2933|76213|-237750|637^2934|HR2934|75944|-525339|494^2935|HR2935|76707|383444|573^2936|HR2936|76650|320097|617^2937|HR2937|76228|-349686|453^2938|74 Gem|76579|176747|505^2939|HR2939|76868|481317|556^2940|HR2940|76122|-488303|572^2941|HR2941|76005|-558875|639^2942|HR2942|76291|-352772|660^2943|Procyon|76550|52250|38^2944|HR2944|76383|-253647|470^2945|HR2945|76292|-380106|638^2946|24 Lyn|77168|587103|499^2947|HR2947|76520|-186792|672^2948|Markab|76470|-268017|450^2949|HR2949|76472|-268036|462^2950|HR2950|76686|52308|602^2951|HR2951|76829|230186|589^2952|HR2952|76401|-399914|659^2953|HR2953|76798|137708|624^2954|HR2954|7645)BSC5";
static const char BSC5_CHUNK_30[] PROGMEM = R"BSC5(5|-364969|580^2955|HR2955|76424|-387811|619^2956|HR2956|76575|-268631|650^2957|HR2957|76384|-486011|568^2958|HR2958|76765|-81858|601^2959|HR2959|76731|-152636|494^2960|HR2960|76704|-196608|593^2961|HR2961|76576|-383083|484^2962|HR2962|77121|340003|602^2963|HR2963|76622|-381394|573^2964|HR2964|76633|-382608|576^2965|HR2965|76977|134806|577^2966|HR2966|76931|36247|594^2967|HR2967|77009|142083|556^2968|HR2968|76661|-375794|600^2969|HR2969|77345|504339|527^2970|α Mon|76874|-95511|393^2971|HR2971|76501|-532733|606^2972|HR2972|76787|-279458|676^2973|σ Gem|77219|288836|428^2974|HR2974|76813|-316608|656^2975|51 Cam|77778|654558|592^2976|HR2976|76899|-223372|618^2977|49 Cam|77743|628306|649^2978|HR2978|77228|223994|621^2979|HR2979|75894|-742756|716^2980|HR2980|75894|-742756|726^2981|HR2981|76877|-385336|542^2982|HR2982|77182|1894|619^2983|76 Gem|77352|257842|531^2984|HR2984|76894|-446322|641^2985|κ Gem|77408|243981|357^2986|HR2986|76994|-385289|654^2987|HR2987|77372|128594|643^2988|HR2988|77134|-263511|564^2989|HR2989|77354|24050|647^2990|Pollux|77552|280261|114^2991|79 Gem|77526|203164|633^2992|HR2992|77275|-255039|655^2993|1 Pup|77257|-284111|459^2994|HR2994|77200|-360503|560^2995|HR2995|77186|-388642|689^2996|3 Pup|77301|-289547|396^2997|HR2997|79381|802656|656^2998|HR2998|77159|-451733|506^2999|HR2999|77776|375175|518^3000|HR3000|76012|-776342|618^3001|HR3001|77286|-382019|640^3002|HR3002|77283|-409339|517^3003|81 Gem|77687|185100|488^3004|HR3004|77428|-246739|562^3005|HR3005|77186|-499928|657^3006|HR3006|77028|-586308|643^3007|HR3007|77360|-360628|580^3008|11 CMi|77712|107683|530^3009|2 Pup|77580|-146861|689^3010|2 Pup|77581|-146908|607^3011|HR3011|77428|-379431|588^3012|HR3012|77148|-582300|621^3013|π Gem|77918|334156|514^3014|HR3014|77673|-67725|549^3015|4 Pup|77658|-145639|504^3016|HR3016|77513|-378878|654^3017|HR3017|77542|-379686|361^3018|HR3018|77597|-341731|537^3019|HR3019|77791|-126753|639^3020|HR3020|77550|-437522|603^3021|82 Gem|78093|231411|618^3022|HR3022|77696|-379339|588^3023|HR3023|77868|-225197|590^3024|ζ Vol|76970|-726061|395^3025|HR3025|77759|-400597|657^3026|HR3026|77940|-159908|634^3027|HR3027|77959|-160144|643^3028|HR3028|78516|541292|602^3029|5 Pup|77991|-121931|548^3030|HR3030|78172|133708|604^3031|HR3031|77599|-567225|612^3032|HR3032|77849|-393314|631^3033|HR3033|78164|43328|653^3034|ο Pup|78014|-259372|450^3035|HR3035|77903|-385111|508^3036|HR3036|77455|-660719|638^3037|HR3037|77921|-466086|523^3038|HR3038|77369|-698214|618^3039|HR3039|78768|552094|638^3040|HR3040|78506|332336|603^3041|HR3041|78024|-406522|614^3042|HR3042|78246|-133533|623^3043|HR3043|78171|-249122|533^3044|6 Pup|78281|-172283|518^3045|Azmidiske|78216|-248597|334^3046|HR3046|78056|-470778|471^3047|HR3047|78363|-91833|561^3048|HR3048|78292|-202069|656^3049|HR3049|)BSC5";
static const char BSC5_CHUNK_31[] PROGMEM = R"BSC5(78208|-352433|593^3050|HR3050|78465|32772|618^3051|HR3051|78349|-195236|612^3052|HR3052|78265|-332889|560^3053|HR3053|78658|193253|599^3054|HR3054|78487|-111286|616^3055|HR3055|78206|-463733|411^3056|HR3056|78053|-564711|633^3057|HR3057|78245|-447519|632^3058|HR3058|78202|-468578|584^3059|ζ CMi|78617|17669|514^3060|HR3060|78500|-245283|645^3061|HR3061|78687|32772|631^3062|HR3062|78185|-564106|559^3063|8 Pup|78614|-128194|636^3064|9 Pup|78629|-138981|517^3065|25 Lyn|79081|473861|625^3066|26 Lyn|79119|475647|545^3067|φ Gem|78916|267658|497^3068|HR3068|78619|-211739|563^3069|HR3069|78451|-445797|645^3070|HR3070|78202|-602836|578^3071|HR3071|78400|-505097|591^3072|HR3072|78800|-54281|576^3073|10 Pup|78719|-148464|569^3074|HR3074|78557|-430956|632^3075|HR3075|80032|739181|541^3076|HR3076|78319|-600511|672^3077|HR3077|79408|565044|672^3078|HR3078|78612|-428883|604^3079|HR3079|78710|-347053|501^3080|HR3080|78703|-405758|373^3081|HR3081|78281|-661958|579^3082|HR3082|80797|794797|542^3083|HR3083|79280|354128|623^3084|HR3084|78774|-388631|449^3085|HR3085|78843|-363639|543^3086|85 Gem|79278|198839|535^3087|HR3087|79254|88628|586^3088|HR3088|78749|-543672|570^3089|HR3089|78844|-496131|463^3090|HR3090|78884|-481031|424^3091|HR3091|79031|-358775|549^3092|HR3092|79111|-348469|615^3093|HR3093|79400|44858|617^3094|HR3094|79713|439775|634^3095|1 Cnc|79498|157903|578^3096|HR3096|79205|-309175|644^3097|HR3097|79544|86414|605^3098|HR3098|79545|11269|635^3099|HR3099|79397|-302853|633^3100|HR3100|79168|-525831|638^3101|HR3101|79296|-438450|602^3102|11 Pup|79476|-228800|420^3103|HR3103|79683|72136|641^3104|HR3104|79754|165186|599^3105|HR3105|79148|-573031|563^3106|HR3106|80224|590475|577^3107|HR3107|79401|-407364|678^3108|HR3108|82816|840578|649^3109|53 Cam|80284|603244|601^3110|14 CMi|79724|22247|529^3111|HR3111|79494|-424061|609^3112|HR3112|80419|630903|640^3113|HR3113|79611|-303347|479^3114|HR3114|79494|-435003|535^3115|HR3115|79931|132422|602^3116|HR3116|79551|-441097|509^3117|χ Car|79463|-529822|347^3118|HR3118|79556|-478903|622^3119|HR3119|80433|572736|649^3120|HR3120|79385|-605264|574^3121|HR3121|79644|-455778|517^3122|27 Mon|79956|-36797|493^3123|12 Pup|79849|-233106|511^3124|ω¹ Cnc|80155|253928|583^3125|HR3125|80133|198161|625^3126|HR3126|79474|-591264|625^3127|HR3127|80169|235831|634^3128|3 Cnc|80131|173086|555^3129|HR3129|79707|-492450|441^3130|HR3130|80320|354131|634^3131|HR3131|79978|-183992|461^3132|ω² Cnc|80288|250897|631^3133|HR3133|79726|-514486|644^3134|5 Cnc|80251|164553|599^3135|HR3135|80122|-28817|651^3136|HR3136|80205|48797|565^3137|HR3137|79838|-452161|599^3138|HR3138|79630|-603033|560^3139|HR3139|79535|-632969|614^3140|HR3140|79912|-392972|524^3141|28 Mon|80204|-13925|468^3142|HR3142|79868|-499767|632^3143|HR3143|79871|-499736|634^3144|HR3144|80308|8)BSC5";
static const char BSC5_CHUNK_32[] PROGMEM = R"BSC5(9139|622^3145|HR3145|80377|23344|439^3146|HR3146|80054|-454569|661^3147|HR3147|79807|-608244|581^3148|HR3148|80041|-489814|602^3149|χ Gem|80586|277942|494^3150|HR3150|80406|-63372|633^3151|HR3151|80080|-488714|612^3152|HR3152|79945|-602075|633^3153|HR3153|79938|-605869|517^3154|HR3154|80271|-372836|595^3155|HR3155|80351|-370506|634^3156|HR3156|80139|-541514|587^3157|HR3157|80230|-545150|610^3158|HR3158|80792|188422|615^3159|HR3159|80056|-635675|482^3160|HR3160|80511|-324639|582^3161|HR3161|80254|-554550|628^3162|HR3162|80458|-413100|552^3163|8 Cnc|80846|131181|512^3164|HR3164|80936|275297|621^3165|Naos|80598|-400033|225^3166|HR3166|80582|-429486|629^3167|28 Lyn|81194|432603|626^3168|14 Pup|80782|-197281|613^3169|μ¹ Cnc|81051|226356|599^3170|HR3170|80712|-326750|531^3171|HR3171|79878|-732447|634^3172|HR3172|80971|-5736|641^3173|27 Lyn|81409|515067|484^3174|HR3174|81076|-92450|623^3175|HR3175|81677|582481|593^3176|μ² Cnc|81294|215817|530^3177|HR3177|80958|-335692|614^3178|HR3178|80784|-505906|595^3179|HR3179|80890|-469789|619^3180|HR3180|80844|-531081|553^3181|HR3181|81564|424306|627^3182|HR3182|82136|684742|532^3183|HR3183|81217|-205547|538^3184|12 Cnc|81451|136408|627^3185|Tureis|81257|-243042|281^3186|HR3186|80786|-628361|630^3187|HR3187|81112|-452664|505^3188|ζ Mon|81432|-29839|434^3189|HR3189|81491|-113397|632^3190|HR3190|81454|-203631|636^3191|ψ Cnc|81742|255072|573^3192|16 Pup|81504|-192450|440^3193|HR3193|81893|387314|658^3194|HR3194|81579|-162489|568^3195|HR3195|81438|-376814|637^3196|HR3196|81519|-303225|665^3197|HR3197|84091|824308|632^3198|HR3198|81830|146294|623^3199|HR3199|81528|-354550|620^3200|HR3200|82306|564522|585^3201|HR3201|81879|98211|607^3202|18 Pup|81777|-137992|554^3203|HR3203|81527|-486844|570^3204|HR3204|81600|-441228|521^3205|HR3205|81632|-426406|626^3206|γ¹ Vel|81581|-473458|427^3207|γ² Vel|81589|-473367|178^3208|Tegmine|82035|176478|563^3209|Tegmine|82035|176478|602^3210|ζ² Cnc|82037|176478|620^3211|19 Pup|81879|-129269|472^3212|HR3212|81925|-77725|536^3213|HR3213|81620|-479375|523^3214|HR3214|82061|140039|654^3215|15 Cnc|82191|296567|564^3216|HR3216|83256|757569|554^3217|HR3217|81401|-638011|628^3218|HR3218|81593|-560856|566^3219|HR3219|81838|-372922|644^3220|HR3220|81502|-613025|476^3221|HR3221|82640|603806|645^3222|HR3222|82166|165142|601^3223|ε Vol|81322|-686172|435^3224|HR3224|82283|231378|656^3225|HR3225|81893|-396186|445^3226|HR3226|81905|-429872|475^3227|HR3227|81864|-484619|582^3228|HR3228|82364|176758|647^3229|20 Pup|82222|-157883|499^3230|HR3230|82128|-299108|652^3231|HR3231|82392|130483|638^3232|HR3232|82000|-466442|576^3233|HR3233|82143|-379244|643^3234|HR3234|82086|-462642|603^3235|29 Lyn|82973|595711|564^3236|HR3236|83445|724072|598^3237|HR3237|82249|-358997|478^3238|HR3238|82281|-335692|637^3239|HR3239|8236)BSC5";
static const char BSC5_CHUNK_33[] PROGMEM = R"BSC5(4|-321408|606^3240|HR3240|82329|-363225|508^3241|HR3241|82330|-363411|611^3242|HR3242|82370|-354906|578^3243|HR3243|82341|-403481|444^3244|HR3244|82267|-469919|513^3245|HR3245|83215|625072|571^3246|HR3246|83044|541436|627^3247|HR3247|82261|-501961|551^3248|HR3248|82761|117264|713^3249|Altarf|82753|91856|352^3250|HR3250|82400|-458344|583^3251|HR3251|82646|-309258|621^3252|HR3252|82921|88661|629^3253|HR3253|82664|-359028|616^3254|30 Lyn|83406|577433|589^3255|HR3255|82817|-213203|660^3256|HR3256|82565|-504494|644^3257|21 Pup|82898|-162850|616^3258|HR3258|83414|535744|649^3259|HR3259|83066|-126319|598^3260|HR3260|82544|-629158|516^3261|HR3261|82995|-300033|645^3262|χ Cnc|83344|272178|514^3263|HR3263|83789|606311|641^3264|HR3264|83392|207478|583^3265|HR3265|83209|-101658|632^3266|HR3266|83048|-354517|558^3267|HR3267|83035|-373742|670^3268|λ Cnc|83422|240222|598^3269|HR3269|83305|39478|605^3270|HR3270|83092|-366594|445^3271|HR3271|83370|-9094|618^3272|HR3272|83381|-53292|613^3273|HR3273|83248|-345903|643^3274|HR3274|82988|-591669|642^3275|Alsciaukat|83806|431881|425^3276|HR3276|83409|-229247|613^3277|HR3277|83968|532197|551^3278|HR3278|83556|-16022|650^3279|HR3279|83559|-200792|558^3280|HR3280|83053|-656133|507^3281|HR3281|83652|-175864|575^3282|HR3282|83564|-330544|483^3283|HR3283|83558|-364844|520^3284|20 Cnc|83894|183322|595^3285|HR3285|83751|-61792|615^3286|HR3286|83567|-396208|616^3287|HR3287|84119|420050|602^3288|HR3288|83817|-75433|596^3289|22 Pup|83797|-130547|611^3290|21 Cnc|83987|106319|608^3291|HR3291|83805|-263481|590^3292|HR3292|84180|350114|606^3293|HR3293|83533|-579731|597^3294|HR3294|83754|-484903|482^3295|HR3295|84101|-47169|601^3296|HR3296|83881|-382858|632^3297|1 Hya|84097|-37511|561^3298|HR3298|83521|-641061|612^3299|25 Cnc|84305|170461|614^3300|HR3300|83820|-521239|585^3301|κ¹ Vol|83303|-715150|537^3302|κ² Vol|83335|-715053|565^3303|HR3303|84962|672975|588^3304|φ¹ Cnc|84410|278936|557^3305|HR3305|84265|21022|573^3306|HR3306|84319|75644|513^3307|Avior|83752|-595097|186^3308|HR3308|84153|-231536|568^3309|HR3309|84602|456531|632^3310|φ² Cnc|84463|269344|632^3311|φ² Cnc|84464|269353|630^3312|24 Cnc|84444|245342|702^3313|24 Cnc|84445|245353|781^3314|HR3314|84277|-39064|390^3315|HR3315|84177|-240461|528^3316|HR3316|84220|-210458|601^3317|HR3317|84276|-174394|644^3318|α Cha|83088|-769197|407^3319|27 Cnc|84455|126544|550^3320|HR3320|84321|-149297|598^3321|2 Hya|84409|-39875|559^3322|HR3322|84159|-427694|598^3323|Muscida|85044|607181|336^3324|HR3324|84450|-125344|554^3325|HR3325|84548|-64097|659^3326|HR3326|84311|-421533|547^3327|HR3327|84382|-390594|653^3328|HR3328|84384|-390603|725^3329|28 Cnc|84769|241447|610^3330|HR3330|84253|-517281|517^3331|HR3331|84474|-292153|673^3332|HR3332|85482|693200|631^3333|29 Cnc|84770|142108|595^3334|η Vol|83679)BSC5";
static const char BSC5_CHUNK_34[] PROGMEM = R"BSC5(|-734000|529^3335|HR3335|84592|-208439|656^3336|HR3336|84546|-316731|633^3337|HR3337|84748|-25172|639^3338|HR3338|84721|-88161|643^3339|HR3339|84649|-261325|662^3340|θ Cha|83440|-774844|435^3341|HR3341|84403|-528075|605^3342|HR3342|84808|-97483|600^3343|HR3343|84665|-351139|575^3344|HR3344|84766|-230717|651^3345|HR3345|84815|-209503|667^3346|HR3346|84310|-646008|597^3347|β Vol|84289|-661369|377^3348|HR3348|85222|372658|618^3349|HR3349|84576|-550117|653^3350|HR3350|84601|-530886|509^3351|HR3351|85426|531147|624^3352|HR3352|86135|747236|631^3353|HR3353|84910|-273325|670^3354|2 UMa|85767|651450|547^3355|υ¹ Cnc|85251|240811|575^3356|HR3356|84854|-441606|579^3357|θ Cnc|85266|180944|535^3358|HR3358|84846|-479292|533^3359|HR3359|84910|-447250|499^3360|HR3360|85486|380164|590^3361|HR3361|85318|98144|683^3362|HR3362|85079|-321594|565^3363|HR3363|84960|-463319|599^3364|HR3364|85082|-367211|669^3365|32 Lyn|85561|364364|624^3366|η Cnc|85451|204411|533^3367|HR3367|85253|-195775|542^3368|HR3368|84934|-551911|636^3369|υ² Cnc|85500|240847|636^3370|HR3370|84547|-700933|553^3371|HR3371|85109|-447372|630^3372|34 Cnc|85444|100661|646^3373|HR3373|85235|-390642|631^3374|HR3374|85426|-150294|638^3375|HR3375|85197|-478667|639^3376|HR3376|85625|132572|628^3377|33 Lyn|85788|364194|578^3378|HR3378|85621|47567|587^3379|HR3379|86618|736297|615^3380|HR3380|85704|84519|603^3381|HR3381|85513|-246064|619^3382|HR3382|85249|-543942|634^3383|HR3383|85671|-21517|581^3384|HR3384|85476|-315008|638^3385|HR3385|85496|-346339|636^3386|HR3386|85347|-532122|569^3387|35 Cnc|85887|195900|658^3388|HR3388|85555|-383711|649^3389|HR3389|85606|-388489|596^3390|HR3390|85584|-469711|624^3391|π¹ UMa|86532|650208|564^3392|HR3392|85903|27436|633^3393|HR3393|84055|-809142|569^3394|HR3394|86021|153136|632^3395|HR3395|85975|66200|599^3396|HR3396|85976|66225|725^3397|HR3397|85755|-325986|643^3398|3 Hya|85912|-79822|572^3399|HR3399|85748|-376114|630^3400|HR3400|86395|534014|566^3401|HR3401|86528|599394|648^3402|HR3402|85913|-268436|596^3403|π² UMa|86702|643278|460^3404|HR3404|85868|-399700|647^3405|HR3405|86500|529250|642^3406|36 Cnc|86183|96556|588^3407|HR3407|85788|-499442|501^3408|HR3408|86549|527117|591^3409|HR3409|86386|328019|594^3410|δ Hya|86276|57036|416^3411|HR3411|86242|-49336|619^3412|37 Cnc|86348|95747|653^3413|HR3413|85978|-509700|580^3414|HR3414|85888|-580092|486^3415|HR3415|85876|-582250|526^3416|HR3416|86390|-66625|651^3417|HR3417|85451|-733567|612^3418|Minchir|86459|33414|444^3419|HR3419|86249|-337458|648^3420|η Pyx|86312|-262550|527^3421|HR3421|86222|-401475|655^3422|34 Lyn|86836|458339|537^3423|HR3423|86718|319419|610^3424|HR3424|86568|80172|645^3425|HR3425|86445|-197369|633^3426|HR3426|86274|-429892|414^3427|39 Cnc|86684|200078|639^3428|HR3428|86728|196700|644^3429|ε Cnc|86742|195450|630^34)BSC5";
static const char BSC5_CHUNK_35[] PROGMEM = R"BSC5(30|HR3430|86522|-226619|505^3431|6 Hya|86671|-124753|498^3432|HR3432|86219|-628536|547^3433|ζ Pyx|86618|-295611|489^3434|HR3434|86561|-366069|613^3435|HR3435|86458|-530906|647^3436|HR3436|87167|469011|622^3437|HR3437|86838|-90519|663^3438|β Pyx|86684|-353083|397^3439|HR3439|86720|-402642|520^3440|HR3440|86566|-534397|548^3441|9 Hya|86954|-159433|488^3442|HR3442|86660|-530550|519^3443|HR3443|86537|-603172|636^3444|HR3444|86765|-451914|571^3445|HR3445|86771|-466489|384^3446|HR3446|87027|-119661|645^3447|HR3447|86716|-529219|362^3448|HR3448|86715|-530153|561^3449|Asellus Borealis|87214|214686|466^3450|45 Cnc|87201|126808|564^3451|HR3451|87361|369181|633^3452|HR3452|86870|-473169|477^3453|HR3453|86848|-489225|590^3454|η Hya|87204|33986|430^3455|HR3455|86788|-575453|634^3456|HR3456|86991|-454108|523^3457|HR3457|86769|-597611|433^3458|HR3458|87332|43347|637^3459|HR3459|87279|-72336|462^3460|θ Vol|86514|-703869|520^3461|Asellus Australis|87448|181542|394^3462|HR3462|87044|-480992|551^3463|HR3463|87158|-359433|642^3464|46 Cnc|87559|306978|613^3465|49 Cnc|87458|100817|566^3466|HR3466|87052|-531000|552^3467|HR3467|87071|-531139|486^3468|α Pyx|87265|-331864|368^3469|10 Hya|87504|56806|613^3470|HR3470|88137|667081|620^3471|HR3471|87058|-557742|629^3472|HR3472|87558|-26008|641^3473|HR3473|87487|-211678|611^3474|ι Cnc|87778|287653|657^3475|ι Cnc|87783|287600|402^3476|HR3476|87279|-498228|516^3477|HR3477|87400|-426492|407^3478|HR3478|87674|-20489|570^3479|HR3479|87478|-371472|576^3480|HR3480|87686|-110064|625^3481|50 Cnc|87822|121100|587^3482|ε Hya|87796|64189|338^3483|HR3483|87637|-253875|610^3484|12 Hya|87729|-135478|432^3485|δ Vel|87451|-547083|196^3486|HR3486|87875|-18972|529^3487|HR3487|87671|-460417|391^3488|HR3488|87733|-411256|621^3489|HR3489|87515|-587250|621^3490|HR3490|87803|-346228|637^3491|HR3491|87318|-682117|632^3492|ρ Hya|88072|58378|436^3493|HR3493|88014|-65586|609^3494|HR3494|87752|-459128|546^3495|HR3495|87417|-658256|605^3496|HR3496|87886|-461556|575^3497|HR3497|87946|-417372|636^3498|HR3498|87785|-567697|449^3499|HR3499|88423|332853|625^3500|14 Hya|88227|-34431|531^3501|HR3501|88024|-424639|643^3502|η Cha|86888|-789633|547^3503|HR3503|88001|-528506|630^3504|HR3504|88459|188322|616^3505|5 UMa|88896|619622|573^3506|HR3506|88850|590561|625^3507|HR3507|88291|-210486|647^3508|35 Lyn|88658|437267|515^3509|HR3509|88699|453125|599^3510|54 Cnc|88504|153506|638^3511|HR3511|88694|420025|599^3512|HR3512|88310|-327806|521^3513|HR3513|88339|-294631|587^3514|HR3514|88276|-403206|548^3516|HR3516|88393|-286181|617^3517|HR3517|88312|-391417|639^3518|γ Pyx|88422|-277100|401^3519|σ¹ Cnc|88763|324742|566^3520|HR3520|88299|-453081|493^3521|53 Cnc|88746|282592|623^3522|ρ¹ Cnc|88766|283308|595^3523|15 Hya|88596|-71772|554^3524|HR3524|87201|-790697|605^3525|HR3525|8839)BSC5";
static const char BSC5_CHUNK_36[] PROGMEM = R"BSC5(2|-420900|600^3526|HR3526|88734|53400|633^3527|HR3527|88426|-465292|510^3528|HR3528|88988|355383|614^3529|HR3529|88752|-132336|613^3530|HR3530|88578|-425044|655^3531|6 UMa|89438|646039|558^3532|57 Cnc|89041|305794|539^3533|HR3533|88739|-325092|650^3534|HR3534|88774|-365456|642^3535|HR3535|88800|-387242|582^3536|HR3536|88602|-576336|559^3537|HR3537|88430|-667931|535^3538|HR3538|89050|-54344|600^3539|HR3539|88774|-483592|591^3540|ρ² Cnc|89277|279275|522^3541|HR3541|89230|172314|664^3542|HR3542|88780|-521292|639^3543|HR3543|87653|-795044|579^3544|HR3544|88306|-725508|611^3545|HR3545|89472|456319|574^3546|HR3546|89418|402017|589^3547|Hydrobius|89232|59456|311^3548|HR3548|88974|-404475|647^3549|HR3549|88844|-566494|603^3550|60 Cnc|89321|116261|541^3551|HR3551|88974|-475208|533^3552|17 Hya|89249|-79703|691^3553|17 Hya|89249|-79711|667^3554|HR3554|89201|-182414|575^3555|σ² Cnc|89491|329103|545^3556|δ Pyx|89254|-276819|489^3557|HR3557|89436|42367|614^3558|HR3558|89523|171439|617^3559|HR3559|89322|-238183|639^3560|HR3560|88969|-603542|578^3561|ο¹ Cnc|89541|153228|520^3562|HR3562|89220|-450417|626^3563|61 Cnc|89663|302336|629^3564|HR3564|89428|-167094|596^3565|ο² Cnc|89598|155814|567^3566|HR3566|89743|358025|651^3567|HR3567|89617|93878|619^3568|HR3568|89150|-582400|638^3569|Talitha|89868|480417|314^3570|HR3570|89200|-549656|571^3571|HR3571|89174|-606447|384^3572|Acubens|89748|118578|425^3573|HR3573|89689|15417|659^3574|HR3574|89387|-527236|469^3575|σ³ Cnc|89924|324186|520^3576|ρ UMa|90424|676297|476^3577|HR3577|89863|181347|638^3578|HR3578|89789|-161328|586^3579|HR3579|90107|417828|397^3580|HR3580|90086|376044|644^3581|HR3581|92559|841811|633^3582|HR3582|89496|-592294|492^3583|HR3583|89655|-485733|587^3584|HR3584|89944|-192081|618^3585|HR3585|89877|-288061|625^3586|HR3586|90279|397133|636^3587|66 Cnc|90234|322525|582^3588|HR3588|89812|-472347|518^3589|67 Cnc|90302|279028|607^3590|HR3590|90254|56408|607^3591|HR3591|90015|-412539|445^3592|HR3592|90668|542839|575^3593|HR3593|90062|-431733|607^3594|Talitha Australis|90604|471567|360^3595|ν Cnc|90456|244528|545^3596|HR3596|90328|-4828|567^3597|HR3597|90198|-266639|620^3598|HR3598|89900|-590836|516^3599|HR3599|90458|72981|585^3600|HR3600|90224|-418644|555^3601|70 Cnc|90694|278983|638^3602|HR3602|90351|-394025|627^3603|HR3603|90900|485303|595^3604|HR3604|90127|-609639|579^3605|HR3605|90291|-521883|523^3606|HR3606|90820|323769|646^3607|HR3607|90524|-255044|674^3608|HR3608|91120|593444|645^3609|σ¹ UMa|91399|668733|514^3610|HR3610|90190|-686839|588^3611|HR3611|90514|-535497|640^3612|HR3612|91088|384522|456^3613|ω Hya|90996|50922|497^3614|HR3614|90692|-470978|375^3615|α Vol|90408|-663961|400^3616|σ² UMa|91731|671347|480^3617|HR3617|91241|229811|640^3618|HR3618|91166|14628|617^3619|15 UMa|91479|516047|448^3620|HR3620|9134)BSC5";
static const char BSC5_CHUNK_37[] PROGMEM = R"BSC5(5|325406|650^3621|τ Cnc|91334|296542|543^3622|HR3622|90800|-578525|644^3623|κ Cnc|91291|106681|524^3624|τ UMa|91820|635136|467^3625|HR3625|91475|338822|593^3626|75 Cnc|91465|266292|598^3627|ξ Cnc|91560|220456|514^3628|κ Pyx|91341|-258583|458^3629|HR3629|91094|-558033|611^3630|19 Hya|91451|-85894|560^3631|HR3631|91208|-512119|673^3632|HR3632|91021|-644997|637^3633|HR3633|92342|716558|655^3634|Suhail|91333|-434325|221^3635|HR3635|91629|115644|648^3636|HR3636|91532|-123578|577^3637|HR3637|91454|-267678|615^3638|HR3638|91512|-183286|573^3639|HR3639|91774|309631|595^3640|79 Cnc|91725|219964|601^3641|20 Hya|91599|-87878|546^3642|HR3642|90940|-705389|471^3643|HR3643|90858|-726028|448^3644|ε Pyx|91657|-303653|559^3645|HR3645|92646|729461|596^3646|HR3646|91731|-231767|653^3647|HR3647|91625|-494247|648^3648|16 UMa|92391|614233|513^3649|HR3649|91988|54683|635^3650|π¹ Cnc|92049|149961|651^3651|HR3651|92036|38672|614^3652|36 Lyn|92301|432178|532^3653|HR3653|91996|-197478|573^3654|HR3654|91846|-448681|500^3655|21 Hya|92072|-71097|611^3656|HR3656|91947|-392589|600^3657|HR3657|92270|212833|648^3658|HR3658|91926|-465839|579^3659|HR3659|91828|-589669|344^3660|17 UMa|92639|567414|527^3661|HR3661|92085|-436136|557^3662|18 UMa|92698|540219|483^3663|HR3663|91879|-623172|397^3664|HR3664|92540|346336|597^3665|θ Hya|92394|23142|388^3666|HR3666|93322|740164|650^3667|HR3667|92239|-386164|631^3668|HR3668|92218|-422736|629^3669|π² Cnc|92538|149414|534^3670|HR3670|92262|-473386|592^3672|HR3672|92356|-441458|585^3673|HR3673|92154|-594144|554^3674|HR3674|92401|-432275|525^3675|HR3675|92569|-150247|635^3676|HR3676|92920|468172|597^3677|HR3677|92492|-376025|586^3678|ζ Oct|89448|-856631|542^3679|HR3679|92383|-555697|527^3680|HR3680|92541|-455556|625^3681|23 Hya|92783|-63531|524^3682|HR3682|92602|-385700|494^3683|24 Hya|92781|-87447|547^3684|HR3684|92625|-374133|462^3685|Miaplacidus|92200|-697172|168^3686|HR3686|93072|353642|575^3687|HR3687|92854|-145736|584^3688|HR3688|92678|-448986|604^3689|HR3689|92976|115011|641^3690|38 Lyn|93141|368025|382^3691|HR3691|92549|-583886|602^3692|HR3692|92731|-442658|512^3693|HR3693|92597|-575781|632^3694|HR3694|92826|-394014|533^3695|HR3695|92034|-766631|614^3696|HR3696|92701|-575414|434^3697|HR3697|93455|512661|613^3698|HR3698|93620|566992|547^3699|Aspidiske|92848|-592753|225^3700|HR3700|92951|-544953|633^3701|HR3701|93498|381883|612^3702|HR3702|93260|-113142|662^3703|HR3703|93016|-510511|526^3704|HR3704|93259|-158344|578^3705|α Lyn|93509|343925|313^3706|26 Hya|93296|-119750|479^3707|HR3707|93576|329019|616^3708|HR3708|93117|-515606|587^3709|27 Hya|93414|-95558|480^3710|HR3710|93300|-341033|639^3711|HR3711|93543|153711|653^3712|HR3712|92881|-686894|539^3713|HR3713|92977|-670508|611^3714|HR3714|93488|-156178|633^3715|HR3715|93456|-317606|682^3716|HR371)BSC5";
static const char BSC5_CHUNK_38[] PROGMEM = R"BSC5(6|93416|-375814|605^3717|HR3717|93257|-551867|628^3718|θ Pyx|93582|-259656|472^3719|HR3719|94643|750983|629^3720|HR3720|92904|-748944|529^3721|HR3721|92909|-747347|586^3722|HR3722|94289|639408|628^3723|HR3723|93922|251831|641^3724|HR3724|93808|-98389|653^3725|HR3725|94155|515739|631^3726|HR3726|93641|-421950|558^3727|HR3727|94062|365869|667^3728|HR3728|93491|-624047|481^3729|HR3729|93769|-397747|654^3730|HR3730|93733|-460475|575^3731|Al Minliar al Asad|94109|261822|446^3732|HR3732|93639|-555150|563^3733|λ Pyx|93867|-288339|469^3734|κ Vel|93686|-550108|250^3735|HR3735|93958|-377572|648^3736|HR3736|94257|165856|629^3737|HR3737|94045|-394258|606^3738|28 Hya|94233|-51175|559^3739|HR3739|93999|-517372|608^3740|HR3740|93909|-603025|630^3741|HR3741|94395|-14639|601^3742|HR3742|94016|-616489|599^3743|HR3743|94778|456014|541^3744|29 Hya|94541|-92236|654^3745|HR3745|94458|-287875|610^3746|HR3746|94412|-405019|620^3747|HR3747|94966|557456|645^3748|Alphard|94598|-86586|198^3749|HR3749|94551|-223439|469^3750|HR3750|94630|-60711|538^3751|HR3751|96181|813264|429^3752|HR3752|94242|-619503|577^3753|HR3753|94384|-533792|511^3754|ω Leo|94743|90567|541^3755|3 Leo|94748|81883|571^3756|HR3756|94607|-350078|665^3757|23 UMa|95255|630619|367^3758|HR3758|94839|-12569|627^3759|τ¹ Hya|94858|-27689|460^3760|HR3760|94901|-22053|614^3761|HR3761|94456|-649297|605^3762|HR3762|94923|-42472|626^3763|HR3763|94868|-207486|566^3764|7 LMi|95120|336556|585^3765|ε Ant|94874|-359514|451^3766|HR3766|94879|-384039|619^3767|HR3767|94972|-233453|624^3768|22 UMa|95816|722056|572^3769|8 LMi|95257|351031|537^3770|HR3770|94985|-265897|548^3771|24 UMa|95747|698303|456^3772|HR3772|95063|-155772|585^3773|Alterf|95287|229681|431^3774|HR3774|96019|743178|646^3775|Sarir|95476|516772|317^3776|HR3776|94797|-622731|592^3777|HR3777|94518|-716022|547^3778|HR3778|95520|494386|676^3779|6 Leo|95327|97158|507^3780|ζ¹ Ant|95126|-318914|700^3781|ζ¹ Ant|95128|-318894|618^3782|ξ Leo|95324|112997|497^3783|HR3783|94752|-667019|591^3784|HR3784|95014|-515172|545^3785|HR3785|95275|-105522|614^3786|ψ Vel|95117|-404667|360^3787|τ² Hya|95330|-11850|457^3788|HR3788|95322|-103706|613^3789|ζ² Ant|95256|-318719|593^3790|HR3790|95258|-357150|587^3791|9 LMi|95584|364869|618^3792|HR3792|95551|283681|653^3793|HR3793|95065|-583617|588^3794|HR3794|95448|18642|611^3795|ι Cha|94025|-807869|536^3796|HR3796|95390|-194003|574^3797|HR3797|95721|469022|652^3798|HR3798|95385|-286281|646^3799|26 UMa|95804|520514|450^3800|10 LMi|95704|363975|455^3801|HR3801|95506|-85053|612^3802|HR3802|95488|-135169|594^3803|HR3803|95204|-570344|313^3804|HR3804|95664|234539|625^3805|HR3805|95556|-71900|624^3806|HR3806|96323|730806|642^3807|HR3807|95387|-406494|535^3808|HR3808|95535|-211158|501^3809|HR3809|95844|396214|481^3810|HR3810|95573|-228639|591^3811|HR381)BSC5";
static const char BSC5_CHUNK_39[] PROGMEM = R"BSC5(1|95896|399633|676^3812|HR3812|95522|-391289|643^3813|HR3813|95258|-667194|627^3814|33 Hya|95758|-59150|556^3815|11 LMi|95943|358103|541^3816|HR3816|95374|-627889|610^3817|HR3817|95624|-490050|512^3818|7 Leo|95980|143797|636^3819|HR3819|95691|-512553|501^3820|HR3820|96119|311617|556^3821|HR3821|95268|-730808|547^3822|HR3822|95927|-195836|631^3823|HR3823|95866|-358239|649^3824|HR3824|96578|672722|594^3825|HR3825|95741|-592294|408^3826|8 Leo|96174|164378|569^3827|10 Leo|96202|68358|500^3828|HR3828|96094|-247028|653^3829|42 Lyn|96394|402397|525^3830|HR3830|96167|-252967|570^3831|HR3831|96070|-487514|617^3832|34 Hya|96310|-94244|640^3833|HR3833|96194|-321786|563^3834|HR3834|96409|46492|468^3835|HR3835|96245|-360958|598^3836|HR3836|96138|-493553|435^3837|HR3837|96129|-529442|619^3838|HR3838|97041|692375|569^3839|27 UMa|97159|722525|517^3840|HR3840|96201|-536686|545^3841|HR3841|96014|-649506|656^3842|HR3842|96337|-431911|550^3843|HR3843|97586|781347|623^3844|HR3844|96446|-396142|670^3845|ι Hya|96643|-11428|391^3846|37 Hya|96632|-105703|631^3847|HR3847|97883|791367|617^3848|HR3848|96722|-107692|637^3849|κ Hya|96718|-143322|506^3850|HR3850|96931|312781|589^3851|43 Lyn|97001|397578|562^3852|Subra|96858|98922|352^3853|13 Leo|96940|259128|624^3854|HR3854|97120|484311|639^3855|HR3855|97186|543636|647^3856|HR3856|96558|-613281|452^3857|13 LMi|97119|350933|614^3858|HR3858|96881|-235917|477^3859|HR3859|97435|649839|617^3860|ζ Cha|95648|-809414|511^3861|15 Leo|97259|299744|564^3862|HR3862|97040|-239156|494^3863|HR3863|96785|-579836|532^3864|HR3864|96839|-572597|580^3865|28 UMa|97654|636533|634^3866|ψ Leo|97289|140217|535^3867|HR3867|97115|-355017|641^3868|HR3868|96966|-552142|600^3869|HR3869|97417|188636|650^3870|HR3870|97755|571281|520^3871|θ Ant|97367|-277694|479^3872|HR3872|97243|-512283|615^3873|Ras Elased Australis|97642|237742|298^3874|HR3874|97377|-395711|682^3875|HR3875|97284|-538917|556^3876|HR3876|97694|67086|579^3877|18 Leo|97731|118100|563^3878|HR3878|97561|-302028|645^3879|HR3879|97732|17856|565^3880|19 Leo|97905|115683|645^3881|HR3881|98098|460211|509^3882|HR3882|97926|114289|602^3883|HR3883|97612|-571856|646^3884|HR3884|97541|-625078|369^3885|HR3885|98399|655933|631^3886|HR3886|97751|-447550|555^3887|HR3887|97654|-587942|622^3888|υ UMa|98498|590386|380^3889|20 Leo|98306|211794|609^3890|υ Car|97850|-650719|301^3891|υ Car|97852|-650725|626^3892|HR3892|98245|-371864|597^3893|4 Sex|98417|43436|624^3894|φ UMa|98684|540644|459^3895|HR3895|98111|-564119|606^3896|23 Leo|98506|130661|646^3897|HR3897|98309|-362686|637^3898|HR3898|98325|-457328|508^3899|6 Sex|98539|-42433|601^3900|22 Leo|98647|243953|532^3901|HR3901|98560|-61817|642^3902|ν Cha|97724|-767761|545^3903|υ¹ Hya|98580|-148467|412^3904|HR3904|98450|-469344|573^3905|Rasalas|98794|260069|388^3906|7 Sex|9)BSC5";
static const char BSC5_CHUNK_40[] PROGMEM = R"BSC5(8701|24542|602^3907|HR3907|98700|756|635^3908|HR3908|98666|-165347|608^3909|γ Sex|98751|-81050|505^3910|HR3910|98555|-461939|562^3911|HR3911|99176|611161|627^3912|HR3912|98613|-465478|458^3913|HR3913|98534|-594258|579^3914|HR3914|98488|-627453|557^3915|HR3915|98952|59583|595^3916|HR3916|98828|-273322|630^3917|31 UMa|99286|498200|527^3918|HR3918|99730|728794|583^3919|HR3919|99034|-259325|488^3920|HR3920|98833|-553733|648^3921|HR3921|99088|-224883|624^3922|HR3922|99538|574183|593^3923|HR3923|99145|-190094|494^3924|HR3924|98973|-511469|593^3925|HR3925|99049|-452839|571^3926|HR3926|99406|89331|585^3927|HR3927|99143|-502439|572^3928|19 LMi|99614|410556|514^3929|HR3929|99658|454144|630^3930|HR3930|99348|-408247|641^3931|HR3931|99463|-265503|628^3932|HR3932|99432|-334186|584^3933|HR3933|99483|-274750|632^3934|HR3934|101429|839183|637^3935|HR3935|99394|-513361|637^3936|HR3936|99739|277589|630^3937|ν Leo|99704|124447|526^3938|HR3938|99688|83142|604^3939|HR3939|99977|568119|548^3940|φ Vel|99477|-545678|354^3941|HR3941|99530|-526389|612^3942|HR3942|99934|296453|573^3943|HR3943|99618|-484144|605^3944|HR3944|99360|-713894|635^3945|12 Sex|99953|33847|670^3946|HR3946|99850|-239503|621^3947|η Ant|99812|-358911|523^3948|HR3948|99542|-644894|658^3949|HR3949|99499|-691019|620^3950|π Leo|100036|80442|470^3951|20 LMi|100169|319236|536^3952|HR3952|100469|219492|566^3953|HR3953|100096|-569467|652^3954|HR3954|100768|538917|574^3955|HR3955|100279|-533644|620^3956|HR3956|100470|-305775|654^3957|HR3957|100328|-573497|620^3958|HR3958|100862|523708|614^3959|HR3959|100614|-95739|612^3960|HR3960|100333|-604208|594^3961|13 Sex|100690|32011|645^3962|HR3962|100615|-253167|670^3963|HR3963|100675|-181017|586^3964|HR3964|100557|-466361|612^3965|HR3965|100725|-242856|570^3966|HR3966|100500|-601786|619^3967|HR3967|100471|-621564|642^3968|HR3968|100732|-399758|643^3969|HR3969|100947|157575|637^3970|υ² Hya|100854|-130647|460^3971|HR3971|100595|-618839|614^3972|HR3972|100876|-363839|627^3973|14 Sex|101132|56114|621^3974|21 LMi|101238|352447|448^3975|η Leo|101222|167628|352^3976|HR3976|101031|-473700|508^3977|HR3977|101193|-171417|560^3978|HR3978|101020|-521881|652^3979|HR3979|101377|316042|624^3980|31 Leo|101318|99975|437^3981|α Sex|101323|-3717|449^3982|Regulus|101395|119672|135^3983|μ¹ Cha|100121|-822147|552^3984|HR3984|101338|-373336|636^3985|HR3985|101460|-108847|653^3986|HR3986|101432|-156117|627^3987|HR3987|101830|406614|632^3988|HR3988|101657|-120958|624^3989|17 Sex|101688|-84083|591^3990|HR3990|101490|-518111|486^3991|HR3991|101683|-128161|531^3992|HR3992|101588|-358567|613^3993|HR3993|101869|374019|585^3994|λ Hya|101765|-123542|361^3995|HR3995|101452|-658153|528^3996|18 Sex|101822|-84183|565^3997|μ² Cha|100688|-815658|660^3998|34 Leo|101939|133550|644^3999|HR3999|101561|-615492|560^)BSC5";
static const char BSC5_CHUNK_41[] PROGMEM = R"BSC5(4000|HR4000|101883|-73167|625^4001|HR4001|101772|-417150|598^4002|HR4002|101585|-686833|581^4003|HR4003|102008|-286064|628^4004|19 Sex|102134|46147|577^4005|HR4005|102105|-191536|644^4006|HR4006|102305|271358|604^4007|HR4007|101931|-588281|640^4008|HR4008|102521|599856|625^4009|HR4009|101963|-580606|572^4010|HR4010|102064|-521633|616^4011|HR4011|102221|-270289|625^4012|HR4012|102416|211678|602^4013|HR4013|102236|-330319|638^4014|22 LMi|102518|314681|646^4015|HR4015|102294|-403458|590^4016|HR4016|103003|730733|640^4017|HR4017|102230|-512333|528^4018|HR4018|102170|-599181|610^4019|HR4019|102324|-403106|635^4020|HR4020|102244|-517561|578^4021|HR4021|102974|710606|666^4022|HR4022|102226|-616589|641^4023|HR4023|102456|-421219|385^4024|23 LMi|102707|293106|535^4025|HR4025|102252|-663731|516^4026|32 UMa|103006|651083|582^4027|24 LMi|102745|286825|649^4028|HR4028|102711|177403|655^4029|HR4029|102558|-365181|619^4030|35 Leo|102756|235031|597^4031|Adhafera|102782|234172|344^4032|HR4032|102783|253714|584^4033|Tania Borealis|102849|429144|345^4034|HR4034|102692|-112033|608^4035|37 Leo|102780|137283|541^4036|HR4036|102587|-431125|560^4037|ω Car|102289|-700381|332^4038|HR4038|102546|-549742|616^4039|39 Leo|102874|231061|582^4040|HR4040|102793|-206706|657^4041|HR4041|103029|274153|652^4042|ε Sex|102938|-80689|524^4043|HR4043|102675|-599033|622^4044|HR4044|103164|467608|643^4045|HR4045|102778|-512050|630^4046|HR4046|103241|483969|600^4047|HR4047|103509|687475|596^4048|HR4048|103169|247117|640^4049|HR4049|103021|-289919|534^4050|HR4050|102847|-613322|340^4051|HR4051|103374|537792|645^4052|HR4052|103420|542169|600^4053|HR4053|103105|-368047|630^4054|40 Leo|103289|194708|479^4055|HR4055|103213|-125281|600^4056|HR4056|103078|-416683|596^4057|Algieba|103329|198417|261^4058|γ² Leo|103329|198406|380^4059|HR4059|103256|-51058|637^4060|HR4060|103332|-90589|632^4061|HR4061|103104|-561100|581^4062|HR4062|104949|842522|550^4063|HR4063|103269|-550294|457^4064|23 Sex|103506|22897|666^4065|HR4065|103180|-646764|567^4066|HR4066|103380|-476992|565^4067|HR4067|103696|412294|576^4068|HR4068|103522|-179850|651^4069|Tania Australis|103721|414994|305^4070|42 Leo|103640|149756|612^4071|HR4071|103580|-237108|650^4072|HR4072|104022|655664|497^4073|HR4073|103600|-225283|651^4074|HR4074|103486|-560431|450^4075|27 LMi|103851|339081|590^4076|HR4076|103702|-198669|613^4077|43 Leo|103834|65425|607^4078|HR4078|103949|296158|639^4079|HR4079|103874|56942|654^4080|HR4080|103721|-416500|483^4081|28 LMi|104024|337186|550^4082|25 Sex|103907|-40742|597^4083|HR4083|103870|-301622|627^4084|HR4084|105179|825586|526^4085|HR4085|104036|23681|632^4086|HR4086|103915|-380100|533^4087|HR4087|103946|-419533|627^4088|44 Leo|104209|87847|561^4089|HR4089|103828|-669017|499^4090|30 LMi|104319|337961|474^4091|HR4091|10)BSC5";
static const char BSC5_CHUNK_42[] PROGMEM = R"BSC5(3975|-579539|635^4092|HR4092|104290|-70597|557^4093|HR4093|104214|-424681|618^4094|μ Hya|104348|-168364|381^4095|HR4095|104165|-585764|595^4096|HR4096|104578|416008|602^4097|HR4097|104501|193644|615^4098|HR4098|104677|487847|644^4099|HR4099|104360|-427389|613^4100|β LMi|104647|367072|421^4101|45 Leo|104608|97625|604^4102|HR4102|104066|-740317|400^4103|HR4103|104768|452122|635^4104|α Ant|104525|-310678|425^4105|HR4105|104123|-739717|619^4106|35 UMa|104984|656261|632^4107|HR4107|104469|-548775|558^4108|HR4108|105074|642575|612^4109|HR4109|104789|-37425|605^4110|HR4110|104568|-576389|466^4111|HR4111|104672|-494056|610^4112|36 UMa|105104|559806|484^4113|32 LMi|105018|389253|577^4114|HR4114|104646|-587394|382^4115|HR4115|104570|-657047|601^4116|δ Sex|104913|-27392|521^4117|HR4117|104914|-296636|558^4118|δ Ant|104932|-306072|556^4119|β Sex|105049|-6369|509^4120|HR4120|104813|-641722|529^4121|HR4121|106005|804944|652^4122|HR4122|105163|-76375|620^4123|HR4123|105166|-135883|558^4124|33 LMi|105309|323794|590^4125|HR4125|105143|-264839|651^4126|HR4126|105849|757131|484^4127|46 Leo|105366|141372|546^4128|HR4128|105109|-613561|643^4129|HR4129|105024|-669850|619^4130|HR4130|105302|-282375|605^4131|HR4131|105621|534975|645^4132|HR4132|105539|404256|475^4133|ρ Leo|105469|93067|385^4134|HR4134|105227|-537156|489^4135|HR4135|105326|-450667|574^4136|HR4136|105324|-450694|609^4137|34 LMi|105586|349886|558^4138|HR4138|105056|-719931|474^4139|HR4139|105427|-446189|591^4140|HR4140|105337|-616853|332^4141|37 UMa|105860|570828|516^4142|HR4142|105172|-732217|493^4143|HR4143|105491|-470033|502^4144|HR4144|105466|-586669|600^4145|44 Hya|105669|-237453|508^4146|48 Leo|105800|69536|508^4147|HR4147|105570|-581903|614^4148|49 Leo|105839|86503|567^4149|HR4149|105827|-231761|610^4150|35 LMi|106059|363269|628^4151|HR4151|105702|-609878|623^4152|HR4152|105941|-185692|649^4153|HR4153|105869|-395628|538^4154|HR4154|105862|-436647|608^4155|HR4155|106048|-105833|657^4156|φ² Hya|106046|-163444|603^4157|HR4157|106013|-266750|629^4158|HR4158|106090|-122303|570^4159|HR4159|105931|-575578|445^4160|HR4160|106199|-117486|652^4161|HR4161|105308|-819211|707^4162|HR4162|106205|-274125|489^4163|HR4163|106259|-133844|482^4164|HR4164|106056|-595647|508^4165|HR4165|106516|536683|552^4166|37 LMi|106453|319761|471^4167|HR4167|106217|-482258|384^4168|38 LMi|106521|379100|585^4169|HR4169|106241|-587333|545^4170|HR4170|105902|-763092|630^4171|φ³ Hya|106431|-168767|491^4172|HR4172|106473|-124436|604^4173|HR4173|106340|-572564|591^4174|γ Cha|105911|-786078|411^4175|HR4175|106473|-427536|611^4176|HR4176|106968|684433|575^4177|HR4177|106459|-591831|466^4178|38 UMa|106991|657164|512^4179|HR4179|106498|-588169|592^4180|HR4180|106551|-556033|428^4181|HR4181|107178|690761|500^4182|33 Sex|106901|-17417|626^4183|HR41)BSC5";
static const char BSC5_CHUNK_43[] PROGMEM = R"BSC5(83|106810|-357417|637^4184|HR4184|107031|316969|602^4185|HR4185|106698|-651006|552^4186|HR4186|106546|-744936|607^4187|39 UMa|107287|571992|580^4188|HR4188|106882|-596769|642^4189|40 LMi|107172|263256|551^4190|HR4190|107087|-139750|624^4191|HR4191|107258|462039|518^4192|41 LMi|107236|231883|508^4193|35 Sex|107225|47478|579^4194|HR4194|107120|-327158|564^4195|HR4195|107511|674114|600^4196|HR4196|107039|-644664|482^4197|HR4197|107374|197586|627^4198|HR4198|107113|-592158|538^4199|θ Car|107159|-643944|276^4200|HR4200|107256|-605667|457^4201|36 Sex|107526|24881|628^4202|41 UMa|107729|573658|634^4203|42 LMi|107644|306822|524^4204|HR4204|107309|-642489|577^4205|HR4205|107352|-639611|482^4206|HR4206|106976|-797833|597^4207|HR4207|107682|63731|637^4208|51 Leo|107735|188914|549^4209|52 Leo|107737|141947|548^4210|η Car|107510|-596842|621^4211|HR4211|107387|-708600|626^4212|HR4212|107422|-708550|646^4213|HR4213|107407|-724439|627^4214|HR4214|107811|-172967|542^4215|HR4215|108139|651322|639^4216|μ Vel|107795|-494200|269^4217|HR4217|107713|-606033|625^4218|HR4218|107939|-152619|667^4219|HR4219|107712|-645150|534^4220|HR4220|107749|-642633|523^4221|HR4221|107826|-567572|523^4222|HR4222|107809|-643833|485^4223|43 LMi|108159|294158|615^4224|HR4224|108113|-19589|593^4225|HR4225|108039|-316883|588^4226|HR4226|107941|-574678|636^4227|53 Leo|108209|105453|534^4228|HR4228|108015|-599192|600^4229|40 Sex|108215|-40242|661^4230|44 LMi|108316|279739|604^4231|δ¹ Cha|107544|-804697|547^4232|ν Hya|108271|-161936|311^4233|HR4233|108288|-98528|586^4234|δ² Cha|107630|-805403|445^4235|43 UMa|108531|565822|567^4236|42 UMa|108566|593200|558^4237|41 Sex|108384|-88978|579^4238|HR4238|108325|-340581|561^4239|HR4239|108234|-593236|591^4240|HR4240|108515|-30925|595^4241|HR4241|108752|525653|665^4242|HR4242|108755|525036|644^4243|HR4243|108919|698539|593^4244|HR4244|108705|10253|638^4245|HR4245|108767|-2014|631^4246|44 UMa|108929|545850|510^4247|Praecipua|108885|342150|383^4248|ω UMa|108996|431900|471^4249|HR4249|108902|-22553|612^4250|HR4250|108753|-572406|525^4251|HR4251|108915|-201389|524^4252|HR4252|108925|-154456|638^4253|HR4253|108955|-21292|545^4254|48 LMi|109117|254908|620^4255|HR4255|109049|-137581|566^4256|HR4256|109162|340347|572^4257|HR4257|108916|-588533|378^4258|46 UMa|109290|335069|503^4259|54 Leo|109269|247497|450^4260|54 Leo|109270|247489|630^4261|HR4261|109199|-206650|644^4262|HR4262|108950|-707203|599^4263|HR4263|109169|-422511|611^4264|HR4264|109374|420083|603^4265|55 Leo|109284|7369|591^4266|HR4266|109082|-618267|593^4267|56 Leo|109338|61853|581^4268|HR4268|108743|-795594|633^4269|HR4269|109380|223517|614^4270|50 LMi|109429|255000|635^4271|HR4271|109214|-605169|592^4272|HR4272|109991|777700|620^4273|ι Ant|109453|-371378|460^4274|HR4274|109522|-507650|591^4275|HR4275|10)BSC5";
static const char BSC5_CHUNK_44[] PROGMEM = R"BSC5(9883|518822|617^4276|HR4276|109634|-597319|611^4277|Chalawan|109911|404303|505^4278|HR4278|109924|360931|600^4279|HR4279|109544|-750997|613^4280|HR4280|110041|455261|547^4281|HR4281|109948|117058|655^4282|HR4282|109872|-337372|571^4283|HR4283|110071|515019|643^4284|HR4284|109919|-163539|589^4285|HR4285|110057|429114|602^4286|HR4286|110183|634211|639^4287|Alkes|109962|-182989|408^4288|49 UMa|110140|392122|508^4289|HR4289|110032|-140833|588^4290|HR4290|109872|-613203|616^4291|58 Leo|110093|36175|484^4292|HR4292|109998|-438072|581^4293|HR4293|110026|-422258|439^4294|59 Leo|110124|61014|499^4295|Merak|110307|563825|237^4296|HR4296|110024|-518178|615^4297|HR4297|110159|-157928|634^4298|HR4298|110113|-318394|607^4299|61 Leo|110305|-24847|474^4300|60 Leo|110388|201797|442^4301|Dubhe|110621|617508|179^4302|HR4302|110401|-268314|623^4303|HR4303|110541|-7525|614^4304|HR4304|109869|-815561|671^4305|HR4305|110541|-113036|550^4306|62 Leo|110602|-8|595^4307|HR4307|110545|-319608|646^4308|HR4308|110601|-134344|634^4309|51 UMa|110753|382414|600^4310|χ Leo|110836|73361|463^4311|HR4311|110753|-476792|567^4312|η Oct|109872|-845939|619^4313|HR4313|110817|-358047|543^4314|χ¹ Hya|110889|-272936|494^4315|HR4315|110928|-110889|609^4316|HR4316|110845|-493925|613^4317|χ² Hya|110993|-272878|571^4318|HR4318|111016|-512125|630^4319|65 Leo|111151|19556|552^4320|HR4320|111108|-287278|677^4321|HR4321|111076|-509567|632^4322|64 Leo|111277|233236|646^4323|HR4323|111081|-586753|602^4324|HR4324|111190|-325872|659^4325|HR4325|111090|-624242|461^4326|HR4326|111067|-648397|641^4327|HR4327|111213|-426386|515^4328|HR4328|111318|-301747|654^4329|HR4329|111139|-708781|557^4330|HR4330|111611|672103|606^4331|HR4331|111377|-299728|649^4332|67 Leo|111470|246583|568^4333|HR4333|111553|363094|574^4334|HR4334|111455|-280806|544^4335|ψ UMa|111611|444986|301^4336|HR4336|111607|432075|589^4337|HR4337|111432|-589750|391^4338|HR4338|111428|-619472|513^4339|HR4339|111648|-323675|581^4340|HR4340|112031|682719|640^4341|HR4341|111955|144003|630^4342|HR4342|111819|-584553|688^4343|β Crt|111943|-228258|448^4344|HR4344|112124|548942|663^4345|HR4345|112089|358136|641^4346|HR4346|112041|-324339|638^4347|ψ Crt|112084|-185000|613^4348|HR4348|112096|-217492|640^4349|HR4349|111915|-714364|635^4350|HR4350|112092|-491011|536^4351|HR4351|112278|410886|633^4352|HR4352|112100|-603175|460^4353|HR4353|112158|-497364|611^4354|HR4354|112208|-443722|580^4355|HR4355|112126|-641697|523^4356|69 Leo|112293|-697|542^4357|Zosma|112351|205236|256^4358|HR4358|112338|80606|579^4359|Chertan|112373|154294|334^4360|HR4360|112276|-532317|576^4361|HR4361|112252|-596194|574^4362|72 Leo|112534|230956|463^4363|HR4363|112678|527731|650^4364|HR4364|112483|-437342|621^4365|73 Leo|112644|133075|532^4366|HR4366|112661|128447|667^4367|HR4367|112783|)BSC5";
static const char BSC5_CHUNK_45[] PROGMEM = R"BSC5(494764|588^4368|φ Leo|112777|-36517|447^4369|HR4369|112828|-71347|614^4370|HR4370|112744|-458800|631^4371|75 Leo|112882|20106|518^4372|HR4372|112866|-380144|627^4373|HR4373|112942|-347372|645^4374|Alula Australis|113030|315292|487^4375|Alula Australis|113031|315292|441^4376|HR4376|112953|-365344|668^4377|Alula Borealis|113080|330942|348^4378|HR4378|113058|119847|666^4379|HR4379|112886|-678236|606^4380|55 UMa|113189|381856|478^4381|76 Leo|113153|16506|591^4382|δ Crt|113224|-147786|356^4383|HR4383|113483|671006|621^4384|HR4384|113212|-645825|599^4385|HR4385|113095|-796686|635^4386|σ Leo|113523|60294|405^4387|HR4387|113268|-751425|627^4388|HR4388|113637|570750|643^4389|HR4389|113344|-719944|641^4390|π Cen|113501|-544911|389^4391|HR4391|113809|643306|602^4392|56 UMa|113804|434828|499^4393|HR4393|113731|-446458|612^4394|HR4394|113883|1317|605^4395|λ Crt|113894|-187800|509^4396|HR4396|113869|-361647|500^4397|HR4397|113659|-776083|643^4398|HR4398|113856|-567794|579^4399|ι Leo|113988|105292|394^4400|79 Leo|114006|14078|539^4401|HR4401|113894|-649550|511^4402|ε Crt|114102|-108594|483^4403|HR4403|114061|-426692|612^4404|HR4404|114164|114303|580^4405|γ Crt|114147|-176839|408^4406|HR4406|114031|-722567|559^4407|HR4407|114325|558506|575^4408|81 Leo|114268|164564|557^4409|HR4409|114248|-360631|522^4410|80 Leo|114306|38600|637^4411|HR4411|114259|-377478|589^4412|HR4412|114404|334506|632^4413|HR4413|114287|-639728|517^4414|83 Leo|114459|30131|650^4415|HR4415|114431|-611153|530^4416|κ Crt|114526|-123567|594^4417|HR4417|114465|-531600|581^4418|τ Leo|114656|28561|495^4419|HR4419|114649|-17000|625^4420|HR4420|114662|-353286|645^4421|HR4421|114846|617783|583^4422|57 UMa|114845|393369|531^4423|HR4423|114764|-426742|508^4424|HR4424|114954|567375|628^4425|HR4425|114717|-724744|609^4426|85 Leo|114950|154133|574^4427|HR4427|115036|543617|641^4428|HR4428|114941|-244633|576^4429|HR4429|115307|811272|615^4430|HR4430|115069|466575|635^4431|58 UMa|115086|431733|594^4432|87 Leo|115052|-30036|477^4433|86 Leo|115081|184097|552^4434|Gianfar|115234|693311|384^4435|HR4435|115147|479292|642^4436|HR4436|115195|487892|656^4437|88 Leo|115291|143644|620^4438|HR4438|115208|-612783|638^4439|HR4439|115391|610825|548^4440|HR4440|115299|-207767|624^4441|ο¹ Cen|115295|-594422|513^4442|ο² Cen|115302|-595158|515^4443|HR4443|115378|-292633|581^4444|HR4444|115379|-292611|564^4445|HR4445|115398|-267467|616^4446|HR4446|115465|-78275|595^4447|HR4447|115467|-404364|564^4448|HR4448|115389|-669622|590^4449|HR4449|115484|-310872|504^4450|ξ Hya|115500|-318578|354^4451|HR4451|115541|-162806|605^4452|HR4452|115656|368156|640^4453|HR4453|115604|-405869|539^4454|HR4454|115694|110236|655^4455|89 Leo|115728|30600|577^4456|90 Leo|115785|167969|595^4457|HR4457|115847|547853|563^4458|HR4458|115749|-328314|598^4459|HR4)BSC5";
static const char BSC5_CHUNK_46[] PROGMEM = R"BSC5(459|115844|204414|645^4460|HR4460|115794|-542642|462^4461|2 Dra|116008|693228|520^4462|HR4462|115824|-491367|550^4463|HR4463|115870|-473725|571^4464|HR4464|115954|109111|656^4465|HR4465|116050|277811|580^4466|HR4466|115988|-476417|525^4467|λ Cen|115963|-630197|313^4468|θ Crt|116114|-98022|470^4469|HR4469|116097|-335700|574^4470|HR4470|116113|-372378|631^4471|υ Leo|116158|-8239|430^4472|HR4472|116062|-610522|583^4473|HR4473|116170|-329881|629^4474|HR4474|116314|506183|614^4475|HR4475|116168|-612833|515^4476|HR4476|116261|-477472|544^4477|59 UMa|116391|436256|559^4478|HR4478|116361|88842|617^4479|π Cha|116210|-758967|565^4480|60 UMa|116426|468342|610^4481|HR4481|116470|643469|646^4482|HR4482|116423|336256|627^4483|ω Vir|116410|81342|536^4484|HR4484|116400|-24361|622^4485|HR4485|116301|-676203|596^4486|HR4486|116458|451086|644^4487|HR4487|116354|-618264|515^4488|ι Crt|116445|-132019|548^4489|HR4489|116501|-247211|642^4490|HR4490|116642|-144686|621^4491|HR4491|116640|-166203|619^4492|HR4492|116582|-653978|517^4493|HR4493|116743|579706|637^4494|ο Hya|116702|-347447|470^4495|92 Leo|116797|213528|526^4496|61 UMa|116842|342017|533^4497|HR4497|116785|-539686|596^4498|HR4498|116857|-291964|644^4499|HR4499|116816|-620900|494^4500|HR4500|116954|551725|627^4501|62 UMa|116929|317461|573^4502|HR4502|116888|-430958|555^4503|HR4503|116956|-324997|522^4504|3 Dra|117079|667450|530^4505|HR4505|117014|222108|659^4506|HR4506|117010|-202939|622^4507|HR4507|116837|-831000|633^4508|HR4508|117242|-371903|598^4509|HR4509|117154|-793064|639^4510|HR4510|117320|-66772|607^4511|HR4511|117253|-624894|503^4512|HR4512|117370|252183|602^4513|HR4513|117314|-628783|610^4514|ζ Crt|117461|-183508|473^4515|ξ Vir|117548|82583|485^4516|HR4516|117535|-490697|626^4517|ν Vir|117643|65294|403^4518|Al Kaphrah|117675|477794|371^4519|HR4519|117622|-456900|529^4520|λ Mus|117601|-667286|364^4521|HR4521|117821|556283|527^4522|HR4522|117752|-611783|411^4523|HR4523|117753|-405006|491^4524|HR4524|117853|-359069|617^4525|HR4525|117877|-302869|648^4526|HR4526|117886|-576964|541^4527|93 Leo|117998|202189|453^4528|4 Vir|117986|82458|532^4529|HR4529|118065|-103133|626^4530|μ Mus|118040|-668147|472^4531|HR4531|118108|142842|588^4532|HR4532|118125|-267497|511^4533|HR4533|118170|-3186|615^4534|Denebola|118177|145719|214^4535|HR4535|118208|162428|604^4536|HR4536|118282|349317|570^4537|HR4537|118281|-637883|432^4538|HR4538|118324|-702258|497^4539|HR4539|118388|-158639|613^4540|Zavijava|118449|17647|361^4541|HR4541|118409|-626494|570^4542|HR4542|118436|-272778|648^4543|HR4543|118487|122789|635^4544|HR4544|118506|-53333|564^4545|HR4545|118526|333750|627^4546|HR4546|118524|-451736|446^4547|HR4547|118561|-121878|635^4548|HR4548|118616|-308350|585^4549|HR4549|118642|-652061|490^4550|HR4550|118830|377186|645^4551|HR4551)BSC5";
static const char BSC5_CHUNK_47[] PROGMEM = R"BSC5(|118695|-569878|557^4552|β Hya|118818|-339081|428^4553|HR4553|118908|-350667|617^4554|Phecda|118972|536947|244^4555|HR4555|118973|5519|630^4556|HR4556|119032|-574100|606^4557|HR4557|119072|-377489|646^4558|HR4558|119118|-257139|530^4559|6 Vir|119175|84439|558^4560|65 UMa|119182|464769|654^4561|65 UMa|119198|464697|703^4562|HR4562|119206|367564|649^4563|HR4563|119166|-632789|591^4564|95 Leo|119279|156467|553^4565|HR4565|119278|-284769|593^4566|66 UMa|119329|565986|584^4567|η Crt|119336|-171508|518^4568|HR4568|119319|-396892|613^4569|HR4569|119481|615492|622^4570|HR4570|119455|-470725|626^4571|HR4571|119510|-333153|621^4572|HR4572|119541|403436|662^4573|HR4573|119611|-624489|557^4574|HR4574|119687|322739|642^4575|HR4575|119724|614647|676^4576|HR4576|119709|-563172|544^4577|HR4577|119723|-409472|679^4578|HR4578|119799|-643392|561^4579|HR4579|119818|-259089|643^4580|HR4580|119843|5306|617^4581|HR4581|119882|331675|596^4582|HR4582|119864|-516967|605^4583|ε Cha|119937|-782219|491^4584|HR4584|119992|340350|650^4585|7 Vir|119991|36553|537^4586|HR4586|120052|808531|617^4587|HR4587|120124|-104461|555^4588|HR4588|120118|-218372|628^4589|π Vir|120146|66142|466^4590|HR4590|120142|-196589|526^4591|HR4591|120172|-17681|631^4592|HR4592|120247|-575036|616^4593|HR4593|120276|360419|559^4594|67 UMa|120352|430456|521^4595|HR4595|120389|-856317|605^4596|HR4596|120413|-714889|642^4597|HR4597|120438|-691922|589^4598|HR4598|120477|-76836|622^4599|θ¹ Cru|120504|-633128|433^4600|HR4600|120610|-424342|515^4601|HR4601|120623|-742139|644^4602|2 Com|120713|214592|587^4603|θ² Cru|120720|-631656|472^4604|HR4604|120774|-683292|535^4605|κ Cha|120796|-765192|504^4606|HR4606|120745|855872|627^4607|HR4607|120825|-609692|596^4608|ο Vir|120868|87331|412^4609|Tonatiuh|120875|769058|580^4610|HR4610|120944|629331|613^4611|HR4611|120981|-655472|633^4612|HR4612|120991|-356939|623^4613|HR4613|120999|-31317|637^4614|HR4614|121055|-686508|623^4615|HR4615|121064|-657092|606^4616|η Cru|121147|-646136|415^4617|HR4617|121305|-753669|518^4618|HR4618|121348|-506614|447^4619|HR4619|121347|-507633|637^4620|HR4620|121374|-486928|534^4621|δ Cen|121393|-507225|260^4622|HR4622|121402|-608472|622^4623|Alchiba|121402|-247289|402^4624|HR4624|121483|-443261|575^4625|HR4625|121485|-412314|548^4626|10 Vir|121615|18978|595^4627|HR4627|121631|746614|635^4628|HR4628|121674|-347050|617^4629|11 Vir|121676|58069|572^4630|Minkar|121688|-226197|300^4631|HR4631|121761|-378703|606^4632|3 Com|121754|168092|639^4633|HR4633|121795|272814|601^4634|HR4634|121847|-612775|608^4635|3 Crv|121844|-236025|546^4636|HR4636|121841|-454228|661^4637|HR4637|121921|-513594|623^4638|ρ Cen|121942|-523686|396^4639|HR4639|121833|817100|600^4640|4 Com|121976|258703|566^4641|68 UMa|121958|570544|643^4642|HR4642|122003|285361|649^4643|5 Com|122026|)BSC5";
static const char BSC5_CHUNK_48[] PROGMEM = R"BSC5(205419|557^4644|HR4644|122061|-629508|592^4645|HR4645|122130|-701519|617^4646|HR4646|122033|776164|514^4647|HR4647|122203|-341256|650^4648|HR4648|122237|-389292|576^4649|HR4649|122321|-785736|635^4650|12 Vir|122239|102622|585^4651|HR4651|122269|-337928|633^4652|HR4652|122341|-457239|531^4653|HR4653|122380|-644086|622^4654|HR4654|122454|534347|616^4655|HR4655|122499|-208442|583^4656|δ Cru|122524|-587489|280^4657|HR4657|122529|-103125|611^4658|HR4658|122585|-419131|626^4659|HR4659|122524|702000|571^4660|Megrez|122571|570325|331^4661|HR4661|122631|-233536|654^4662|Gienah|122634|-175419|259^4663|6 Com|122667|148989|510^4664|HR4664|122732|-726147|622^4665|HR4665|122615|725508|629^4666|2 CVn|122688|406603|566^4667|7 Com|122724|239453|495^4668|HR4668|122750|330614|500^4669|HR4669|122850|-656928|606^4670|HR4670|122842|-166936|605^4671|ε Mus|122928|-679608|411^4672|HR4672|122915|531911|581^4673|HR4673|122918|289372|570^4674|β Cha|123058|-793122|426^4675|HR4675|122965|-360939|615^4676|HR4676|122956|151442|634^4677|HR4677|123025|-39544|699^4678|HR4678|123027|-39486|654^4679|ζ Cru|123073|-640031|404^4680|HR4680|123088|302492|623^4681|13 Vir|123112|-7872|590^4682|HR4682|123166|-551431|500^4683|HR4683|122809|864361|633^4684|HR4684|123172|260078|648^4685|8 Com|123220|230347|627^4686|HR4686|122556|877000|628^4687|HR4687|123139|751606|538^4688|9 Com|123249|281569|633^4689|Zaniah|123318|-6669|389^4690|3 CVn|123302|489842|529^4691|HR4691|123363|-221756|597^4692|HR4692|123411|-658428|621^4693|HR4693|123388|266194|554^4694|HR4694|123382|260019|615^4695|16 Vir|123392|33125|496^4696|ζ Crv|123427|-222158|521^4697|11 Com|123453|177928|474^4698|HR4698|123448|270547|713^4699|HR4699|123488|-135656|514^4700|ε Cru|123560|-604011|359^4701|70 UMa|123474|578639|555^4702|HR4702|123660|-563747|592^4703|ζ² Mus|123687|-675219|515^4704|ζ¹ Mus|123700|-683075|574^4705|HR4705|123697|247739|619^4706|HR4706|123804|-576761|539^4707|12 Com|123751|258461|481^4708|17 Vir|123756|53056|640^4709|HR4709|124271|-861506|633^4710|HR4710|123872|-676317|636^4711|6 Crv|123893|-248406|568^4712|HR4712|123932|-354128|532^4713|HR4713|123936|-393031|640^4714|HR4714|123958|-389111|579^4715|4 CVn|123964|425428|606^4716|5 CVn|124004|515622|480^4717|13 Com|124051|260986|518^4718|HR4718|124124|-413842|625^4719|HR4719|124074|255828|642^4720|HR4720|124215|-657706|630^4721|HR4721|124190|-425144|611^4722|HR4722|124199|-116103|595^4723|HR4723|124218|-277492|609^4724|HR4724|124227|-351864|573^4725|HR4725|124209|239261|603^4726|71 UMa|124176|567775|581^4727|HR4727|124184|638028|632^4728|6 CVn|124308|390186|502^4729|HR4729|124419|-631225|486^4730|Acrux|124433|-630992|133^4731|α² Cru|124435|-630994|173^4732|HR4732|124421|-514508|482^4733|14 Com|124400|272683|495^4734|HR4734|124467|-489133|626^4735|HR4735|124477|-328300|555^4)BSC5";
static const char BSC5_CHUNK_49[] PROGMEM = R"BSC5(736|HR4736|124568|-637892|600^4737|γ Com|124490|282683|436^4738|16 Com|124498|268256|500^4739|HR4739|124580|-589919|550^4740|HR4740|124401|719297|624^4741|HR4741|124617|86103|637^4742|HR4742|124637|-166319|635^4743|σ Cen|124673|-502306|391^4744|HR4744|124719|-643414|604^4745|73 UMa|124598|557128|570^4746|HR4746|124643|-46153|622^4747|HR4747|124738|-617953|622^4748|HR4748|124729|-390414|544^4749|HR4749|124759|-564078|615^4750|HR4750|124772|262267|654^4751|HR4751|124791|258992|665^4752|17 Com|124819|259128|529^4753|18 Com|124908|241089|548^4754|HR4754|124983|-565247|580^4755|HR4755|124994|-417361|602^4756|20 Com|124953|208961|569^4757|Algorab|124978|-165156|295^4758|HR4758|125013|-133931|635^4759|HR4759|125049|-236967|563^4760|74 UMa|124992|584058|535^4761|7 CVn|125008|515356|621^4762|75 UMa|125012|587675|608^4763|Gacrux|125194|-571133|163^4764|Gacrux|125213|-570811|642^4765|4 Dra|125019|692011|495^4766|21 Com|125168|245672|546^4767|HR4767|125139|530767|621^4768|HR4768|125279|-594239|548^4769|HR4769|125361|-730017|588^4770|HR4770|125226|76042|605^4771|HR4771|125322|-635061|595^4772|HR4772|125274|-50525|619^4773|γ Mus|125411|-721331|387^4774|HR4774|125346|-325336|646^4775|η Crv|125345|-161961|431^4776|HR4776|125433|-138592|574^4777|20 Vir|125508|102956|626^4778|HR4778|125562|-197919|626^4779|HR4779|125595|-128303|558^4780|22 Com|125595|242831|629^4781|21 Vir|125630|-94519|548^4782|HR4782|125664|-499094|638^4783|HR4783|125608|332475|542^4784|HR4784|125632|333847|624^4785|Chara|125624|413575|426^4786|Kraz|125731|-233967|265^4787|κ Dra|125581|697883|387^4788|HR4788|125784|-446733|577^4789|23 Com|125809|226292|481^4790|HR4790|125914|-618419|622^4791|24 Com|125851|183772|656^4792|24 Com|125855|183769|502^4793|HR4793|125856|218814|585^4794|HR4794|125960|-410219|513^4795|6 Dra|125789|700219|494^4796|HR4796|126003|-398700|580^4797|HR4797|125996|-205272|620^4798|α Mus|126197|-691356|269^4799|25 Vir|126132|-58319|587^4800|HR4800|126065|594869|550^4801|25 Com|126162|170894|568^4802|τ Cen|126284|-485411|386^4803|HR4803|126284|-271389|545^4804|HR4804|126540|-753694|649^4805|HR4805|126346|32825|633^4806|HR4806|126479|-671931|625^4807|HR4807|126396|18547|571^4808|HR4808|126417|69883|708^4809|HR4809|126457|-182503|600^4810|HR4810|126510|-304222|589^4811|9 CVn|126462|408744|637^4812|HR4812|126506|226594|638^4813|χ Vir|126541|-79956|466^4814|HR4814|126654|-665117|626^4815|26 Com|126520|210625|546^4816|HR4816|126547|359519|645^4817|HR4817|126646|-399875|464^4818|HR4818|126897|-461456|584^4819|γ Cen|126919|-489597|217^4820|HR4820|127014|-694075|633^4821|HR4821|126878|-130136|608^4822|HR4822|126878|-130150|598^4823|HR4823|126991|-596858|493^4824|27 Vir|126929|104264|619^4825|Porrima|126943|-14494|365^4826|Porrima|126943|-14494|368^4827|HR4827|126970|-197586|603^4828|ρ Vir|1)BSC5";
static const char BSC5_CHUNK_50[] PROGMEM = R"BSC5(26981|102356|488^4829|31 Vir|126992|68067|559^4830|HR4830|127140|-630586|531^4831|HR4831|127098|-488131|466^4832|HR4832|127138|-559472|608^4833|76 UMa|126928|627131|607^4834|HR4834|127192|-561761|600^4835|HR4835|127245|-589031|640^4836|HR4836|127240|-401778|644^4837|HR4837|127272|-15769|593^4838|HR4838|127330|-363492|639^4839|HR4839|127335|-283239|548^4840|HR4840|127178|611556|638^4841|HR4841|127505|-688311|616^4842|ι Cru|127605|-609811|469^4843|HR4843|127409|441031|633^4844|β Mus|127714|-681081|305^4845|10 CVn|127499|392789|595^4846|HR4846|127522|454403|499^4847|32 Vir|127603|76733|522^4848|HR4848|127730|-564889|465^4849|33 Vir|127729|95400|567^4850|HR4850|127794|-333156|586^4851|27 Com|127774|165775|512^4852|HR4852|127406|806211|640^4853|Mimosa|127953|-596886|125^4854|HR4854|127840|59508|634^4855|34 Vir|127871|119581|607^4856|HR4856|127926|-63019|626^4857|HR4857|127982|-248517|644^4858|35 Vir|127976|35728|641^4859|HR4859|127886|627808|589^4860|HR4860|128073|-275975|566^4861|28 Com|128040|135531|656^4862|HR4862|128291|-719864|555^4863|7 Dra|127929|667903|543^4864|HR4864|128131|248403|631^4865|29 Com|128151|141225|570^4866|11 CVn|128116|484669|627^4867|HR4867|128109|603200|585^4868|HR4868|128367|-604008|675^4869|30 Com|128215|275522|578^4870|ι Oct|129163|-851233|546^4871|HR4871|128388|-484597|624^4872|HR4872|128494|-527875|573^4873|HR4873|128382|228633|643^4874|HR4874|128448|-339994|491^4875|HR4875|128363|375169|589^4876|HR4876|128549|-603297|572^4877|HR4877|128564|-103383|641^4878|37 Vir|128602|30567|602^4879|HR4879|128658|-396808|598^4880|HR4880|128681|-480942|633^4881|HR4881|128661|-267381|615^4882|HR4882|128735|-538294|624^4883|31 Com|128616|275406|494^4884|32 Com|128701|170739|632^4885|HR4885|128844|-549525|593^4886|HR4886|128743|161225|630^4887|HR4887|128894|-603286|576^4888|HR4888|128852|-489433|433^4889|HR4889|128906|-401789|427^4890|κ Cru|128970|-603769|590^4891|38 Vir|128864|-35531|611^4892|HR4892|128185|834181|585^4893|HR4893|128204|834128|528^4894|35 Com|128883|212450|490^4895|HR4895|129061|-584306|658^4896|HR4896|128939|-42239|644^4897|λ Cru|129109|-591467|462^4898|μ¹ Cru|129099|-571778|403^4899|μ² Cru|129102|-571683|517^4900|41 Vir|128971|124186|625^4901|HR4901|129052|-116486|600^4902|ψ Vir|129059|-95389|479^4903|HR4903|129162|-441519|589^4904|HR4904|129036|335344|626^4905|Alioth|129005|559597|177^4906|HR4906|129221|-429158|547^4907|HR4907|129421|-721853|593^4908|HR4908|129325|-568361|532^4909|HR4909|129157|471967|584^4910|Auva|129267|33975|338^4911|HR4911|129315|-153269|617^4912|HR4912|129417|-264603|662^4913|HR4913|129512|-511986|516^4914|α¹ CVn|129334|383147|560^4915|Chara|129338|383183|290^4916|8 Dra|129246|654386|524^4917|HR4917|129382|540994|582^4918|HR4918|129592|-227539|631^4919|HR4919|129521|461769|612^4920|36 Com|129821|174094)BSC5";
static const char BSC5_CHUNK_51[] PROGMEM = R"BSC5(|478^4921|44 Vir|129943|-38119|579^4922|HR4922|130091|-335053|602^4923|δ Mus|130378|-715489|362^4924|37 Com|130046|307850|490^4925|46 Vir|130100|-33686|599^4926|HR4926|130108|183731|620^4927|HR4927|129798|754725|601^4928|9 Dra|129986|665972|532^4929|38 Com|130193|171231|596^4930|HR4930|130514|-714761|603^4931|78 UMa|130122|563664|493^4932|Vindemiatrix|130363|109592|283^4933|ξ¹ Cen|130592|-495272|485^4934|HR4934|130297|636103|600^4935|HR4935|130628|-205831|558^4936|HR4936|130446|597161|653^4937|48 Vir|130651|-36633|659^4938|HR4938|130800|-411967|626^4939|HR4939|130919|-521150|643^4940|HR4940|131046|-484636|471^4941|HR4941|131098|-415886|559^4942|ξ² Cen|131152|-499061|427^4943|14 CVn|130957|357989|525^4944|HR4944|131234|-598606|599^4945|HR4945|130979|452686|563^4946|39 Com|131059|211533|599^4947|HR4947|131151|-358619|654^4948|HR4948|131028|290294|654^4949|40 Com|131063|226161|560^4950|HR4950|130805|730253|631^4951|HR4951|131273|-534597|571^4952|θ Mus|131353|-653064|551^4953|HR4953|131063|620419|614^4954|41 Com|131196|276247|480^4955|49 Vir|131316|-107403|519^4956|HR4956|131316|275558|619^4957|HR4957|131424|-89844|555^4958|ψ Hya|131509|-231181|495^4959|HR4959|131540|-95383|632^4960|HR4960|131534|100222|578^4961|50 Vir|131626|-103294|594^4962|HR4962|131633|168486|591^4963|θ Vir|131658|-55389|438^4964|HR4964|131608|374231|602^4965|HR4965|131829|-525669|606^4966|HR4966|131977|-699419|591^4967|15 CVn|131617|385339|628^4968|Diadem|131665|175294|522^4969|Diadem|131665|175294|522^4970|HR4970|131858|-422331|579^4971|17 CVn|131676|384989|591^4972|HR4972|131981|-633028|633^4973|HR4973|131898|-433689|525^4974|HR4974|131639|622292|654^4975|HR4975|132048|-599208|460^4976|HR4976|132381|-784472|585^4977|HR4977|132136|-662269|590^4978|HR4978|131942|-265517|650^4979|HR4979|132009|-378031|485^4980|HR4980|132156|-598167|616^4981|53 Vir|132010|-161986|504^4982|HR4982|132141|-426997|622^4983|β Com|131979|278781|426^4984|HR4984|132023|242581|633^4985|HR4985|132232|-507000|589^4986|HR4986|132091|115561|577^4987|HR4987|132100|187517|653^4988|HR4988|132367|-586839|589^4989|HR4989|132374|-591033|492^4990|54 Vir|132241|-188267|628^4991|HR4991|132326|-431389|616^4992|HR4992|132201|187269|611^4993|η Mus|132541|-678944|480^4994|HR4994|132571|-696797|637^4995|55 Vir|132364|-199308|533^4996|HR4996|132453|-489567|589^4997|HR4997|132286|401528|492^4998|HR4998|132420|113317|567^4999|HR4999|132527|-363711|619^5000|HR5000|132791|-651383|607^5001|57 Vir|132663|-199431|522^5002|HR5002|132869|-667836|487^5003|HR5003|132256|727989|659^5004|19 CVn|132589|408553|579^5005|HR5005|132738|-13906|668^5006|HR5006|132814|-315061|510^5007|HR5007|132706|190517|645^5008|HR5008|132872|-439794|584^5009|HR5009|132071|804714|625^5010|HR5010|132756|197853|645^5011|59 Vir|132796|94242|522^5012|HR5012|133219|-72)BSC5";
static const char BSC5_CHUNK_52[] PROGMEM = R"BSC5(0356|604^5013|HR5013|132877|136756|533^5014|HR5014|132916|-6767|637^5015|σ Vir|132934|54697|480^5016|HR5016|133096|-512861|619^5017|20 CVn|132924|405725|473^5018|HR5018|132746|684081|620^5019|61 Vir|133068|-183114|474^5020|γ Hya|133154|-231717|300^5021|HR5021|133142|36878|662^5022|HR5022|133077|340981|582^5023|21 CVn|133040|496819|515^5024|HR5024|133430|-597733|618^5025|HR5025|133178|351281|602^5026|HR5026|133438|-527481|548^5027|HR5027|133468|-558006|602^5028|ι Cen|133433|-367122|275^5029|HR5029|133494|-468806|577^5030|HR5030|133813|-721467|605^5031|HR5031|133449|29417|626^5032|23 CVn|133386|401506|560^5033|HR5033|133583|-194889|621^5034|HR5034|133766|-609722|618^5035|HR5035|133772|-609883|453^5036|HR5036|133712|-521831|583^5037|HR5037|133616|20872|569^5038|HR5038|133813|-479431|616^5039|HR5039|133841|-485628|638^5040|64 Vir|133694|51547|587^5041|HR5041|134001|-645358|453^5042|ι¹ Mus|134186|-748878|505^5043|HR5043|133858|-331900|622^5044|63 Vir|133836|-177353|537^5045|HR5045|133677|439031|635^5046|HR5046|133979|-498231|648^5047|65 Vir|133886|-49244|589^5048|HR5048|134205|-644853|531^5049|HR5049|134306|-706275|567^5050|66 Vir|134092|-51639|575^5051|ι² Mus|134551|-746919|663^5052|HR5052|133983|370339|607^5053|HR5053|134085|124319|644^5054|Mizar|133988|549253|227^5055|Mizar|133990|549217|395^5056|Spica|134199|-111614|98^5057|HR5057|134185|238544|578^5058|HR5058|134355|-397553|509^5059|HR5059|134365|-11925|597^5060|HR5060|134489|-414981|569^5061|HR5061|134518|-491439|631^5062|Alcor|134204|549881|401^5063|HR5063|134558|-493808|628^5064|68 Vir|134453|-127078|525^5065|HR5065|134541|-401631|640^5066|HR5066|134796|-696281|620^5067|HR5067|134379|460281|588^5068|69 Vir|134576|-159736|476^5069|HR5069|134854|-646758|611^5070|HR5070|134333|632611|650^5071|HR5071|134903|-511653|506^5072|70 Vir|134738|137789|498^5073|HR5073|134356|723914|579^5074|HR5074|134513|647356|666^5075|HR5075|134530|647194|704^5076|HR5076|134665|527458|634^5077|HR5077|134739|407297|647^5078|HR5078|134875|-13644|643^5079|HR5079|134699|505872|680^5080|HR5080|134952|-232814|497^5081|71 Vir|134869|108183|565^5082|HR5082|135541|-775683|648^5083|HR5083|134794|507183|643^5084|κ Oct|136821|-857861|558^5085|HR5085|134742|599458|540^5086|HR5086|135000|71789|617^5087|HR5087|134993|60133|651^5088|72 Vir|135071|-64703|609^5089|HR5089|135174|-394075|388^5090|HR5090|135259|-281128|647^5091|HR5091|134491|786439|577^5092|HR5092|135348|-383992|616^5093|HR5093|135599|-656325|637^5094|73 Vir|135341|-187289|601^5095|74 Vir|135328|-62558|469^5096|HR5096|135211|421061|608^5097|HR5097|135433|-286928|569^5098|HR5098|135429|-295653|645^5099|75 Vir|135477|-153631|555^5100|76 Vir|135495|-101650|521^5101|HR5101|135502|-71950|668^5102|HR5102|135467|243464|611^5103|HR5103|135747|-482722|633^5104|HR5104|135788|-333108|644^)BSC5";
static const char BSC5_CHUNK_53[] PROGMEM = R"BSC5(5105|78 Vir|135689|36589|494^5106|HR5106|135779|-132144|591^5107|Heze|135782|-5958|337^5108|HR5108|135727|387892|637^5109|81 UMa|135687|553486|560^5110|HR5110|135799|371825|498^5111|80 Vir|135920|-53961|573^5112|24 CVn|135742|490161|470^5113|HR5113|136201|-616919|563^5114|HR5114|135926|102047|649^5115|HR5115|136533|-756839|634^5116|HR5116|135872|441969|684^5117|HR5117|136140|-344678|650^5118|HR5118|136183|-441433|598^5119|HR5119|136460|-704450|610^5120|HR5120|136134|-264950|578^5121|HR5121|136232|-464283|590^5122|HR5122|136354|-584150|642^5123|HR5123|136164|246133|574^5124|HR5124|136470|-576231|601^5125|HR5125|136668|-707883|659^5126|HR5126|136111|494867|649^5127|25 CVn|136243|362950|482^5128|HR5128|136450|-295608|583^5129|HR5129|136355|143017|652^5130|HR5130|136697|-645769|579^5131|HR5131|135786|765467|657^5132|ε Cen|136648|-534664|230^5133|HR5133|136286|507147|648^5134|HR5134|136666|-499503|600^5135|HR5135|136613|-397481|627^5136|HR5136|136635|-400519|560^5137|HR5137|136506|182653|648^5138|HR5138|136596|107461|557^5139|HR5139|136197|712422|550^5140|HR5140|137003|-587872|538^5141|HR5141|136958|-545600|501^5142|82 UMa|136584|529214|546^5143|HR5143|136710|310119|621^5144|1 Boo|136779|199556|575^5145|HR5145|136775|280653|623^5146|HR5146|136919|-234497|659^5147|HR5147|136960|-335969|605^5148|HR5148|136731|505194|632^5149|2 Boo|136840|224958|562^5150|82 Vir|136936|-87031|501^5151|HR5151|137156|-567681|600^5152|HR5152|137152|-507906|641^5153|HR5153|136726|572075|629^5154|83 UMa|136790|546817|466^5155|HR5155|137153|-414011|598^5156|HR5156|137035|83883|616^5157|HR5157|137278|-420675|598^5158|HR5158|137377|-510131|647^5159|84 Vir|137177|35381|536^5160|HR5160|137080|416742|630^5161|HR5161|137121|349889|598^5162|HR5162|136916|648225|585^5163|HR5163|137318|-54989|651^5164|HR5164|137292|227003|613^5165|83 Vir|137416|-161792|560^5166|HR5166|137460|-255008|621^5167|HR5167|137602|-261161|581^5168|1 Cen|137614|-330439|423^5169|HR5169|137319|520644|602^5170|85 Vir|137598|-157675|619^5171|HR5171|137863|-625900|651^5172|HR5172|137776|-514328|465^5173|86 Vir|137656|-124267|551^5174|HR5174|137823|-362519|515^5175|HR5175|137910|-502494|591^5176|HR5176|137940|-503211|545^5177|HR5177|137537|558794|650^5178|HR5178|137871|-97092|605^5179|HR5179|137704|410886|587^5180|HR5180|137719|385039|594^5181|87 Vir|137904|-178600|543^5182|3 Boo|137787|257022|595^5183|HR5183|137825|63506|633^5184|HR5184|137109|780644|591^5185|τ Boo|137877|174567|450^5186|HR5186|137833|385428|550^5187|84 UMa|137766|544328|570^5188|HR5188|139274|-826661|595^5189|HR5189|138153|-357039|653^5190|ν Cen|138251|-416878|341^5191|Alkaid|137923|493133|186^5192|2 Cen|138241|-344508|419^5193|μ Cen|138269|-424739|304^5194|HR5194|138632|-694014|575^5195|HR5195|138108|311903|562^5196|89 Vir|138312|-181342|497^5197|HR5197)BSC5";
static const char BSC5_CHUNK_54[] PROGMEM = R"BSC5(|138351|-290814|618^5198|HR5198|138387|-399011|644^5199|HR5199|138159|395428|740^5200|υ Boo|138246|157978|407^5201|6 Boo|138286|212642|491^5202|HR5202|138429|-198972|653^5203|HR5203|137064|827525|598^5204|HR5204|138292|366328|638^5205|HR5205|138402|54972|601^5206|HR5206|138631|-468992|577^5207|HR5207|138680|-528117|525^5208|HR5208|138602|-364333|635^5209|HR5209|138557|-243908|645^5210|3 Cen|138638|-329944|456^5211|3 Cen|138639|-329947|606^5212|HR5212|138669|-316194|612^5213|HR5213|138293|614892|596^5214|HR5214|138512|347725|665^5215|HR5215|138526|346644|587^5216|HR5216|138410|585394|646^5217|HR5217|138953|-533736|589^5218|HR5218|139136|-676525|571^5219|HR5219|138632|344442|474^5220|HR5220|138718|121653|604^5221|4 Cen|138868|-319278|473^5222|HR5222|138924|-356642|554^5223|HR5223|138991|-471281|610^5224|HR5224|138978|-353144|619^5225|7 Boo|138869|179328|570^5226|10 Dra|138572|647233|465^5227|HR5227|138498|683153|640^5228|HR5228|139046|-285697|604^5229|HR5229|138862|286481|590^5230|HR5230|139201|-521611|571^5231|ζ Cen|139257|-472883|255^5232|90 Vir|139117|-15031|515^5233|HR5233|139162|-80589|619^5234|HR5234|139388|-541319|614^5235|Muphrid|139114|183978|268^5236|HR5236|139425|-547044|600^5237|HR5237|139290|-312850|651^5238|86 UMa|138975|537286|570^5239|HR5239|139388|-465925|583^5240|HR5240|140091|-785900|609^5241|HR5241|139608|-636867|471^5242|HR5242|139753|-658006|620^5243|HR5243|139306|140564|616^5244|92 Vir|139411|10506|591^5245|HR5245|139362|320325|632^5246|HR5246|139577|-230228|614^5247|9 Boo|139428|274919|501^5248|φ Cen|139712|-421008|383^5249|υ¹ Cen|139780|-448036|387^5250|47 Hya|139753|-249722|515^5251|HR5251|139881|-503700|591^5252|HR5252|140048|-614814|649^5253|HR5253|140145|-662686|597^5254|HR5254|139778|146494|600^5255|10 Boo|139775|216961|576^5256|HR5256|139589|614928|637^5257|48 Hya|140000|-250103|577^5258|HR5258|139970|-35497|640^5259|HR5259|140219|-402222|613^5260|υ² Cen|140287|-456036|434^5261|θ Aps|140888|-767967|550^5262|HR5262|140223|88947|599^5263|11 Boo|140196|273867|623^5264|τ Vir|140274|15444|426^5265|HR5265|140397|-274300|548^5266|HR5266|140573|-562136|592^5267|Hadar|140637|-603731|61^5268|HR5268|140505|-316839|618^5269|HR5269|140576|-414233|611^5270|HR5270|140422|96864|620^5271|HR5271|140367|457536|627^5272|HR5272|140648|-224217|630^5273|HR5273|140590|107867|630^5274|HR5274|140602|75464|626^5275|HR5275|140655|49008|624^5276|HR5276|140707|-53814|639^5277|HR5277|140742|-149717|628^5278|HR5278|140962|-546694|617^5279|HR5279|141409|-748503|602^5280|HR5280|140499|509719|615^5281|HR5281|141070|-597156|642^5282|HR5282|140307|686786|634^5283|HR5283|140771|22975|628^5284|HR5284|140872|-163358|656^5285|χ Cen|141008|-411797|436^5286|HR5286|141030|-430919|620^5287|π Hya|141062|-266825|327^5288|Menkent|141114|-363700|206^5289|HR5289|141373|-)BSC5";
static const char BSC5_CHUNK_55[] PROGMEM = R"BSC5(632081|640^5290|95 Vir|141119|-93133|546^5291|Thuban|140731|643758|365^5292|HR5292|141490|-592767|634^5293|HR5293|141752|-703056|605^5294|HR5294|141478|-434711|617^5295|HR5295|141839|-697197|606^5296|HR5296|141597|-515047|600^5297|HR5297|141652|-534392|475^5298|96 Vir|141502|-103344|647^5299|HR5299|141322|438544|527^5300|13 Boo|141381|494581|525^5301|HR5301|141807|-163019|491^5302|HR5302|141461|593378|646^5303|η Aps|143038|-810078|491^5304|12 Boo|141733|250917|483^5305|3 UMi|141157|745936|645^5306|HR5306|142819|-776639|647^5307|HR5307|141920|13622|643^5308|HR5308|142212|-536658|556^5309|HR5309|142068|-243642|634^5310|HR5310|141875|322956|611^5311|HR5311|142278|-546258|611^5312|50 Hya|142128|-272611|508^5313|HR5313|142044|24094|501^5314|HR5314|142203|-266122|624^5315|κ Vir|142149|-102736|419^5316|HR5316|142492|-570858|507^5317|HR5317|142280|-8456|591^5318|HR5318|142452|-418375|561^5319|HR5319|142559|-535097|639^5320|HR5320|142774|-665878|575^5321|4 UMi|141475|775475|482^5322|HR5322|142392|-59475|636^5323|14 Boo|142348|129594|554^5324|HR5324|142504|-292819|608^5325|HR5325|142608|-450008|631^5326|HR5326|142762|-599139|639^5327|HR5327|144065|-828486|642^5328|κ¹ Boo|142244|517878|669^5329|κ² Boo|142247|517903|454^5330|15 Boo|142474|101006|529^5331|HR5331|142481|33361|645^5332|HR5332|142567|-182008|543^5333|HR5333|142447|218733|639^5334|HR5334|142011|694325|524^5335|HR5335|142399|415189|624^5336|ε Aps|143730|-801089|506^5337|HR5337|142718|-332414|655^5338|Syrma|142669|-60006|408^5339|δ Oct|144486|-836678|432^5340|Arcturus|142610|191825|-4^5341|HR5341|142726|-66219|644^5342|HR5342|142750|-31964|615^5343|HR5343|142678|189119|598^5344|HR5344|142844|-185853|622^5345|HR5345|142547|525358|658^5346|HR5346|142758|201214|625^5347|HR5347|142734|397447|638^5348|HR5348|143066|-332206|654^5349|HR5349|143310|-612731|523^5350|Asellus Secundus|142694|513672|475^5351|λ Boo|142731|460883|418^5352|HR5352|142912|152633|580^5353|HR5353|143002|-75425|647^5354|ι Lup|143234|-460578|355^5355|HR5355|143106|-187161|590^5356|HR5356|143169|-258156|587^5357|HR5357|143233|-370036|594^5358|HR5358|143388|-563867|433^5359|λ Vir|143185|-133711|452^5360|HR5360|142892|513072|620^5361|HR5361|142999|355094|481^5362|HR5362|143360|-430589|556^5363|HR5363|142970|480017|632^5364|HR5364|143451|-451872|477^5365|18 Boo|143212|130042|541^5366|υ Vir|143257|-22656|514^5367|ψ Cen|143426|-378853|405^5368|HR5368|143280|3842|619^5369|HR5369|143155|387675|686^5370|20 Boo|143292|163069|486^5371|HR5371|143769|-584594|492^5372|HR5372|143155|548642|653^5373|HR5373|143299|387939|633^5374|HR5374|143358|304292|644^5375|HR5375|143774|-483203|609^5376|HR5376|143721|-347869|556^5377|HR5377|143890|-507722|602^5378|HR5378|143839|-395122|442^5379|HR5379|144184|-681953|561^5380|HR5380|143968|-531767|600^5381|51 Hya|143849|)BSC5";
static const char BSC5_CHUNK_56[] PROGMEM = R"BSC5(-277539|477^5382|HR5382|144276|-661733|636^5383|2 Lib|143904|-117142|621^5384|HR5384|143876|12417|627^5385|HR5385|143896|84450|686^5386|HR5386|143896|84467|512^5387|HR5387|143852|253381|622^5388|HR5388|144002|82436|595^5389|HR5389|144936|-767292|607^5390|HR5390|144135|-248064|532^5391|HR5391|144520|-658217|585^5392|HR5392|144031|58200|510^5393|HR5393|144114|-116697|649^5394|HR5394|144051|80850|619^5395|τ¹ Lup|144356|-452214|456^5396|τ² Lup|144363|-453794|435^5397|HR5397|144249|-199697|661^5398|HR5398|144371|-423192|632^5399|HR5399|144299|-268525|648^5400|HR5400|144472|-398739|635^5401|HR5401|144534|-461344|583^5402|HR5402|144248|383931|627^5403|HR5403|144788|-591978|645^5404|Asellus Primus|144199|518508|405^5405|22 Boo|144409|192269|539^5406|104 Vir|144568|-61203|617^5407|52 Hya|144696|-294917|497^5408|HR5408|145212|-677172|583^5409|φ Vir|144700|-22281|481^5410|106 Vir|144782|-69006|542^5411|HR5411|144576|410250|663^5412|HR5412|145024|-453217|550^5413|HR5413|145058|-495192|537^5414|HR5414|144754|282892|762^5415|HR5415|144759|282908|712^5416|HR5416|144712|361969|610^5417|HR5417|145157|-408450|639^5418|HR5418|144974|8289|594^5419|HR5419|145197|-388697|597^5420|24 Boo|144772|498447|559^5421|HR5421|145425|-568878|693^5422|HR5422|144971|317911|606^5423|HR5423|144936|417958|635^5424|HR5424|145126|47722|602^5425|σ Lup|145436|-504569|442^5426|HR5426|145590|-549983|587^5427|HR5427|145583|-526800|587^5428|HR5428|145527|-307142|609^5429|ρ Boo|145305|303714|358^5430|5 UMi|144587|756961|425^5431|HR5431|145689|-420997|660^5432|HR5432|145881|-600158|640^5433|HR5433|145389|266772|601^5434|26 Boo|145424|222600|592^5435|Seginus|145346|383083|303^5436|HR5436|145128|631856|609^5437|HR5437|145286|602256|627^5438|HR5438|145808|-204392|650^5439|HR5439|145921|-415172|587^5440|η Cen|145918|-421578|231^5441|HR5441|145556|369594|643^5442|HR5442|145419|553978|576^5443|HR5443|146295|-679322|604^5444|HR5444|146053|-462453|555^5445|HR5445|145699|325344|633^5446|HR5446|146067|-395972|613^5447|σ Boo|145780|297450|446^5448|HR5448|145774|366258|603^5449|HR5449|146123|-402117|574^5450|HR5450|146222|-461339|541^5451|HR5451|145711|570653|648^5452|HR5452|145777|493683|574^5453|ρ Lup|146314|-494258|405^5454|HR5454|146019|232503|638^5455|HR5455|146166|-123053|620^5456|HR5456|146388|-387942|602^5457|HR5457|146531|-465842|607^5458|HR5458|146569|-490556|639^5459|Rigil Kentaurus|146600|-608353|-1^5460|α² Cen|146600|-608356|133^5461|HR5461|146758|-564408|630^5462|HR5462|146372|182983|591^5463|α Cir|147084|-649753|319^5464|HR5464|146368|436419|570^5465|HR5465|146988|-586161|622^5466|HR5466|146837|-361350|567^5467|HR5467|146376|540233|585^5468|33 Boo|146473|444044|539^5469|α Lup|146988|-473883|230^5470|α Aps|147977|-790447|383^5471|HR5471|146993|-377936|400^5472|HR5472|146728|219756|610^5473|HR547)BSC5";
static const char BSC5_CHUNK_57[] PROGMEM = R"BSC5(3|146784|135342|591^5474|HR5474|146975|-309333|637^5475|π¹ Boo|146788|164183|494^5476|π² Boo|146789|164178|588^5477|ζ Boo|146858|137283|483^5478|ζ Boo|146858|137283|443^5479|HR5479|145606|796603|626^5480|31 Boo|146941|81617|486^5481|32 Boo|146954|116606|556^5482|HR5482|147548|-628758|536^5483|HR5483|146984|211236|638^5484|4 Lib|147204|-249975|573^5485|HR5485|147276|-351736|405^5486|HR5486|147488|-584778|611^5487|Rijl al Awwa|147177|-56583|388^5488|HR5488|147530|-556019|610^5489|HR5489|147498|-351919|492^5490|34 Boo|147237|265278|481^5491|HR5491|154720|-881331|648^5492|HR5492|147009|612619|625^5493|HR5493|147290|404592|573^5494|HR5494|147748|-474411|574^5495|HR5495|147837|-523836|521^5496|HR5496|147532|-14178|607^5497|54 Hya|147667|-254431|494^5498|HR5498|147868|-522056|607^5499|HR5499|147686|-231531|581^5500|HR5500|148123|-665939|591^5501|108 Vir|147584|7172|569^5502|ο Boo|147540|169644|460^5503|5 Lib|147661|-154597|633^5504|HR5504|147697|-211761|640^5505|Izar|147498|270750|512^5506|Izar|147498|270742|270^5507|HR5507|147558|188847|613^5508|HR5508|147848|-382908|594^5509|HR5509|147922|-435575|630^5510|HR5510|147538|327883|628^5511|109 Vir|147708|18928|372^5512|HR5512|147683|151319|563^5513|HR5513|147871|-213247|606^5514|55 Hya|147896|-256244|563^5515|HR5515|148186|-566678|623^5516|56 Hya|147958|-260875|524^5517|57 Hya|147993|-266464|577^5518|HR5518|147986|-128397|635^5519|HR5519|148106|-366347|604^5520|HR5520|148871|-731900|560^5521|HR5521|148219|-242517|568^5522|HR5522|148150|-8478|614^5523|μ Lib|148220|-141489|531^5524|HR5524|148065|243667|614^5525|π¹ Oct|150308|-832278|565^5526|58 Hya|148381|-279603|441^5527|HR5527|148764|-638100|587^5528|ο Lup|148607|-435756|432^5529|HR5529|148185|378111|616^5530|α¹ Lib|148448|-159972|515^5531|Zubenelgenubi|148480|-160417|275^5532|HR5532|148329|286158|580^5533|Merga|148219|461161|574^5534|HR5534|148377|239119|585^5535|11 Lib|148503|-22992|494^5536|HR5536|148500|-2572|618^5537|HR5537|148256|513747|651^5538|39 Boo|148281|487206|569^5539|ζ Cir|149118|-659914|609^5540|HR5540|149647|-766628|534^5541|HR5541|148416|372719|548^5542|HR5542|148759|-305769|629^5543|HR5543|148809|-378033|503^5544|ξ Boo|148565|191011|455^5545|π² Oct|150796|-830383|565^5546|HR5546|149263|-601142|520^5547|HR5547|150033|-771603|593^5548|12 Lib|149056|-246422|530^5549|HR5549|149106|-333006|582^5550|HR5550|148898|157044|640^5551|θ Cir|149456|-627808|511^5552|HR5552|148573|592939|546^5553|HR5553|148899|191528|601^5554|ξ¹ Lib|149064|-118983|580^5555|HR5555|149989|-750325|620^5556|HR5556|149381|-528097|538^5557|ω Oct|151856|-847875|591^5558|HR5558|149291|-338558|532^5559|HR5559|149422|-478792|564^5560|HR5560|149504|-514469|664^5561|HR5561|149433|-394161|636^5562|HR5562|149419|-326367|606^5563|Kochab|148451|741556|208^5564|ξ² Lib|149461|-114097|546^556)BSC5";
static const char BSC5_CHUNK_58[] PROGMEM = R"BSC5(5|HR5565|149538|-291578|629^5566|HR5566|149691|-488631|635^5567|HR5567|149370|144464|577^5568|HR5568|149578|-214156|574^5569|HR5569|149330|323003|612^5570|16 Lib|149531|-43464|449^5571|β Lup|149755|-431339|268^5572|HR5572|149769|-399067|615^5573|HR5573|149592|-1675|553^5574|HR5574|149510|215553|649^5575|HR5575|149532|163883|571^5576|κ Cen|149860|-421042|313^5577|59 Hya|149776|-276572|565^5578|17 Lib|149704|-111550|660^5579|HR5579|149872|-378814|647^5580|HR5580|149909|-431600|610^5581|HR5581|149397|496286|563^5582|18 Lib|149816|-111442|587^5583|HR5583|149813|-49892|609^5584|HR5584|149898|45678|593^5585|HR5585|150203|-380583|589^5586|Zuben-el-Akribi|150162|-85189|492^5587|HR5587|150328|-343589|622^5588|40 Boo|149936|392653|564^5589|HR5589|149597|659325|460^5590|HR5590|150222|-27550|552^5591|60 Hya|150351|-280606|585^5592|HR5592|150146|220456|638^5593|η Cir|150801|-640317|517^5594|HR5594|150302|-1406|571^5595|HR5595|150498|-326433|544^5596|HR5596|148390|825119|564^5597|HR5597|150108|472778|637^5598|HR5598|151191|-719053|652^5599|HR5599|150458|-30314|661^5600|ω Boo|150351|250081|481^5601|110 Vir|150483|20914|440^5602|Nekkar|150324|403906|350^5603|Brachium|150678|-252819|329^5604|HR5604|150786|-408614|641^5605|π Lup|150853|-470511|472^5606|π Lup|150853|-470511|482^5607|HR5607|150886|-410672|515^5608|HR5608|150242|602044|593^5609|HR5609|150517|352058|551^5610|HR5610|150684|54925|650^5611|HR5611|151324|-652756|617^5612|HR5612|150518|446444|665^5613|HR5613|150601|345661|659^5614|HR5614|150966|-257897|667^5615|HR5615|151039|-362642|627^5616|ψ Boo|150741|269475|454^5617|HR5617|151239|-490883|577^5618|44 Boo|150632|476544|476^5619|HR5619|151092|-309189|596^5620|HR5620|151075|-220319|617^5621|HR5621|151583|-670842|576^5622|ν Lib|151104|-162569|520^5623|HR5623|151571|-636428|628^5624|HR5624|151367|-405839|579^5625|HR5625|151442|-428678|585^5626|λ Lup|151474|-452797|405^5627|47 Boo|150905|481511|557^5628|HR5628|152094|-727703|601^5629|HR5629|150661|659197|613^5630|HR5630|151098|364556|635^5631|HR5631|151279|54981|616^5632|HR5632|151791|-614225|630^5633|HR5633|151223|184417|602^5634|45 Boo|151217|248692|493^5635|HR5635|151046|545564|525^5636|HR5636|151688|-387925|598^5637|HR5637|151878|-553461|554^5638|46 Boo|151399|263011|567^5639|HR5639|151482|132350|610^5640|HR5640|151432|251086|581^5641|HR5641|151718|-263328|576^5642|HR5642|151930|-452775|644^5643|HR5643|151922|-452794|739^5644|HR5644|152386|-700794|581^5645|HR5645|152169|-617439|632^5646|κ¹ Lup|151989|-487378|387^5647|κ² Lup|151993|-487436|569^5648|HR5648|151388|500550|639^5649|ζ Lup|152048|-520992|341^5650|HR5650|152087|-482186|633^5651|HR5651|152138|-445006|482^5652|ι¹ Lib|152037|-197917|454^5653|HR5653|152187|-360914|610^5654|HR5654|152012|189758|589^5655|HR5655|152215|-240083|647^5656|ι² Lib|152220|-196475)BSC5";
static const char BSC5_CHUNK_59[] PROGMEM = R"BSC5(|608^5657|23 Lib|152246|-253092|645^5658|HR5658|152315|-261936|584^5659|HR5659|152121|192858|668^5660|1 Lup|152437|-315192|491^5661|HR5661|152768|-609039|573^5662|26 Lib|152427|-177686|617^5663|HR5663|152649|-480742|595^5664|δ Cir|152824|-609575|509^5665|HR5665|152255|229833|630^5666|ε Cir|152941|-636106|486^5667|HR5667|152678|-414911|516^5668|HR5668|152696|-434847|604^5669|HR5669|152474|-55028|628^5670|β Cir|152919|-588011|407^5671|Gatria|153152|-686794|289^5672|HR5672|151789|677814|617^5673|HR5673|152266|382647|620^5674|HR5674|152350|317881|599^5675|3 Ser|152532|49394|533^5676|χ Boo|152414|291642|526^5677|HR5677|152362|421714|613^5678|HR5678|152731|-223994|550^5679|4 Ser|152636|3722|563^5680|HR5680|153136|-604964|546^5681|δ Boo|152584|333147|347^5682|HR5682|153026|-410608|628^5683|μ Lup|153089|-478750|427^5684|HR5684|153446|-674814|628^5685|Zubeneschamali|152834|-93831|261^5686|2 Lup|152972|-301489|434^5687|HR5687|153157|-407883|559^5688|HR5688|153115|-312094|618^5689|HR5689|153254|-370967|620^5690|HR5690|153073|-4614|589^5691|HR5691|152440|673467|513^5692|HR5692|153068|205728|570^5693|HR5693|152478|689453|651^5694|5 Ser|153219|17653|506^5695|δ Lup|153562|-406475|322^5696|HR5696|153598|-407497|620^5697|HR5697|153584|-382192|648^5698|ν¹ Lup|153690|-479278|500^5699|ν² Lup|153634|-483178|565^5700|HR5700|153862|-606569|567^5701|28 Lib|153482|-181586|617^5702|HR5702|153251|325153|632^5703|ο Lib|153504|-155483|630^5704|γ Cir|153896|-593208|451^5705|φ¹ Lup|153634|-362614|356^5706|HR5706|153464|-24133|635^5707|HR5707|153521|-58247|554^5708|ε Lup|153780|-446894|337^5709|ο CrB|153357|296161|551^5710|6 Ser|153506|7153|535^5711|HR5711|153519|249578|639^5712|φ² Lup|153859|-368586|454^5713|HR5713|154374|-683092|589^5714|11 UMi|152850|718239|502^5715|HR5715|153348|519586|566^5716|HR5716|153449|444342|619^5717|7 Ser|153731|125675|628^5718|50 Boo|153635|329339|537^5719|υ Lup|154125|-397103|537^5720|HR5720|153978|-123694|572^5721|8 Ser|153955|-10225|612^5722|HR5722|154185|-381694|703^5723|ε Lib|154033|-103222|494^5724|HR5724|154223|-387336|460^5725|HR5725|154592|-645314|571^5726|HR5726|153771|395814|550^5727|η CrB|153868|302878|558^5728|η CrB|153868|302878|608^5729|ρ Oct|157213|-844653|557^5730|κ¹ Aps|155252|-733894|549^5731|HR5731|153770|620472|598^5732|HR5732|154014|452711|601^5733|Alkalurops|154082|373772|431^5734|μ² Boo|154086|373478|650^5735|Pherkad|153455|718339|305^5736|HR5736|154551|-367678|545^5737|HR5737|153773|633414|579^5738|HR5738|154742|-515975|610^5739|τ¹ Ser|154298|154281|517^5740|HR5740|154315|194808|627^5741|HR5741|154382|343358|546^5742|HR5742|154901|-467328|524^5743|ζ¹ Lib|154709|-167164|564^5744|Edasich|154155|589661|329^5745|HR5745|154608|251017|602^5746|10 Ser|154773|18422|517^5747|Nusakan|154638|291058|368^5748|HR5748|154424|540200|645^5749|H)BSC5";
static const char BSC5_CHUNK_60[] PROGMEM = R"BSC5(R5749|155101|-207283|622^5750|ζ³ Lib|155112|-166094|582^5751|HR5751|155346|-386231|625^5752|HR5752|154790|472014|615^5753|HR5753|155306|-328811|646^5754|HR5754|154613|622756|650^5755|HR5755|154643|606703|590^5756|HR5756|155287|-201647|622^5757|HR5757|156551|-779181|618^5758|HR5758|155154|85792|657^5759|HR5759|154824|551950|643^5760|HR5760|155063|312861|646^5761|HR5761|155078|368042|637^5762|HR5762|155435|-196706|552^5763|ν¹ Boo|155155|408331|502^5764|ζ⁴ Lib|155487|-168528|550^5765|HR5765|155525|-244894|700^5766|HR5766|156048|-656133|651^5767|HR5767|155671|-400661|582^5768|HR5768|154892|620997|638^5769|HR5769|155229|366164|638^5770|τ² Ser|155360|160561|622^5771|ε TrA|156120|-663169|411^5772|11 Ser|155494|-11864|551^5773|HR5773|155725|-393494|636^5774|ν² Boo|155297|408994|502^5775|36 Lib|155770|-280469|515^5776|γ Lup|155857|-411669|278^5777|37 Lib|155696|-100644|462^5778|θ CrB|155488|313592|414^5779|HR5779|155724|-56950|651^5780|HR5780|155741|-91833|517^5781|HR5781|155981|-449586|454^5782|κ² Aps|156726|-734467|565^5783|HR5783|155647|171378|645^5784|HR5784|156034|-443969|543^5785|HR5785|155155|642086|579^5786|HR5786|156985|-760819|595^5787|Zuben-el-Akrab|155921|-147894|391^5788|δ Ser|155800|105375|380^5789|δ Ser|155800|105392|380^5790|HR5790|156032|-330928|624^5791|HR5791|155846|16689|656^5792|HR5792|156699|-702278|644^5793|Alphecca|155781|267147|223^5794|υ Lib|156171|-281350|358^5795|τ³ Ser|155926|176556|612^5796|HR5796|155982|112656|607^5797|ω Lup|156342|-425675|433^5798|HR5798|156471|-523728|544^5799|14 Ser|156094|-5617|651^5800|μ CrB|155875|390100|511^5801|HR5801|156246|-262800|619^5802|16 Ser|156082|100100|526^5803|HR5803|156657|-599083|595^5804|τ⁵ Ser|156081|161189|593^5805|HR5805|156424|-391608|657^5806|HR5806|156300|-231417|578^5807|HR5807|156451|-391281|604^5808|HR5808|155970|383739|642^5809|HR5809|156377|-282067|632^5810|HR5810|156379|-210161|584^5811|HR5811|155879|539219|597^5812|τ Lib|156443|-297778|366^5813|HR5813|156148|299911|652^5814|41 Lib|156485|-193019|538^5815|HR5815|156444|-87944|650^5816|HR5816|156445|-87911|648^5817|HR5817|156011|520697|674^5818|HR5818|155992|546306|574^5819|HR5819|156559|-231503|634^5820|ψ¹ Lup|156628|-344119|467^5821|HR5821|156829|-477356|623^5822|HR5822|156709|-312136|634^5823|φ Boo|156304|403533|524^5824|42 Lib|156714|-238181|496^5825|HR5825|156865|-446611|464^5826|θ UMi|155236|773494|496^5827|HR5827|156469|346750|611^5828|HR5828|156256|545089|587^5829|HR5829|154864|804486|658^5830|HR5830|156378|467978|575^5831|HR5831|156696|120531|625^5832|HR5832|157103|-494894|604^5833|ζ¹ CrB|156562|366367|600^5834|ζ² CrB|156563|366358|507^5835|HR5835|156429|504233|584^5836|HR5836|157320|-602872|648^5837|HR5837|157106|-374250|524^5838|κ Lib|156991|-196789|474^5839|ψ² Lup|157114|-347106|475^5840|τ⁶ Ser|156831|160247|601^5841|)BSC5";
static const char BSC5_CHUNK_61[] PROGMEM = R"BSC5(HR5841|156526|579244|645^5842|ι Ser|156925|196703|452^5843|χ Ser|156965|128475|533^5844|HR5844|156275|692833|562^5845|τ⁷ Ser|156985|184639|581^5846|HR5846|157396|-418194|594^5847|HR5847|157236|-150433|631^5848|η Lib|157346|-156728|541^5849|γ CrB|157124|262956|384^5850|HR5850|157196|136678|648^5851|HR5851|157982|-654425|618^5852|HR5852|157982|-654425|639^5853|ψ Ser|157338|25150|588^5854|Unukalhai|157378|64256|265^5855|π CrB|157331|325158|556^5856|HR5856|157702|-280617|651^5857|HR5857|157141|523608|551^5858|τ⁸ Ser|157450|172642|614^5859|HR5859|157565|54469|558^5860|HR5860|157789|-346825|561^5861|HR5861|157610|8914|633^5862|HR5862|157904|-401942|642^5863|25 Ser|157682|-18044|540^5864|HR5864|157914|-379164|601^5865|HR5865|158140|-524381|607^5866|HR5866|157793|-61203|624^5867|Chow|157698|154219|367^5868|λ Ser|157741|73531|443^5869|HR5869|158352|-532094|577^5870|υ Ser|157881|141153|571^5871|HR5871|158326|-489119|584^5872|HR5872|158379|-454017|612^5873|HR5873|158519|-550558|573^5874|HR5874|158037|137886|600^5875|HR5875|158158|-38186|553^5876|HR5876|158824|-651525|654^5877|HR5877|158006|317358|644^5878|HR5878|157763|554747|592^5879|κ Ser|158123|181417|409^5880|HR5880|158096|281567|585^5881|μ Ser|158270|-34303|353^5882|HR5882|158588|-470606|601^5883|χ Lup|158493|-336272|395^5884|HR5884|158897|-626069|619^5885|1 Sco|158496|-257514|464^5886|HR5886|157778|625994|519^5887|HR5887|157939|553767|586^5888|ω Ser|158382|21964|523^5889|δ CrB|158266|260683|463^5890|HR5890|158810|-506153|660^5891|κ TrA|159249|-686031|509^5892|ε Ser|158469|44778|371^5893|HR5893|158702|-298867|640^5894|HR5894|158449|151336|520^5895|36 Ser|158543|-30906|511^5896|HR5896|158607|-141336|619^5897|Betria|159190|-634306|285^5898|HR5898|159146|-607433|615^5899|ρ Ser|158544|209778|476^5900|HR5900|159256|-601778|577^5901|κ CrB|158539|356575|482^5902|λ Lib|158889|-201672|503^5903|ζ UMi|157343|777944|432^5904|2 Sco|158935|-253272|459^5905|HR5905|159350|-604828|576^5906|HR5906|158983|-245331|539^5907|HR5907|158988|-239781|542^5908|θ Lib|158971|-167294|415^5909|HR5909|158823|174036|636^5910|HR5910|159083|-273386|614^5911|39 Ser|158867|131967|610^5912|3 Sco|159110|-252436|587^5913|HR5913|158930|160750|609^5914|χ Her|158779|424517|462^5915|47 Lib|159168|-193831|594^5916|HR5916|159251|-310836|621^5917|4 Sco|159250|-262658|562^5918|HR5918|159351|-398642|603^5919|40 Ser|159112|85803|629^5920|HR5920|159828|-650378|575^5921|HR5921|159511|-481622|631^5922|HR5922|158713|558267|581^5923|HR5923|159372|-317858|629^5924|HR5924|159096|203108|544^5925|ξ¹ Lup|159482|-339664|512^5926|ξ² Lup|159484|-339642|562^5927|HR5927|159373|-143994|637^5928|ρ Sco|159481|-292142|388^5929|HR5929|159559|-361850|580^5930|HR5930|159426|-148292|613^5931|HR5931|159277|186206|626^5932|2 Her|159105|431386|537^5933|γ Ser|159409|156617|385^5934)BSC5";
static const char BSC5_CHUNK_62[] PROGMEM = R"BSC5(|HR5934|159612|-209831|585^5935|HR5935|159752|-375031|631^5936|λ CrB|159299|379469|545^5937|HR5937|159983|-540211|610^5938|4 Her|159252|425661|575^5939|HR5939|160196|-637767|641^5940|φ Ser|159541|144144|554^5941|48 Lib|159698|-142794|488^5942|HR5942|159763|-248314|543^5943|HR5943|159918|-417444|499^5944|π Sco|159809|-261142|289^5945|HR5945|159994|-406528|649^5946|HR5946|160184|-545778|613^5947|ε CrB|159598|268778|415^5948|η Lup|160020|-383969|341^5949|HR5949|159305|589117|631^5950|HR5950|159583|396953|631^5951|HR5951|160479|-625417|625^5952|HR5952|160149|-404353|621^5953|Dschubba|160056|-226217|232^5954|49 Lib|160054|-165333|547^5955|HR5955|160988|-724008|570^5956|HR5956|160221|-318894|633^5957|HR5957|159827|366439|562^5958|HR5958|159917|259203|200^5959|50 Lib|160132|-84114|555^5960|HR5960|159632|547497|495^5961|ι¹ Nor|160589|-577753|463^5962|η Nor|160536|-492297|465^5963|HR5963|160142|44275|583^5964|HR5964|159846|498811|605^5965|HR5965|160443|-291358|603^5966|5 Her|160206|178183|512^5967|HR5967|160567|-386025|489^5968|ρ CrB|160174|333036|541^5969|HR5969|160557|-258653|500^5970|HR5970|160595|-320006|601^5971|ι CrB|160241|298511|499^5972|π Ser|160382|228044|483^5973|HR5973|160652|-247264|621^5974|HR5974|160716|-332144|610^5975|HR5975|160769|-378631|590^5976|43 Ser|160627|49867|608^5977|Graffias|160728|-113731|507^5978|Graffias|160728|-113731|477^5979|HR5979|161233|-561914|616^5980|δ Nor|161082|-451733|472^5981|HR5981|160349|529158|593^5982|υ Her|160466|460367|476^5983|HR5983|160554|366317|583^5984|Acrab|160906|-198056|262^5985|β² Sco|160907|-198019|492^5986|θ Dra|160315|585653|401^5987|θ Lup|161099|-368022|423^5988|HR5988|161018|-236064|592^5989|HR5989|160957|-62917|653^5990|HR5990|160999|-61394|641^5991|HR5991|161212|-367556|573^5992|HR5992|160938|80961|629^5993|ω¹ Sco|161134|-206692|396^5994|ι² Nor|161552|-579344|557^5995|HR5995|160526|594108|619^5996|HR5996|161176|-140708|632^5997|ω² Sco|161234|-208686|432^5998|HR5998|161311|-244619|633^5999|HR5999|161428|-391053|705^6000|HR6000|161429|-390931|665^6001|HR6001|161354|-263267|538^6002|11 Sco|161268|-127456|578^6003|HR6003|161455|-236856|588^6004|45 Ser|161271|98917|563^6005|HR6005|161228|218225|614^6006|HR6006|161588|-326497|619^6007|HR6007|161646|-335458|554^6008|Marfik|161346|170469|500^6009|κ Her|161347|170544|625^6010|47 Ser|161411|85342|573^6011|HR6011|161497|34544|591^6012|HR6012|161653|-183408|647^6013|8 Her|161463|172058|614^6014|HR6014|161531|63789|597^6015|HR6015|161882|-411197|586^6016|HR6016|161640|-34669|537^6017|HR6017|161839|-294164|513^6018|τ CrB|161495|364908|476^6019|ζ Nor|162229|-555408|581^6020|δ¹ Aps|163391|-786958|468^6021|δ² Aps|163408|-786672|527^6022|HR6022|162213|-536717|583^6023|φ Her|161462|449350|426^6024|κ Nor|162246|-546306|494^6025|HR6025|161055|678103|544^6026|Jabbah|)BSC5";
static const char BSC5_CHUNK_63[] PROGMEM = R"BSC5(161996|-194497|630^6027|Jabbah|161999|-194606|401^6028|13 Sco|162051|-279264|459^6029|12 Sco|162044|-284175|567^6030|δ TrA|162573|-636856|385^6031|ψ Sco|162000|-100642|494^6032|HR6032|161916|97125|653^6033|16 Sco|162020|-85475|543^6034|HR6034|160587|767936|556^6035|HR6035|161913|166656|608^6036|HR6036|161508|579378|633^6037|HR6037|162849|-679414|575^6038|HR6038|161572|558289|649^6039|10 Her|161939|234947|570^6040|HR6040|162638|-579122|563^6041|HR6041|162157|-42208|625^6042|HR6042|162294|-244219|641^6043|HR6043|161944|333425|629^6044|HR6044|162395|-330114|592^6045|θ Nor|162542|-473722|514^6046|HR6046|161967|364250|563^6047|9 Her|162209|50211|548^6048|χ Sco|162308|-118375|522^6049|HR6049|162567|-428997|614^6050|HR6050|161966|423744|587^6051|HR6051|162413|-211075|641^6052|HR6052|162126|266708|650^6053|HR6053|162442|-185353|632^6054|HR6054|162482|-254769|605^6055|HR6055|162786|-538111|544^6056|Yed Prior|162391|-36944|274^6057|HR6057|162371|59019|631^6058|γ¹ Nor|162836|-500683|499^6059|HR6059|162891|-530867|633^6060|18 Sco|162604|-83694|550^6061|HR6061|162643|-148492|609^6062|HR6062|163144|-578997|649^6063|σ CrB|162447|338586|564^6064|σ CrB|162447|338583|666^6065|16 Her|162580|188083|569^6066|HR6066|162830|-213039|661^6067|HR6067|162820|-39533|618^6068|HR6068|162632|274222|614^6069|HR6069|162070|671442|621^6070|HR6070|163050|-286139|478^6071|λ Nor|163216|-426739|545^6072|γ² Nor|163307|-501556|402^6073|HR6073|163403|-551400|577^6074|υ CrB|162791|291503|578^6075|Yed Posterior|163054|-46925|324^6076|HR6076|163188|-202178|629^6077|HR6077|163258|-309067|549^6078|HR6078|163168|-148725|594^6079|19 UMi|161804|758775|548^6080|HR6080|163424|-394308|612^6081|ο Sco|163439|-241694|455^6082|20 UMi|162089|752106|639^6083|HR6083|163744|-495722|533^6084|Al Niyat|163531|-255928|289^6085|HR6085|163747|-439122|588^6086|HR6086|162876|597550|540^6087|HR6087|163345|211325|605^6088|HR6088|162426|733950|598^6089|HR6089|164228|-631247|615^6090|HR6090|163198|490381|591^6091|HR6091|163320|397086|546^6092|τ Her|163290|463133|389^6093|σ Ser|163679|10292|482^6094|HR6094|164004|-391931|540^6095|γ Her|163653|191531|375^6096|HR6096|163775|-20797|623^6097|HR6097|163991|-331994|647^6098|ζ TrA|164745|-700844|491^6099|HR6099|164151|-453492|633^6100|HR6100|164088|-375658|542^6101|HR6101|163026|685544|641^6102|γ Aps|165575|-788972|389^6103|ξ CrB|163683|308919|485^6104|ψ Oph|164017|-200375|450^6105|HR6105|164110|-297033|663^6106|HR6106|164110|-297047|584^6107|ν¹ CrB|163726|337992|520^6108|ν² CrB|163748|337036|539^6109|ι TrA|164659|-640581|527^6110|HR6110|163824|323331|640^6111|21 Her|164030|69481|585^6112|ρ Oph|164264|-234472|502^6113|ρ Oph|164264|-234461|592^6114|HR6114|164709|-585997|569^6115|ε Nor|164531|-475550|447^6116|η UMi|162918|757553|495^6117|Kajam|164236|140333|457^6118|χ Oph|164504|-)BSC5";
static const char BSC5_CHUNK_64[] PROGMEM = R"BSC5(184564|442^6119|HR6119|164299|188925|670^6120|HR6120|164958|-577558|606^6121|HR6121|164365|114075|611^6122|HR6122|164708|-371794|579^6123|25 Her|164234|373939|554^6124|HR6124|164472|23475|607^6125|HR6125|165137|-616336|520^6126|HR6126|163635|691094|525^6127|HR6127|164070|552050|574^6128|HR6128|164621|-75981|523^6129|υ Oph|164634|-83717|463^6130|HR6130|163964|616967|567^6131|HR6131|164951|-462433|535^6132|η Dra|163999|615142|274^6133|HR6133|172665|-875664|657^6134|Antares|164901|-264319|96^6135|HR6135|165720|-709881|550^6136|HR6136|164761|6650|539^6137|HR6137|164802|-81289|648^6138|HR6138|167649|-832389|657^6139|HR6139|170162|-863644|604^6140|HR6140|164964|-145508|568^6141|22 Sco|165034|-251150|479^6142|HR6142|165282|-418169|533^6143|HR6143|165230|-347044|423^6144|HR6144|165083|-75150|650^6145|HR6145|165230|-265378|610^6146|30 Her|164774|418817|504^6147|φ Oph|165190|-166128|428^6148|Kornephoros|165037|214897|277^6149|Marfik|165152|19839|382^6150|HR6150|164787|514078|629^6151|θ TrA|165958|-654953|552^6152|HR6152|165093|204792|525^6153|ω Oph|165356|-214664|445^6154|HR6154|165204|221953|576^6155|μ Nor|165681|-440453|494^6156|34 Her|165017|489608|645^6157|HR6157|165174|352250|625^6158|28 Her|165432|55211|563^6159|29 Her|165434|114881|484^6160|HR6160|165855|-452447|646^6161|15 Dra|164664|687681|500^6162|HR6162|165298|455983|565^6163|β Aps|167179|-775175|424^6164|HR6164|166063|-428589|547^6165|Alniyat|165981|-282161|282^6166|HR6166|166063|-352556|416^6167|HR6167|166480|-609903|618^6168|σ Her|165684|424369|420^6169|HR6169|165906|170572|641^6170|HR6170|165405|608233|594^6171|12 Oph|166060|-23247|575^6172|η¹ TrA|166898|-682961|591^6173|HR6173|164286|789639|556^6174|HR6174|166406|-433986|583^6175|ζ Oph|166193|-105672|256^6176|HR6176|166119|154981|630^6177|HR6177|166807|-604461|618^6178|HR6178|166514|-372175|591^6179|HR6179|166338|-65381|609^6180|HR6180|165245|726119|630^6181|HR6181|166300|136869|631^6182|HR6182|167228|-674325|603^6183|HR6183|166031|466133|579^6184|16 Dra|166032|529003|553^6185|17 Dra|166038|529244|508^6186|17 Dra|166039|529242|653^6187|HR6187|166890|-487631|565^6188|HR6188|166945|-496517|565^6189|HR6189|166609|-95544|635^6190|HR6190|166762|-204086|626^6191|HR6191|165108|774467|634^6192|HR6192|166960|-331464|587^6193|HR6193|166934|-244681|609^6194|36 Her|166764|42072|693^6195|37 Her|166774|42197|577^6196|HR6196|166929|-177422|496^6197|HR6197|167176|-460706|623^6198|HR6198|166153|630728|616^6199|HR6199|166334|560156|529^6200|42 Her|166458|489283|490^6201|HR6201|166865|-10006|624^6202|HR6202|166983|-199244|557^6203|HR6203|166809|123950|608^6204|HR6204|167778|-671097|513^6205|14 Oph|166951|11811|574^6206|HR6206|167293|-411189|620^6207|HR6207|167444|-531525|596^6208|HR6208|166835|248586|606^6209|HR6209|167316|-411131|612^6210|HR6210|167299|-381567|6)BSC5";
static const char BSC5_CHUNK_65[] PROGMEM = R"BSC5(05^6211|HR6211|167274|-321061|646^6212|ζ Her|166881|316031|281^6213|39 Her|166935|269169|592^6214|HR6214|167452|-408397|571^6215|HR6215|167726|-585036|574^6216|HR6216|167381|-274561|658^6217|Atria|168111|-690278|192^6218|HR6218|167501|-285097|602^6219|HR6219|167888|-583414|558^6220|η Her|167149|389222|353^6221|HR6221|167799|-393772|548^6222|HR6222|167310|340389|599^6223|18 Dra|166820|645892|483^6224|16 Oph|167582|10203|603^6225|25 Sco|167809|-255286|671^6226|HR6226|167162|556903|616^6227|HR6227|167562|157453|556^6228|43 Her|167639|85825|515^6229|η Ara|168298|-590414|376^6230|HR6230|167532|432172|605^6231|HR6231|168715|-676819|632^6232|19 Oph|167861|20644|610^6233|HR6233|168650|-653756|613^6234|45 Her|167962|52467|524^6235|HR6235|168075|-149094|603^6236|HR6236|168433|-500456|647^6237|HR6237|167549|567819|485^6238|HR6238|166314|789183|632^6239|HR6239|168025|135906|635^6240|HR6240|168244|-156675|610^6241|ε Sco|168361|-342933|229^6242|HR6242|167888|422389|587^6243|20 Oph|168306|-107831|465^6244|HR6244|168499|-375144|611^6245|HR6245|168594|-412306|522^6246|HR6246|168263|132614|591^6247|μ¹ Sco|168645|-380475|308^6248|HR6248|168395|-26539|632^6249|HR6249|168720|-418544|649^6250|47 Her|168387|72478|549^6251|HR6251|169001|-579094|594^6252|μ² Sco|168722|-380175|357^6253|HR6253|169235|-632697|602^6254|52 Her|168206|459833|482^6255|21 Oph|168569|12161|551^6256|HR6256|168279|434306|613^6257|HR6257|168951|-430508|596^6258|50 Her|168442|298067|572^6259|HR6259|168453|325536|613^6260|HR6260|169005|-418064|545^6261|HR6261|168997|-419947|632^6262|ζ¹ Sco|168999|-423622|473^6263|HR6263|169033|-418503|645^6264|HR6264|168434|418967|629^6265|HR6265|169054|-418200|659^6266|HR6266|169075|-424789|588^6267|HR6267|167183|775142|598^6268|49 Her|168680|149742|652^6269|HR6269|168903|-204156|588^6270|51 Her|168626|246564|504^6271|ζ² Sco|169097|-423614|362^6272|HR6272|169162|-411511|577^6273|HR6273|169100|-305872|635^6274|HR6274|169358|-506750|633^6275|HR6275|169413|-522839|594^6276|HR6276|169927|-692683|579^6277|HR6277|169029|-16122|625^6278|HR6278|169112|-117925|657^6279|53 Her|168828|317017|532^6280|23 Oph|169099|-61539|525^6281|ι Oph|169001|101653|438^6282|HR6282|169327|-335072|637^6283|HR6283|169433|-408236|615^6284|HR6284|169338|-168061|637^6285|ζ Ara|169770|-559903|313^6286|HR6286|168882|474169|600^6287|HR6287|169153|209586|541^6288|27 Sco|169531|-332594|548^6289|HR6289|169717|-506411|555^6290|HR6290|169211|136197|634^6291|24 Oph|169467|-231500|558^6292|56 Her|169173|257306|608^6293|54 Her|169228|184333|535^6294|HR6294|169511|-195400|627^6295|ε¹ Ara|169931|-531606|406^6296|HR6296|169572|-109633|619^6297|HR6297|170017|-545969|565^6298|HR6298|169812|-376211|609^6299|κ Oph|169611|93750|320^6300|HR6300|170075|-486478|600^6301|HR6301|169589|138842|637^6302|HR6302|169782|-148697|659)BSC5";
static const char BSC5_CHUNK_66[] PROGMEM = R"BSC5(^6303|HR6303|170090|-454517|665^6304|HR6304|170298|-589583|611^6305|57 Her|169586|253528|628^6306|HR6306|169351|500389|656^6307|HR6307|169618|243814|632^6308|HR6308|169993|-250919|586^6310|26 Oph|170026|-249892|575^6311|HR6311|170102|-359342|597^6312|HR6312|170295|-511308|645^6313|HR6313|169639|425125|634^6314|ε² Ara|170524|-532369|529^6315|19 Dra|169338|651347|489^6316|HR6316|170313|-321436|503^6317|HR6317|170082|65836|659^6318|30 Oph|170177|-42225|482^6319|20 Dra|169403|650392|641^6320|HR6320|170735|-577122|573^6321|29 Oph|170309|-188856|626^6322|ε UMi|167661|820372|423^6323|HR6323|170616|-471600|606^6324|ε Her|170048|309264|392^6325|HR6325|170161|226322|565^6326|HR6326|170258|149494|631^6327|HR6327|170641|-381522|591^6328|HR6328|170193|271964|655^6329|HR6329|170331|84506|633^6330|HR6330|169893|566886|603^6331|HR6331|170848|-455017|628^6332|59 Her|170268|335683|525^6333|HR6333|170385|255056|575^6334|HR6334|170804|-341228|487^6335|HR6335|169380|731278|630^6336|HR6336|170381|318847|636^6337|HR6337|170522|140919|498^6338|HR6338|170968|-441050|619^6339|HR6339|170529|145111|652^6340|HR6340|170792|-204947|630^6341|HR6341|170609|136053|593^6342|HR6342|170661|135675|608^6343|HR6343|170646|196906|635^6344|HR6344|171056|-372275|598^6345|HR6345|169841|691864|640^6346|61 Her|170584|354142|669^6347|HR6347|171079|-354511|613^6348|HR6348|170213|606492|613^6349|HR6349|170880|7025|601^6350|HR6350|171033|-215647|630^6351|HR6351|170649|347903|604^6352|HR6352|170781|195992|617^6353|HR6353|170923|-8919|564^6354|HR6354|171148|-265131|629^6355|60 Her|170896|127408|491^6356|HR6356|171684|-616756|639^6357|HR6357|172055|-707211|622^6358|HR6358|171027|97333|637^6359|HR6359|171036|104542|637^6360|HR6360|170376|646006|610^6361|HR6361|171147|-16564|638^6362|HR6362|170847|438122|643^6363|HR6363|170805|488042|609^6364|HR6364|171050|220842|556^6365|HR6365|171375|-176092|599^6366|HR6366|171465|-304036|597^6367|HR6367|171371|-10794|606^6368|HR6368|172215|-671967|589^6369|Arrakis|170888|544703|583^6370|Arrakis|170888|544703|580^6371|HR6371|171784|-445575|508^6372|HR6372|171485|-38828|636^6373|HR6373|172766|-745331|625^6374|HR6374|171941|-488742|584^6375|HR6375|171633|-105233|556^6376|HR6376|171296|405161|634^6377|HR6377|171339|359353|539^6378|Sabik|171730|-157247|243^6379|HR6379|170278|752972|621^6380|η Sco|172026|-432392|333^6381|HR6381|172045|-395069|567^6382|HR6382|172046|-388222|630^6383|HR6383|171381|508422|646^6384|HR6384|172370|-568883|609^6385|HR6385|171794|124672|657^6386|HR6386|172038|-252547|654^6387|HR6387|172069|-277619|614^6388|HR6388|171592|407772|508^6389|HR6389|172163|-324386|601^6390|HR6390|171959|78947|633^6391|63 Her|171842|242378|619^6392|HR6392|172410|-397669|660^6393|37 Oph|172077|105853|533^6394|HR6394|172151|3519|665^6395|HR6395|171752|524089|629^6396|Aldhiba)BSC5";
static const char BSC5_CHUNK_67[] PROGMEM = R"BSC5(in|171464|657147|317^6397|HR6397|172554|-335483|553^6398|HR6398|172600|-385939|596^6399|HR6399|171945|497467|604^6400|HR6400|173369|-700456|653^6401|36 Oph|172558|-266028|511^6402|36 Oph|172558|-266014|507^6403|HR6403|172643|-302106|621^6404|HR6404|172556|-145839|599^6405|HR6405|172726|-357494|612^6406|Rasalgethi|172441|143903|348^6407|α² Her|172442|143900|539^6408|HR6408|173201|-596944|591^6409|HR6409|172844|-326628|555^6410|Sarin|172505|248392|314^6411|ι Aps|173683|-701233|541^6412|HR6412|172706|21861|617^6413|HR6413|172786|-62450|609^6414|HR6414|172755|12106|588^6415|41 Oph|172769|-4453|473^6416|HR6416|173176|-466339|548^6417|ζ Aps|173665|-677706|478^6418|π Her|172508|368092|316^6419|HR6419|172616|237428|596^6420|HR6420|173133|-441297|576^6421|HR6421|172091|628744|556^6422|HR6422|173057|-325533|636^6423|HR6423|173251|-500633|627^6424|ο Oph|173002|-242869|520^6425|ο Oph|173001|-242842|680^6426|HR6426|173159|-349897|591^6427|HR6427|173235|-442231|665^6428|HR6428|173053|-163119|643^6429|HR6429|175242|-808592|588^6430|HR6430|172933|230908|645^6431|68 Her|172888|331000|482^6432|HR6432|173014|173181|600^6433|HR6433|173103|108644|503^6434|HR6434|173147|60853|651^6435|HR6435|173315|-177564|602^6436|69 Her|172945|372917|465^6437|HR6437|172802|496911|748^6438|HR6438|173819|-580103|588^6439|HR6439|173332|-59172|632^6440|HR6440|174003|-628642|570^6441|HR6441|173428|-193328|652^6442|HR6442|173852|-565253|580^6443|HR6443|173135|288231|565^6444|HR6444|173065|388114|594^6445|ξ Oph|173501|-211128|439^6446|ν Ser|173471|-128469|433^6447|HR6447|174052|-606736|577^6448|HR6448|172748|606706|632^6449|HR6449|173480|-106961|646^6450|HR6450|173776|-378050|641^6451|ι Ara|173878|-474683|525^6452|HR6452|173386|180572|500^6453|θ Oph|173668|-249994|327^6454|HR6454|173772|-359100|647^6455|HR6455|173361|255375|538^6456|HR6456|173819|-372206|593^6457|70 Her|173484|244994|512^6458|72 Her|173443|324678|539^6459|43 Oph|173893|-281431|535^6460|HR6460|174036|-441625|512^6461|β Ara|174217|-555300|285^6462|γ Ara|174232|-563775|334^6463|HR6463|173593|167308|635^6464|74 Her|173392|462408|559^6465|HR6465|173809|-23883|629^6466|HR6466|173587|287581|635^6467|HR6467|173427|481883|643^6468|κ Ara|174333|-506336|523^6469|HR6469|173621|399744|551^6470|HR6470|174174|-346964|616^6471|HR6471|174688|-630364|624^6472|HR6472|174117|-214417|585^6473|HR6473|174103|-184458|621^6474|HR6474|174184|-242436|619^6475|HR6475|174490|-519492|619^6476|HR6476|173993|88528|577^6477|HR6477|174476|-458436|529^6478|HR6478|174534|-506303|592^6479|HR6479|173626|534206|567^6480|73 Her|174018|229603|574^6481|HR6481|174087|163011|571^6482|HR6482|174094|156061|635^6483|HR6483|174660|-522972|575^6484|ρ Her|173946|371467|547^6485|ρ Her|173947|371458|452^6486|44 Oph|174395|-241753|417^6487|HR6487|174774|-551697|594^6488|HR6488|1)BSC5";
static const char BSC5_CHUNK_68[] PROGMEM = R"BSC5(74006|385828|649^6489|HR6489|174328|-16517|644^6490|HR6490|174487|-259433|644^6491|HR6491|174075|369519|628^6492|45 Oph|174559|-298669|429^6493|HR6493|174439|-50867|454^6494|HR6494|174604|-297244|600^6495|HR6495|174318|169175|598^6496|HR6496|174506|-125125|621^6497|HR6497|174386|75956|606^6498|σ Oph|174419|41403|434^6499|HR6499|174336|268789|641^6500|δ Ara|175183|-606839|362^6501|HR6501|174822|-367783|602^6502|HR6502|174470|200808|554^6503|HR6503|174905|-385167|639^6504|HR6504|174673|-82083|637^6505|HR6505|175231|-569206|595^6506|HR6506|174462|346958|594^6507|HR6507|174805|3306|544^6508|Lesath|175127|-372958|269^6509|77 Her|174456|482600|585^6510|α Ara|175307|-498761|295^6511|HR6511|174281|600483|565^6512|HR6512|174965|-59197|637^6513|HR6513|175303|-460364|603^6514|HR6514|174347|586519|651^6516|HR6516|175066|-10625|531^6517|HR6517|175298|-337028|644^6518|HR6518|174167|673064|643^6519|51 Oph|175236|-239628|481^6520|HR6520|175290|-262697|605^6521|HR6521|175062|119250|639^6522|HR6522|175401|-342797|617^6523|HR6523|175521|-411736|584^6524|HR6524|175226|27244|559^6525|HR6525|175930|-598461|628^6526|Maasym|175123|261106|441^6527|Shaula|175601|-371039|163^6528|HR6528|175154|311583|561^6529|HR6529|173269|801364|572^6530|HR6530|175889|-533528|610^6531|HR6531|175112|388822|643^6532|HR6532|175375|119303|642^6533|78 Her|175304|284075|562^6534|HR6534|175583|-57447|562^6535|HR6535|175785|-325817|570^6536|Rastaban|175072|523014|279^6537|σ Ara|175943|-465056|459^6538|HR6538|175336|342708|656^6539|HR6539|175953|-374400|648^6540|HR6540|175122|578764|640^6541|HR6541|175563|192567|564^6542|HR6542|175609|163175|569^6543|HR6543|175619|148417|648^6544|HR6544|175796|-112419|555^6545|52 Oph|175885|-220439|657^6546|HR6546|176091|-386353|429^6547|HR6547|176242|-500600|593^6548|53 Oph|175769|95867|581^6549|π Ara|176349|-545003|525^6550|HR6550|175520|412436|574^6551|HR6551|175742|165039|640^6552|HR6552|180261|-852147|645^6553|Sargas|176220|-429978|187^6554|ν¹ Dra|175363|551842|488^6555|Kuma|175378|551731|487^6556|Rasalhague|175822|125600|208^6557|HR6557|176241|-380656|626^6558|HR6558|176357|-428806|610^6559|HR6559|175999|209961|610^6560|HR6560|175588|575589|617^6561|ξ Ser|176264|-153986|354^6562|HR6562|176267|-155711|594^6563|HR6563|175951|373017|610^6564|HR6564|176022|281847|638^6565|HR6565|177388|-722208|649^6566|27 Dra|175328|681350|505^6567|μ Oph|176308|-81189|462^6568|HR6568|176360|-109264|575^6569|λ Ara|176732|-494156|477^6570|HR6570|176102|307853|602^6571|79 Her|176253|243100|577^6572|HR6572|176879|-469219|579^6573|26 Dra|175832|618750|523^6574|82 Her|176104|485856|537^6575|HR6575|176524|20281|626^6576|HR6576|177011|-505106|624^6577|HR6577|176494|133292|612^6578|HR6578|176699|-21525|619^6579|HR6579|176471|327394|637^6580|Girtab|177081|-390300|241^6581|ο Ser|176902|-128753)BSC5";
static const char BSC5_CHUNK_69[] PROGMEM = R"BSC5(|426^6582|η Pav|177622|-647239|362^6583|HR6583|177142|-369458|554^6584|HR6584|176660|312025|603^6585|Cervantes|177358|-518342|515^6586|HR6586|177488|-575453|601^6587|HR6587|177186|-330511|640^6588|ι Her|176578|460064|380^6589|HR6589|176864|151781|634^6590|HR6590|176923|63128|595^6591|HR6591|176781|312875|628^6592|HR6592|176849|245133|636^6593|HR6593|177216|-278842|636^6594|HR6594|176996|159519|552^6595|58 Oph|177238|-216833|487^6596|ω Dra|176159|687581|480^6597|HR6597|177450|-427289|587^6598|HR6598|176110|695708|642^6599|HR6599|176771|434708|659^6600|HR6600|177302|-135086|639^6601|HR6601|177297|-70794|630^6602|83 Her|177079|245642|552^6603|Cebalrai|177246|45672|277^6604|HR6604|177228|142950|624^6605|HR6605|176767|573103|677^6606|HR6606|176191|724558|586^6607|HR6607|176894|518181|599^6608|84 Her|177227|243278|571^6609|61 Oph|177428|25794|617^6610|HR6610|177432|25789|656^6611|HR6611|177381|144103|619^6612|HR6612|177182|440844|634^6613|HR6613|177854|-381117|643^6614|HR6614|178106|-554017|611^6615|ι¹ Sco|177931|-401269|303^6616|3 Sgr|177927|-278308|454^6617|HR6617|177960|-224781|618^6618|HR6618|177331|538017|575^6619|HR6619|177612|315047|623^6620|HR6620|177936|-147258|594^6621|HR6621|178077|-269750|635^6622|HR6622|178412|-536122|592^6623|μ Her|177743|277206|342^6624|HR6624|178599|-601644|578^6625|HR6625|177649|388814|652^6626|HR6626|177662|393225|668^6627|HR6627|177856|176972|572^6628|HR6628|178196|-317033|483^6629|γ Oph|177982|27072|375^6630|HR6630|178310|-370433|321^6631|ι² Sco|178364|-400906|481^6632|HR6632|178531|-531306|609^6633|HR6633|178056|38042|622^6634|HR6634|178884|-654892|649^6635|HR6635|179616|-761775|607^6636|Dsiban|176990|721489|458^6637|ψ¹ Dra|176994|721569|579^6638|HR6638|178069|205656|569^6639|HR6639|178219|19611|647^6640|HR6640|178624|-456006|611^6641|HR6641|177856|476122|643^6642|HR6642|178133|192553|612^6643|HR6643|178591|-407725|596^6644|87 Her|178137|256228|512^6645|HR6645|178535|-305572|666^6646|HR6646|180908|-814864|635^6647|HR6647|178704|-347992|590^6648|HR6648|178721|-344167|584^6649|HR6649|178813|-419967|620^6650|HR6650|178454|119467|617^6651|HR6651|178803|-341142|606^6652|HR6652|178822|-350186|645^6653|HR6653|178828|-356242|603^6654|HR6654|178397|293222|550^6655|HR6655|178468|223164|598^6656|30 Dra|178179|507811|502^6657|HR6657|178888|-347306|617^6658|HR6658|178898|-348953|560^6659|HR6659|178665|-12367|635^6660|HR6660|178960|-347858|638^6661|HR6661|178774|-61436|621^6662|HR6662|178986|-347525|596^6663|HR6663|178995|-348317|642^6664|88 Her|178342|483942|668^6665|HR6665|178662|153258|646^6666|HR6666|178843|-108997|618^6667|HR6667|178765|13050|595^6668|HR6668|179076|-344664|596^6669|HR6669|178539|400725|646^6670|HR6670|178873|61014|577^6671|HR6671|179189|-364758|606^6672|HR6672|179150|-248872|620^6673|HR6673|178680|399822|604^66)BSC5";
static const char BSC5_CHUNK_70[] PROGMEM = R"BSC5(74|HR6674|178669|466433|638^6675|HR6675|179465|-443422|486^6676|HR6676|179039|111306|638^6677|90 Her|178884|400081|516^6678|HR6678|179488|-403056|643^6679|HR6679|179319|-188022|652^6680|HR6680|179450|-280653|580^6681|HR6681|179386|-158125|589^6682|HR6682|179632|-417161|488^6683|HR6683|179661|-391369|629^6684|HR6684|179384|6703|582^6685|89 Her|179237|260500|546^6686|HR6686|179466|-40819|547^6687|HR6687|179308|224642|558^6688|Grumium|178921|568728|375^6689|HR6689|179512|664|597^6690|HR6690|179489|64878|629^6691|HR6691|179821|-368583|574^6692|HR6692|179775|-287592|601^6693|HR6693|179848|-302531|516^6694|HR6694|179849|-302533|704^6695|θ Her|179376|372506|386^6696|HR6696|179575|110442|636^6697|HR6697|179540|239958|630^6698|Sinistra|179838|-97736|334^6699|HR6699|179232|559714|610^6700|4 Sgr|179966|-238161|476^6701|35 Dra|178242|769628|504^6702|HR6702|179468|453508|602^6703|ξ Her|179628|292478|370^6704|HR6704|180000|-203392|621^6705|Eltanin|179434|514889|223^6706|HR6706|179935|-48214|587^6707|ν Her|179751|301894|441^6708|HR6708|180301|-363778|630^6709|HR6709|180043|6294|637^6710|ζ Ser|180081|-36903|462^6711|HR6711|179784|362878|600^6712|66 Oph|180044|43686|464^6713|93 Her|180009|167508|467^6714|67 Oph|180108|29317|397^6715|6 Sgr|180231|-171569|628^6716|HR6716|180318|-227806|577^6717|HR6717|178362|783067|624^6718|HR6718|179812|454761|648^6719|HR6719|180147|62683|634^6720|HR6720|180077|195058|650^6721|χ Oct|189130|-876058|528^6722|HR6722|180159|150933|626^6723|68 Oph|180292|13053|445^6724|7 Sgr|180475|-242822|534^6725|ψ² Dra|179198|720050|545^6726|HR6726|180101|332139|599^6727|HR6727|180505|-227183|674^6728|HR6728|179989|455014|567^6729|95 Her|180250|215953|518^6730|95 Her|180251|215956|496^6731|HR6731|181877|-758914|586^6732|HR6732|180462|-53586|676^6733|τ Oph|180514|-81803|594^6734|τ Oph|180514|-81806|524^6735|HR6735|179074|751708|636^6736|9 Sgr|180646|-243606|597^6737|HR6737|180266|333114|615^6738|96 Her|180398|208336|528^6739|HR6739|180807|-359014|600^6740|HR6740|181301|-645500|641^6741|97 Her|180417|229231|621^6742|γ¹ Sgr|180837|-295800|469^6743|θ Ara|181105|-500917|366^6744|HR6744|180541|196131|650^6745|π Pav|181430|-636683|435^6746|Alnasl|180968|-304242|299^6747|HR6747|180770|19192|614^6748|HR6748|181066|-360197|595^6749|HR6749|181138|-434247|577^6750|HR6750|181138|-434247|577^6751|HR6751|182096|-736717|585^6752|70 Oph|180909|24994|403^6753|HR6753|180525|484644|621^6754|HR6754|180778|239425|634^6755|HR6755|181021|-83239|585^6756|HR6756|181042|-47514|577^6757|HR6757|181021|-4467|634^6758|HR6758|180954|120039|704^6759|HR6759|181417|-457672|615^6760|HR6760|181660|-590400|638^6761|ι Pav|181739|-620022|549^6762|HR6762|181198|-214439|628^6763|HR6763|180917|216469|615^6764|HR6764|180787|400842|652^6765|98 Her|181005|222189|506^6766|HR6766|181347|-284572|457^6)BSC5";
static const char BSC5_CHUNK_71[] PROGMEM = R"BSC5(767|HR6767|180836|419467|634^6768|HR6768|180971|322306|571^6769|HR6769|181301|-171542|552^6770|71 Oph|181218|87339|464^6771|72 Oph|181225|95639|373^6772|HR6772|181562|-366725|658^6773|HR6773|181484|-254725|661^6774|HR6774|182400|-707514|673^6775|99 Her|181171|305619|504^6776|HR6776|181301|130711|663^6777|HR6777|181666|-327197|643^6778|HR6778|181846|-475131|607^6779|ο Her|181257|287625|383^6780|HR6780|181682|-307286|553^6781|100 Her|181304|261014|586^6782|100 Her|181304|260975|590^6783|ε Tel|181872|-459544|453^6784|HR6784|181427|142847|637^6785|HR6785|181621|-139344|639^6786|HR6786|181849|-413592|586^6787|102 Her|181460|208144|436^6788|HR6788|181820|-337997|616^6789|Yildun|175369|865864|436^6790|HR6790|181149|508228|629^6791|HR6791|181247|434617|500^6792|HR6792|181184|497106|632^6793|HR6793|181339|364014|548^6794|101 Her|181480|200453|510^6795|73 Oph|181594|39933|573^6796|HR6796|182378|-636894|647^6797|HR6797|181650|31197|569^6798|HR6798|181874|-198422|636^6799|HR6799|181528|304694|638^6800|HR6800|181779|33242|551^6801|11 Sgr|181954|-237011|498^6802|HR6802|181995|-289014|651^6803|HR6803|181691|164767|609^6804|HR6804|182202|-413361|547^6805|HR6805|182614|-630556|560^6806|HR6806|181604|384575|640^6807|HR6807|181664|364664|558^6808|HR6808|183003|-682292|633^6809|40 Dra|180009|800008|604^6810|41 Dra|180026|800042|568^6811|24 UMi|175133|869681|579^6812|μ Sgr|182294|-210589|386^6813|HR6813|182194|-40117|659^6814|HR6814|181959|334469|588^6815|104 Her|181984|314053|497^6816|14 Sgr|182378|-217131|544^6817|HR6817|181754|542867|595^6818|HR6818|182646|-442067|546^6819|HR6819|182854|-560233|533^6820|HR6820|182212|218803|612^6821|HR6821|182836|-510683|606^6822|15 Sgr|182536|-207283|538^6823|16 Sgr|182536|-203881|595^6824|HR6824|182118|411469|636^6825|HR6825|182586|-186614|607^6826|HR6826|182180|387736|604^6827|HR6827|181853|604094|649^6828|HR6828|183278|-638869|618^6829|φ Oct|183934|-750442|547^6830|HR6830|182661|-36175|636^6831|HR6831|182456|292072|656^6832|η Sgr|182938|-367617|311^6833|HR6833|182934|-341072|616^6834|HR6834|182682|23778|601^6835|HR6835|182900|-286525|619^6836|HR6836|182899|-282892|640^6837|HR6837|184888|-802328|595^6838|HR6838|182866|-173739|575^6839|HR6839|183111|-422883|630^6840|HR6840|182814|-30072|600^6841|HR6841|182913|-184633|654^6842|HR6842|183009|-270425|465^6843|HR6843|182901|-97586|631^6844|HR6844|182847|10058|663^6845|HR6845|182608|421594|559^6846|HR6846|183116|-256047|651^6847|HR6847|182591|452094|629^6848|HR6848|183120|-186194|684^6849|HR6849|182448|565883|637^6850|36 Dra|182316|643972|503^6851|HR6851|183008|137769|630^6852|HR6852|183021|181314|599^6853|HR6853|182852|409367|611^6854|HR6854|183021|232967|663^6855|ξ Pav|183871|-614939|436^6856|HR6856|183487|-374875|645^6857|HR6857|183193|72597|539^6858|HR6858|183358|-158317|539^6859|Kau)BSC5";
static const char BSC5_CHUNK_72[] PROGMEM = R"BSC5(s Media|183499|-298281|270^6860|105 Her|183196|244461|527^6861|HR6861|183587|-249150|625^6862|HR6862|183718|-386569|510^6863|HR6863|183564|-188600|575^6864|HR6864|183667|-284300|616^6865|37 Dra|182547|687558|595^6866|74 Oph|183478|33772|486^6867|HR6867|183311|296661|599^6868|106 Her|183383|219614|495^6869|η Ser|183552|-28989|326^6870|HR6870|183814|-366694|534^6871|HR6871|184254|-630214|614^6872|κ Lyr|183310|360644|433^6873|HR6873|183579|54358|613^6874|HR6874|183914|-362383|555^6875|HR6875|184051|-441103|525^6876|108 Her|183492|298589|563^6877|107 Her|183503|288700|512^6878|HR6878|183839|-102186|633^6879|Kaus Australis|184029|-343847|185^6880|HR6880|183322|513478|630^6881|HR6881|183867|-120147|573^6882|HR6882|183691|232853|541^6883|HR6883|183765|120289|589^6884|ζ Sct|183943|-89342|468^6885|HR6885|183803|178267|525^6886|HR6886|183520|497256|640^6887|HR6887|183841|166881|622^6888|18 Sgr|184171|-307567|560^6889|HR6889|184227|-359917|615^6890|HR6890|184010|-35833|638^6891|HR6891|183591|491217|505^6892|HR6892|184117|-70753|631^6893|HR6893|184318|-339453|630^6894|HR6894|184483|-481169|546^6895|109 Her|183950|217697|384^6896|21 Sgr|184225|-205417|481^6897|α Tel|184496|-459683|351^6898|HR6898|184158|-15794|615^6899|HR6899|185487|-739656|589^6900|HR6900|184191|50847|674^6901|HR6901|183993|387392|636^6902|HR6902|184274|80319|565^6903|Alathfar|184038|395072|512^6904|HR6904|184162|273953|627^6905|ζ Tel|184805|-490708|413^6906|HR6906|184321|149667|637^6907|HR6907|184638|-298164|592^6908|HR6908|184991|-575231|576^6909|HR6909|184622|-266347|631^6910|HR6910|184742|-389956|564^6911|HR6911|183966|533008|632^6912|HR6912|187039|-818078|627^6913|Kaus Borealis|184662|-254217|281^6914|HR6914|184684|-267572|627^6915|HR6915|184869|-438458|636^6916|ν Pav|185229|-622783|464^6917|HR6917|184330|298289|583^6918|59 Ser|184535|1961|521^6919|HR6919|184657|-178000|620^6920|φ Dra|183460|713378|422^6921|HR6921|184880|-388511|663^6922|HR6922|184988|-472203|570^6923|39 Dra|183985|588006|498^6924|HR6924|184447|264494|653^6925|HR6925|184640|37486|607^6926|HR6926|184826|-265817|650^6927|χ Dra|183509|727328|357^6928|HR6928|184663|61942|573^6929|HR6929|184894|-252564|659^6930|γ Sct|184866|-145658|470^6931|HR6931|185175|-419136|604^6932|HR6932|184963|-145817|596^6933|HR6933|185033|-187289|566^6934|δ¹ Tel|185293|-459150|496^6935|60 Ser|184947|-19853|539^6936|HR6936|185180|-329892|534^6937|HR6937|185323|-435078|572^6938|δ² Tel|185339|-457572|507^6939|HR6939|185582|-587092|644^6940|HR6940|185040|-57242|628^6941|HR6941|185014|40653|669^6942|HR6942|185393|-397039|516^6943|HR6943|184932|238661|590^6944|HR6944|185240|-184028|514^6945|Fafnir|184331|655636|482^6946|HR6946|185238|-107958|572^6947|HR6947|185315|-191250|668^6948|HR6948|185503|-398922|622^6949|HR6949|184618|595492|643^6950|HR6950|185116|208)BSC5";
static const char BSC5_CHUNK_73[] PROGMEM = R"BSC5(153|650^6951|θ CrA|185584|-423125|464^6952|κ¹ CrA|185565|-387203|632^6953|κ² CrA|185564|-387261|565^6954|HR6954|185753|-528919|622^6955|HR6955|185179|169286|577^6956|HR6956|185391|-146442|637^6957|61 Ser|185325|-10031|594^6958|HR6958|185353|36597|643^6959|HR6959|185454|-148656|550^6960|HR6960|185661|-330167|528^6961|24 Sgr|185649|-240325|549^6962|HR6962|185608|-148536|576^6963|HR6963|185564|-59114|636^6964|HR6964|188661|-833164|716^6965|25 Sgr|185758|-242225|651^6966|HR6966|185462|236169|584^6967|HR6967|185565|82683|642^6968|HR6968|185472|305542|548^6969|HR6969|185892|-208408|648^6970|HR6970|185840|-109772|514^6971|HR6971|185564|308925|659^6972|HR6972|185999|-296992|637^6973|α Sct|185868|-82442|385^6974|HR6974|185365|521156|656^6975|HR6975|185721|204664|657^6976|HR6976|185799|108917|640^6977|HR6977|185868|182033|578^6978|45 Dra|185429|570456|477^6979|HR6979|185208|654361|659^6980|HR6980|185918|236056|561^6981|HR6981|185981|169756|621^6982|ζ Pav|187172|-714281|401^6983|HR6983|185658|523536|536^6984|HR6984|185871|344578|610^6985|HR6985|186077|91225|539^6986|HR6986|186540|-479097|586^6987|HR6987|186109|66719|545^6988|HR6988|186318|-213978|594^6989|HR6989|186346|-140047|647^6990|HR6990|186419|-235050|581^6991|HR6991|186597|-431861|537^6992|HR6992|186202|114217|642^6993|HR6993|186267|-3094|575^6994|HR6994|187971|-778672|639^6995|HR6995|186192|161983|629^6996|HR6996|187062|-646431|637^6997|HR6997|186103|334689|542^6998|HR6998|186482|-210519|586^6999|HR6999|186399|-31936|649^7000|HR7000|186386|-11133|666^7001|Vega|186156|387836|3^7002|HR7002|186392|88339|640^7003|HR7003|186127|432219|620^7004|HR7004|187270|-645511|578^7005|HR7005|186918|-480950|649^7006|HR7006|184958|775469|564^7007|HR7007|186668|-77906|584^7008|HR7008|186602|52642|638^7009|HR7009|186351|396681|604^7010|HR7010|186643|73583|628^7011|26 Sgr|186977|-238333|623^7012|HR7012|187575|-648714|479^7013|HR7013|186037|654886|606^7014|HR7014|186951|-145642|642^7015|HR7015|187532|-610950|604^7016|HR7016|186672|308494|636^7017|HR7017|186592|409350|625^7018|HR7018|186260|625267|574^7019|HR7019|186701|383672|645^7020|δ Sct|187046|-90525|472^7021|λ CrA|187297|-383236|513^7022|HR7022|187566|-568817|622^7023|HR7023|187153|-192839|635^7024|HR7024|187100|-70733|615^7025|HR7025|184026|831753|617^7026|HR7026|187355|-367186|632^7027|HR7027|188288|-729947|606^7028|HR7028|186647|521961|600^7029|HR7029|187387|-356419|487^7030|HR7030|186948|316178|641^7031|HR7031|187492|-396864|543^7032|ε Sct|187254|-82753|490^7033|HR7033|187022|347467|647^7034|HR7034|187309|-68186|631^7035|HR7035|187471|-250111|583^7036|θ Pav|188105|-650778|573^7037|HR7037|187831|-500944|654^7038|HR7038|187552|-210014|636^7039|φ Sgr|187609|-269908|317^7040|4 Aql|187472|20600|502^7041|HR7041|187213|393003|645^7042|HR7042|186823|627497|609^7043|HR7043|1)BSC5";
static const char BSC5_CHUNK_74[] PROGMEM = R"BSC5(87267|365567|601^7044|HR7044|187310|319267|570^7045|HR7045|187670|-196064|642^7046|28 Sgr|187724|-223922|537^7047|HR7047|187445|235897|631^7048|HR7048|187579|54997|583^7049|46 Dra|187105|555394|504^7050|μ CrA|187957|-404061|524^7051|ε¹ Lyr|187390|396700|506^7052|ε¹ Lyr|187390|396711|602^7053|ε² Lyr|187397|396131|514^7054|ε² Lyr|187397|396128|537^7055|HR7055|187787|-101250|571^7056|ζ¹ Lyr|187462|376050|436^7057|ζ² Lyr|187467|375944|573^7058|HR7058|187599|219850|651^7059|5 Aql|187746|-9617|590^7060|HR7060|187247|538719|611^7061|110 Her|187610|205464|419^7062|η¹ CrA|188140|-436800|549^7063|β Sct|187863|-47478|422^7064|HR7064|187679|266622|483^7065|HR7065|188243|-458103|581^7066|HR7066|187914|-57050|520^7067|HR7067|187782|187058|617^7068|η² CrA|188264|-434339|561^7069|111 Her|187837|181814|436^7070|HR7070|188214|-347486|662^7071|HR7071|187487|548967|623^7072|HR7072|188126|-186014|647^7073|HR7073|187703|414417|607^7074|λ Pav|188703|-621875|422^7075|HR7075|187384|610481|599^7076|HR7076|188008|42414|621^7077|HR7077|188265|-191422|675^7078|29 Sgr|188278|-203247|524^7079|HR7079|188046|235142|615^7080|HR7080|187831|463150|652^7081|HR7081|187993|317569|606^7082|HR7082|187195|707928|644^7083|HR7083|188281|-59128|599^7084|HR7084|187786|529881|588^7085|HR7085|188270|8358|625^7086|HR7086|188148|193286|588^7087|κ Tel|188777|-521075|517^7088|30 Sgr|188474|-221622|661^7089|HR7089|188389|-79075|680^7090|HR7090|187944|490750|640^7091|HR7091|188207|250464|659^7092|HR7092|188742|-465953|554^7093|HR7093|188867|-519311|631^7094|HR7094|188496|-97742|583^7095|HR7095|188840|-483600|619^7096|HR7096|188045|487675|612^7097|HR7097|188832|-465858|619^7098|HR7098|188289|316292|664^7099|HR7099|188460|109764|655^7100|ν¹ Lyr|188294|328128|591^7101|8 Aql|188561|-33178|610^7102|ν² Lyr|188314|325508|525^7103|HR7103|188746|-266503|629^7104|HR7104|188769|-293794|613^7105|HR7105|188782|-307342|663^7106|Sheliak|188347|333628|345^7107|κ Pav|189492|-672336|444^7108|HR7108|189090|-498786|660^7109|HR7109|188672|139656|614^7110|HR7110|188839|-95761|634^7111|HR7111|189485|-628011|648^7112|HR7112|188600|287836|618^7113|112 Her|188712|214253|548^7114|33 Sgr|189000|-213597|569^7115|HR7115|188601|365386|609^7116|ν¹ Sgr|189028|-227450|483^7117|HR7117|187630|740856|527^7118|HR7118|188686|413833|628^7119|HR7119|189120|-156031|510^7120|ν² Sgr|189186|-226714|499^7121|Nunki|189211|-262967|202^7122|HR7122|189380|-427106|536^7123|HR7123|188597|529750|551^7124|50 Dra|187728|754339|535^7125|ο Dra|188534|593883|466^7126|HR7126|189253|-163767|579^7127|ω Pav|189768|-602006|514^7128|HR7128|189335|-231736|593^7129|HR7129|189446|-373433|538^7130|HR7130|190010|-666533|601^7131|δ¹ Lyr|188954|369717|558^7132|HR7132|189037|279094|562^7133|113 Her|189125|226450|459^7134|λ Tel|189744|-529386|487^7135|HR7135|189243|66153|557)BSC5";
static const char BSC5_CHUNK_75[] PROGMEM = R"BSC5(^7136|HR7136|189596|-398233|631^7137|HR7137|188871|507083|492^7138|HR7138|189040|412256|730^7139|δ² Lyr|189084|368989|430^7140|HR7140|189146|339686|602^7141|Alya|189370|42036|462^7142|θ² Ser|189374|42019|498^7143|HR7143|189396|-18000|622^7144|HR7144|189404|24711|615^7145|ξ¹ Sgr|189557|-206564|508^7146|HR7146|189145|416028|544^7147|HR7147|189344|179950|663^7148|HR7148|189351|181053|569^7149|η Sct|189510|-58461|483^7150|ξ² Sgr|189622|-211067|351^7151|HR7151|189726|-310361|612^7152|ε CrA|189787|-371075|487^7153|HR7153|188962|574869|622^7154|HR7154|189131|488597|577^7155|HR7155|189724|-248767|662^7156|HR7156|189864|-395347|649^7157|13 Lyr|189223|439461|404^7158|64 Ser|189546|25353|557^7159|HR7159|189735|-225294|614^7160|HR7160|187606|799425|639^7161|HR7161|190582|-687553|588^7162|HR7162|189504|329014|522^7163|HR7163|189733|62403|621^7164|HR7164|189908|-185669|637^7165|HR7165|189708|173608|538^7166|HR7166|189899|-128406|553^7167|10 Aql|189797|139067|589^7168|HR7168|190069|-249422|636^7169|HR7169|190176|-370608|669^7170|HR7170|190179|-370619|640^7171|HR7171|189792|197942|650^7172|11 Aql|189849|136225|523^7173|HR7173|189882|101408|675^7174|HR7174|189672|382661|589^7175|48 Dra|189458|578150|566^7176|ε Aql|189937|150683|402^7177|HR7177|190358|-419100|623^7178|Sulafat|189824|326894|324^7179|HR7179|189796|406792|622^7180|υ Dra|189066|712972|482^7181|HR7181|189960|262306|527^7182|HR7182|190272|-226956|624^7183|HR7183|189995|228147|629^7184|HR7184|189579|582253|646^7185|HR7185|189868|392178|641^7186|HR7186|190260|-152825|632^7187|HR7187|189404|652581|563^7188|ζ CrA|190519|-420953|475^7190|HR7190|190659|-510186|593^7191|HR7191|189548|623967|645^7192|λ Lyr|190002|321456|493^7193|12 Aql|190280|-57389|402^7194|Ascella|190435|-298803|260^7195|HR7195|190410|-248469|565^7196|HR7196|189832|508094|630^7197|HR7197|190549|-382533|574^7198|HR7198|190182|193097|639^7199|HR7199|188926|757875|622^7200|HR7200|190229|208336|669^7201|HR7201|190053|406842|665^7202|HR7202|190215|262914|569^7203|HR7203|190511|-192453|605^7204|HR7204|190153|338022|601^7205|HR7205|190519|-191033|637^7206|HR7206|190264|250258|672^7207|HR7207|190304|222639|640^7208|HR7208|190393|83742|630^7209|14 Aql|190485|-36989|542^7210|HR7210|190038|505336|538^7211|HR7211|190736|-310469|550^7212|HR7212|190301|336214|639^7213|ρ Tel|191055|-523408|516^7214|HR7214|190589|18189|583^7215|16 Lyr|190240|469347|501^7216|HR7216|190479|196611|609^7217|ο Sgr|190781|-217417|377^7218|49 Dra|190121|556583|548^7219|HR7219|190696|33306|673^7220|HR7220|190734|-56850|690^7221|HR7221|191646|-684247|533^7222|HR7222|190618|212678|652^7223|HR7223|191154|-482992|597^7224|HR7224|189813|695311|652^7225|15 Aql|190827|-40314|542^7226|γ CrA|191070|-370633|493^7227|γ CrA|191070|-370633|499^7228|Polaris Australis|211462|-889564|547^7229|HR7229|19)BSC5";
static const char BSC5_CHUNK_76[] PROGMEM = R"BSC5(0353|522611|631^7230|HR7230|190948|-156603|597^7231|HR7231|190885|-15128|653^7232|HR7232|191146|-378103|616^7233|HR7233|191478|-557203|649^7234|τ Sgr|191157|-276706|332^7235|Deneb el Okab|190902|138633|299^7236|Al Thalimain|191041|-48825|344^7237|HR7237|190828|317444|556^7238|HR7238|190829|307333|606^7239|HR7239|191145|-162289|603^7240|HR7240|191252|-286369|604^7241|HR7241|191190|-187378|629^7242|δ CrA|191391|-404967|459^7243|HR7243|191062|82300|609^7244|HR7244|190964|299217|631^7245|HR7245|191192|6417|656^7246|HR7246|191374|-246569|630^7247|HR7247|189659|770508|654^7248|18 Aql|191163|110714|509^7249|HR7249|191380|-192900|554^7250|HR7250|191107|242508|577^7251|51 Dra|190820|533967|538^7252|HR7252|190861|499233|643^7253|HR7253|191105|286286|555^7254|Alfecca Meridiana|191579|-379044|411^7255|HR7255|191610|-398281|646^7256|HR7256|191601|-361647|656^7257|HR7257|191661|-418925|588^7258|HR7258|191047|414139|649^7259|β CrA|191671|-393408|411^7260|HR7260|191326|168533|607^7261|17 Lyr|191238|325017|523^7262|ι Lyr|191217|361003|528^7263|HR7263|191343|216989|623^7264|Albaldah|191627|-210236|289^7265|HR7265|191634|-198036|613^7266|19 Aql|191500|60733|522^7267|HR7267|191445|168514|648^7268|HR7268|191838|-390050|636^7269|HR7269|191643|-4281|634^7270|HR7270|191886|-295022|630^7271|HR7271|192128|-504864|613^7272|HR7272|191512|346006|674^7273|HR7273|192027|-375828|657^7274|τ Pav|192746|-691906|627^7275|HR7275|191405|524256|581^7276|HR7276|192078|-216581|641^7277|HR7277|192205|-259067|580^7278|HR7278|192867|-666614|553^7279|20 Aql|192113|-79394|534^7280|HR7280|191919|267358|636^7281|HR7281|192444|-451933|592^7282|HR7282|192210|-122825|551^7283|19 Lyr|191961|312833|598^7284|HR7284|191898|404292|618^7285|HR7285|192096|168464|673^7286|HR7286|192102|215544|593^7287|21 Aql|192285|22936|515^7288|HR7288|192289|55158|649^7289|HR7289|192727|-454664|540^7290|55 Dra|191627|659786|625^7291|HR7291|192592|-241792|625^7292|ψ Sgr|192590|-252567|485^7293|HR7293|192013|498542|675^7294|HR7294|192014|498561|657^7295|53 Dra|191946|568592|512^7296|HR7296|192758|-335217|625^7297|HR7297|193026|-533867|638^7298|Aladfar|192293|391461|439^7299|HR7299|192507|202033|600^7300|HR7300|192556|150836|557^7301|1 Sge|192548|212322|564^7302|HR7302|192569|305264|585^7303|22 Aql|192753|48347|559^7304|43 Sgr|192939|-189531|496^7305|HR7305|192658|274556|654^7306|1 Vul|192703|213903|477^7307|HR7307|192741|145447|563^7308|HR7308|192665|279269|616^7309|54 Dra|192320|577050|499^7310|Altais|192092|676617|307^7311|HR7311|192554|500708|627^7312|59 Dra|191527|765606|513^7313|HR7313|192967|20317|619^7314|θ Lyr|192728|381336|436^7315|ω¹ Aql|192969|115953|528^7316|HR7316|193278|-354214|559^7317|HR7317|193167|-155364|606^7318|2 Vul|192954|230256|543^7319|23 Aql|193090|10853|510^7320|HR7320|194015|-683711|634^7321|24 Aql)BSC5";
static const char BSC5_CHUNK_77[] PROGMEM = R"BSC5(|193141|3392|641^7322|HR7322|192809|469992|600^7323|HR7323|193406|-318178|658^7324|HR7324|193002|310222|668^7325|HR7325|193146|96181|632^7326|HR7326|193135|196106|658^7327|HR7327|193439|-224025|558^7328|κ Cyg|192851|533686|377^7329|η Tel|193809|-544236|505^7330|HR7330|193583|-349839|648^7331|28 Aql|193276|123747|553^7332|ω² Aql|193314|115350|602^7333|26 Aql|193425|-54158|501^7334|HR7334|193693|-420161|634^7335|HR7335|193177|333889|660^7336|27 Aql|193432|-8922|549^7337|Arkab Prior|193773|-444589|401^7338|HR7338|193170|374453|622^7339|HR7339|193603|-192342|626^7340|ρ¹ Sgr|193612|-178472|393^7341|HR7341|193105|495697|631^7342|υ Sgr|193621|-159550|461^7343|Arkab Posterior|193870|-447997|429^7344|ρ² Sgr|193641|-183083|587^7345|HR7345|193275|373306|631^7346|HR7346|193425|351861|631^7347|HR7347|193724|-82011|631^7348|Rukbat|193981|-406161|397^7349|HR7349|193726|-2525|583^7350|HR7350|194059|-437231|617^7351|HR7351|193268|543761|626^7352|τ Dra|192592|733556|445^7353|HR7353|193846|-74003|632^7354|HR7354|193801|99131|635^7355|HR7355|194084|-278656|604^7356|HR7356|193378|576453|591^7357|HR7357|193856|149211|664^7358|3 Vul|193808|262625|518^7359|HR7359|193759|335181|606^7360|HR7360|194178|-293092|593^7361|HR7361|193295|643908|652^7362|χ¹ Sgr|194212|-245086|503^7363|χ³ Sgr|194249|-239622|543^7364|HR7364|193964|202644|640^7365|HR7365|193571|577669|643^7366|HR7366|194171|-48842|652^7367|HR7367|194227|-138969|569^7368|HR7368|193928|332222|637^7369|2 Sge|194061|169378|625^7370|HR7370|194634|-543253|569^7371|π Dra|193445|657147|459^7372|2 Cyg|194021|296214|497^7373|31 Aql|194162|119444|516^7374|HR7374|194062|280878|653^7375|50 Sgr|194387|-217767|559^7376|HR7376|194017|364519|636^7377|δ Aql|194250|31147|336^7378|HR7378|194364|-150531|572^7379|HR7379|194402|-145511|670^7380|HR7380|194490|-297433|567^7381|HR7381|193899|502714|651^7382|HR7382|193990|433881|584^7383|HR7383|195197|-684339|596^7384|HR7384|194229|202714|631^7385|4 Vul|194246|197986|516^7386|HR7386|194238|249128|619^7387|ν Aql|194420|3386|466^7388|HR7388|194979|-554414|613^7389|HR7389|194400|130239|574^7390|5 Vul|194370|200978|563^7391|HR7391|194413|198914|581^7392|HR7392|194900|-434458|571^7393|μ Tel|195096|-551100|630^7394|λ UMi|172824|890378|638^7395|4 Cyg|194359|363178|515^7396|HR7396|194594|142825|632^7397|HR7397|194724|29303|585^7398|HR7398|194978|-269856|552^7399|HR7399|195041|-320922|660^7400|35 Aql|194836|19503|580^7401|HR7401|194296|580272|660^7402|HR7402|194893|-70439|661^7403|HR7403|194601|379411|634^7404|HR7404|194883|2461|625^7405|Lucida Anseris|194784|246650|444^7406|8 Vul|194825|247686|581^7407|HR7407|194895|145958|556^7408|ι¹ Cyg|194572|523206|575^7409|7 Vul|194891|202797|633^7410|HR7410|195150|-213122|613^7411|HR7411|195483|-531858|575^7412|HR7412|195029|29042|609^7413|HR7413|194407|625572|638)BSC5";
static const char BSC5_CHUNK_78[] PROGMEM = R"BSC5(^7414|36 Aql|195111|-27889|503^7415|HR7415|195092|34444|605^7416|HR7416|195560|-452719|561^7417|Albireo|195120|279597|308^7418|β² Cyg|195126|279653|511^7419|HR7419|195130|362286|625^7420|ι² Cyg|194951|517297|379^7421|HR7421|195227|266172|587^7422|HR7422|195690|-400347|589^7423|HR7423|193612|796028|605^7424|ι Tel|195869|-480992|490^7425|HR7425|192522|834628|653^7426|8 Cyg|195295|344531|474^7427|HR7427|195220|503067|553^7428|HR7428|195204|557319|637^7429|μ Aql|195682|73789|445^7430|37 Aql|195854|-105603|512^7431|51 Sgr|196005|-247192|565^7432|HR7432|195916|-74603|634^7433|HR7433|195927|-122528|627^7434|HR7434|196405|-579833|618^7435|HR7435|196645|-666856|639^7436|HR7436|195601|387622|661^7437|9 Vul|195764|197733|500^7438|HR7438|195903|29133|638^7439|HR7439|196072|-188528|611^7440|52 Sgr|196118|-248836|460^7441|9 Cyg|195808|294631|538^7442|HR7442|195616|492625|596^7443|HR7443|196176|-182311|564^7444|HR7444|195781|424128|535^7445|HR7445|196022|111500|668^7446|κ Aql|196149|-70275|495^7447|Al Thalimain|196120|-12864|436^7448|HR7448|195528|601586|629^7449|HR7449|196044|143917|638^7450|HR7450|195167|709894|607^7451|HR7451|195722|512367|573^7452|HR7452|196023|225858|632^7453|HR7453|195777|481647|667^7454|HR7454|196262|-143017|547^7455|HR7455|196938|-658542|609^7456|HR7456|196146|112733|598^7457|11 Cyg|195968|369444|605^7458|HR7458|196105|203328|714^7459|HR7459|196718|-544175|626^7460|42 Aql|196298|-46475|546^7461|HR7461|196617|-452781|625^7462|Alsafi|195393|696611|468^7463|ε Sge|196215|164628|566^7464|HR7464|196655|-394333|661^7465|HR7465|195989|502386|652^7466|HR7466|196193|293336|643^7467|HR7467|196157|383839|650^7468|HR7468|196105|446950|517^7469|θ Cyg|196074|502211|448^7470|53 Sgr|196637|-234278|634^7471|HR7471|196469|33817|635^7472|HR7472|196382|207828|648^7473|HR7473|196686|-234286|597^7474|σ Aql|196532|53978|517^7475|HR7475|196571|165714|638^7476|54 Sgr|196787|-162933|620^7477|HR7477|196324|492844|647^7478|φ Cyg|196563|301533|469^7479|Sham|196683|180139|437^7480|45 Aql|196787|-6211|567^7481|HR7481|196625|339792|610^7482|HR7482|196745|204767|650^7483|14 Cyg|196574|428183|540^7484|HR7484|196448|549739|582^7485|HR7485|196777|237175|664^7486|HR7486|196849|138156|601^7487|HR7487|196596|459581|620^7488|β Sge|196841|174761|437^7489|55 Sgr|197086|-161239|506^7490|HR7490|196875|224528|636^7491|HR7491|197271|-375389|616^7492|HR7492|196781|430778|616^7493|46 Aql|197036|121933|634^7494|HR7494|199338|-813497|639^7495|HR7495|196806|455250|506^7496|HR7496|197260|-154700|549^7497|χ Aql|197094|118267|527^7498|HR7498|198237|-725033|541^7499|HR7499|196993|402539|623^7500|HR7500|196703|605072|651^7501|HR7501|197136|293317|649^7502|HR7502|197124|324267|594^7503|16 Cyg|196969|505253|596^7504|HR7504|196978|505175|620^7505|HR7505|197193|306786|605^7506|10 Vul|197286|257719|54)BSC5";
static const char BSC5_CHUNK_79[] PROGMEM = R"BSC5(9^7507|HR7507|197670|-319086|552^7508|HR7508|197322|271356|628^7509|HR7509|197011|554633|648^7510|ν Tel|198003|-563625|535^7511|ψ Aql|197428|133028|626^7512|HR7512|197309|341625|605^7513|HR7513|198315|-668128|645^7514|HR7514|197292|417731|584^7515|56 Sgr|197727|-197611|486^7516|HR7516|197645|-28833|648^7517|15 Cyg|197379|373544|489^7518|HR7518|197469|292647|682^7519|υ Aql|197611|76133|591^7520|HR7520|197439|344139|657^7521|HR7521|198153|-528881|625^7522|HR7522|197207|580164|622^7523|HR7523|197469|407167|634^7524|HR7524|198504|-656050|605^7525|Tarazed|197710|106133|272^7526|HR7526|197277|570425|627^7527|HR7527|198394|-610614|621^7528|Al Fawaris|197496|451308|287^7529|HR7529|197610|360911|643^7530|HR7530|197643|350128|609^7531|HR7531|198458|-591931|542^7532|HR7532|198008|-137033|611^7533|HR7533|197776|251339|662^7534|17 Cyg|197738|337278|499^7535|HR7535|197764|328886|618^7536|δ Sge|197898|185342|382^7537|HR7537|198372|-475572|594^7538|HR7538|198199|-287889|605^7540|HR7540|197968|253839|595^7541|HR7541|198173|-108708|604^7542|HR7542|198084|106942|644^7543|HR7543|197911|384075|577^7544|π Aql|198117|118158|572^7545|HR7545|197385|693369|592^7546|ζ Sge|198163|191422|500^7547|HR7547|197908|479078|612^7548|HR7548|198771|-549711|574^7549|HR7549|198775|-549767|650^7550|HR7550|198122|353114|653^7551|HR7551|198141|334372|644^7552|HR7552|198641|-398744|533^7553|51 Aql|198463|-107636|539^7554|HR7554|198382|79025|651^7555|HR7555|198243|387100|611^7556|HR7556|198318|284406|638^7557|Altair|198464|88683|77^7558|HR7558|199112|-611708|624^7559|HR7559|198531|-24608|613^7560|ο Aql|198504|104156|511^7561|57 Sgr|198700|-190450|592^7562|HR7562|198549|96303|625^7563|HR7563|197791|684383|634^7564|χ Cyg|198428|329142|423^7565|12 Vul|198511|226100|495^7566|19 Cyg|198428|387225|512^7567|HR7567|198437|405997|569^7568|HR7568|198464|378264|606^7569|HR7569|198676|116289|613^7570|η Aql|198746|10056|390^7571|HR7571|198851|-146031|648^7572|HR7572|198710|103514|654^7573|HR7573|198671|249922|557^7574|9 Sge|198727|186719|623^7575|HR7575|198885|-31144|565^7576|20 Cyg|198438|529881|503^7577|HR7577|198554|473772|620^7578|HR7578|199049|-239411|618^7579|HR7579|199781|-691639|575^7580|HR7580|198896|44006|653^7581|ι Sgr|199210|-418683|413^7582|Tyl|198029|702678|383^7583|HR7583|198712|364322|610^7584|56 Aql|199023|-85742|579^7585|HR7585|199181|-330464|646^7586|HR7586|199494|-579256|653^7587|HR7587|199517|-589014|526^7588|HR7588|199814|-687622|639^7589|HR7589|198664|470275|562^7590|ε Pav|200099|-729106|396^7591|HR7591|198687|479319|591^7592|13 Vul|198910|240797|458^7593|57 Aql|199104|-82272|571^7594|57 Aql|199106|-82372|649^7595|Libertas|199041|84614|471^7596|58 Aql|199124|2736|561^7597|Terebellum|199307|-262994|470^7598|HR7598|199112|71403|615^7599|HR7599|199221|-67342|651^7600|HR7600|198837|478075)BSC5";
static const char BSC5_CHUNK_80[] PROGMEM = R"BSC5(|629^7601|HR7601|199086|243194|552^7602|Alshain|199219|64067|371^7603|μ¹ Pav|200064|-669494|576^7604|59 Sgr|199491|-271700|452^7605|HR7605|199615|-380583|655^7606|HR7606|199134|369961|576^7607|HR7607|199185|301953|657^7608|23 Cyg|198882|575236|514^7609|10 Sge|199337|166347|536^7610|φ Aql|199373|114239|528^7611|HR7611|198932|597089|606^7612|μ² Pav|200312|-669442|531^7613|22 Cyg|199310|384867|494^7614|61 Sgr|199658|-154914|502^7615|η Cyg|199384|350833|389^7616|HR7616|199501|209981|648^7617|HR7617|199823|-305381|628^7618|60 Sgr|199826|-261956|483^7619|ψ Cyg|199272|524389|492^7620|HR7620|199456|362508|602^7621|HR7621|200070|-493511|617^7622|11 Sge|199626|167892|553^7623|θ¹ Sgr|199956|-352764|437^7624|θ² Sgr|199976|-346978|530^7625|HR7625|200291|-593761|513^7626|HR7626|199228|582503|609^7627|HR7627|200074|-430433|614^7628|HR7628|199539|403681|545^7629|HR7629|200044|-377022|595^7630|HR7630|200134|-451131|581^7631|HR7631|200056|-337039|566^7632|HR7632|199459|509025|643^7633|HR7633|199321|588461|496^7634|HR7634|199386|566869|612^7635|γ Sge|199793|194922|347^7636|HR7636|199896|13778|617^7637|HR7637|199965|-99583|588^7638|HR7638|199656|422608|643^7639|HR7639|200240|-408142|629^7640|HR7640|199772|309836|549^7641|14 Vul|199863|231014|567^7642|HR7642|199762|381056|632^7643|HR7643|200233|-227372|601^7644|HR7644|200924|-673208|607^7645|13 Sge|200009|175167|537^7646|HR7646|199890|457722|592^7647|25 Cyg|199987|370428|519^7648|HR7648|200164|85581|591^7649|63 Sgr|200329|-136369|571^7650|62 Sgr|200443|-277097|458^7651|HR7651|199876|520558|615^7652|HR7652|200593|-379408|477^7653|15 Vul|200184|277536|464^7654|HR7654|199746|635342|596^7655|HR7655|200209|370989|620^7656|HR7656|200291|248003|588^7657|16 Vul|200337|249381|522^7658|HR7658|200623|-225956|645^7659|HR7659|200721|-320564|499^7660|26 Cyg|200227|501047|505^7661|HR7661|200670|-74697|672^7662|HR7662|200546|185006|596^7663|HR7663|201390|-663547|645^7664|HR7664|200583|160314|567^7665|δ Pav|201454|-661819|356^7666|HR7666|199783|703669|633^7667|62 Aql|200731|-7094|568^7668|HR7668|200922|-330000|653^7669|τ Aql|200690|72781|552^7670|HR7670|200604|298967|571^7671|HR7671|200848|-115994|634^7672|15 Sge|200684|170700|580^7673|ξ Tel|201231|-528808|494^7674|HR7674|201264|-550164|626^7675|65 Sgr|200907|-126653|655^7676|64 Dra|200246|648211|527^7677|HR7677|200829|232103|645^7678|HR7678|200767|322186|564^7679|η Sge|200860|199911|510^7680|HR7680|200907|155003|634^7681|HR7681|201034|-40783|647^7682|65 Dra|200390|646344|657^7683|HR7683|200861|384783|619^7684|HR7684|200747|482297|616^7685|ρ Dra|200470|678736|451^7686|69 Dra|199935|764814|620^7687|HR7687|200852|518394|614^7688|17 Vul|201148|236144|507^7689|27 Cyg|201061|359725|536^7690|64 Aql|201338|-6783|599^7691|HR7691|201853|-575239|637^7692|HR7692|200893|563414|621^7693|HR7693|201306)BSC5";
static const char BSC5_CHUNK_81[] PROGMEM = R"BSC5(|93997|643^7694|HR7694|201420|-100628|618^7695|HR7695|200791|638906|626^7696|HR7696|201351|166642|642^7697|HR7697|201038|531658|585^7698|HR7698|204151|-833106|617^7699|HR7699|201282|344231|611^7700|HR7700|201440|107258|631^7701|66 Dra|200924|619956|539^7702|HR7702|201199|502292|654^7703|HR7703|201866|-361011|532^7704|HR7704|200815|680272|628^7705|θ Sge|201657|209153|648^7706|HR7706|202066|-427806|622^7707|HR7707|202408|-634158|609^7708|28 Cyg|201571|368397|493^7709|HR7709|201861|-88422|649^7710|θ Aql|201884|-8214|323^7711|18 Vul|201760|269042|552^7712|ξ¹ Cap|201994|-123925|634^7713|HR7713|201843|211347|622^7714|HR7714|202386|-524456|565^7715|ξ² Cap|202072|-126175|585^7716|HR7716|201892|218756|626^7717|HR7717|202098|8675|627^7718|19 Vul|201967|268089|549^7719|20 Vul|202002|264789|592^7720|66 Aql|202205|-10094|547^7721|HR7721|202011|477369|692^7722|HR7722|202548|-270328|573^7723|HR7723|202279|242389|656^7724|ρ Aql|202379|151975|495^7725|HR7725|202641|-300053|630^7726|HR7726|202088|514636|601^7727|68 Dra|201930|620786|575^7728|HR7728|202732|-364544|639^7729|HR7729|202740|-351978|653^7730|30 Cyg|202217|468158|483^7731|21 Vul|202374|286947|518^7732|HR7732|203175|-632311|627^7733|HR7733|202286|433792|614^7734|HR7734|202347|366053|645^7735|31 Cyg|202272|467414|379^7736|29 Cyg|202422|368064|497^7737|HR7737|202393|421036|671^7738|3 Cap|202730|-123369|632^7739|HR7739|202544|255919|478^7740|33 Cyg|202233|565678|430^7741|22 Vul|202584|235086|515^7742|HR7742|202244|606406|579^7743|HR7743|202566|337294|566^7744|23 Vul|202628|278142|452^7745|HR7745|203156|-477108|631^7746|18 Sge|202721|215986|613^7747|Al Giedi|202941|-125083|424^7748|4 Cap|203004|-218100|587^7749|HR7749|203216|-475803|613^7750|κ Cep|201481|777114|439^7751|32 Cyg|202579|477144|398^7752|HR7752|202676|388978|627^7753|24 Vul|202798|246711|532^7754|Algedi|203009|-125447|357^7755|HR7755|202621|502328|631^7756|HR7756|202668|455794|591^7757|HR7757|202745|370564|648^7758|HR7758|203423|-550508|627^7759|HR7759|202820|403650|524^7760|HR7760|202921|291481|622^7761|σ Cap|203232|-191186|528^7762|HR7762|202914|427219|629^7763|34 Cyg|202964|380331|481^7764|HR7764|203411|-291972|630^7765|HR7765|203477|-356736|646^7766|HR7766|203614|-499994|627^7767|HR7767|203019|407322|584^7768|HR7768|203287|-10786|606^7769|36 Cyg|203079|370000|558^7770|35 Cyg|203109|349828|517^7771|HR7771|203248|132169|621^7772|HR7772|203406|-63617|663^7773|Alshat|203444|-127592|476^7774|HR7774|203334|135481|595^7775|HR7775|203463|-147850|610^7776|Dabih|203502|-147814|308^7777|HR7777|203138|463228|645^7778|HR7778|203390|145692|613^7779|κ¹ Sgr|203743|-420497|559^7780|HR7780|203393|177931|580^7781|HR7781|203069|553972|576^7782|HR7782|203301|371325|657^7783|HR7783|202920|668539|593^7784|HR7784|203376|394033|623^7785|HR7785|205549|-809650|577^7786|HR77)BSC5";
static const char BSC5_CHUNK_82[] PROGMEM = R"BSC5(86|203322|468375|650^7787|κ² Sgr|203981|-424228|564^7788|HR7788|203836|-96547|630^7789|25 Vul|203676|244461|554^7790|Peacock|204275|-567350|194^7791|HR7791|203418|535961|618^7792|71 Dra|203269|622575|572^7793|HR7793|203812|145514|617^7794|HR7794|203863|53431|531^7795|HR7795|203675|411314|639^7796|Sadr|203705|402567|220^7797|HR7797|203771|312650|609^7798|HR7798|203682|457950|558^7799|HR7799|204300|-407964|609^7800|HR7800|203792|410261|593^7801|HR7801|204241|-286633|585^7802|HR7802|203821|429833|620^7803|HR7803|204104|10686|615^7804|HR7804|203350|688803|555^7805|HR7805|203532|639803|569^7806|39 Cyg|203977|321900|443^7807|HR7807|203957|374764|590^7808|HR7808|204481|-374031|625^7809|HR7809|204285|-28003|611^7810|HR7810|204289|100564|633^7811|HR7811|204279|214097|566^7812|HR7812|206385|-812889|591^7813|HR7813|204337|198650|641^7814|Okul|204553|-182117|525^7815|HR7815|204090|535519|651^7816|HR7816|204398|173156|622^7817|HR7817|204796|-355958|610^7818|HR7818|204181|596000|644^7819|HR7819|204788|-157417|641^7820|HR7820|204688|84375|625^7821|68 Aql|204736|-33578|613^7822|ρ Cap|204810|-178136|478^7823|HR7823|204521|343289|639^7824|HR7824|204713|29369|621^7825|HR7825|204920|-223917|616^7826|40 Cyg|204595|384403|562^7827|HR7827|204399|566389|636^7828|43 Cyg|204506|493833|569^7829|ο Cap|204979|-185867|674^7830|ο Cap|204983|-185833|594^7831|69 Aql|204942|-28856|491^7832|HR7832|205158|-291125|639^7833|HR7833|204892|200878|655^7834|41 Cyg|204899|303686|401^7835|42 Cyg|204890|364547|588^7836|1 Del|205050|108958|608^7837|HR7837|205179|-150564|612^7838|HR7838|205977|-696111|611^7839|HR7839|205161|206058|618^7840|HR7840|205203|112606|711^7841|HR7841|205000|459286|641^7842|HR7842|205479|-249439|636^7843|HR7843|204909|560681|591^7844|ω¹ Cyg|205010|489517|495^7845|HR7845|205399|-98533|565^7846|ν Mic|205653|-445161|511^7847|44 Cyg|205164|369358|619^7848|φ¹ Pav|205930|-605817|476^7849|HR7849|205328|258044|634^7850|θ Cep|204930|629942|422^7851|Ruchba|205219|492203|544^7852|Deneb Dulfim|205536|113033|403^7853|HR7853|205821|-380897|644^7854|HR7854|205225|523097|618^7855|HR7855|205699|-137211|613^7856|HR7856|205798|-304736|640^7857|HR7857|205649|100597|656^7858|η Del|205658|130272|538^7859|ρ Pav|206265|-615300|488^7860|HR7860|205296|567800|614^7861|HR7861|205479|431917|660^7862|HR7862|205694|209853|648^7863|μ¹ Oct|207008|-761806|600^7864|μ² Oct|206955|-753506|655^7865|HR7865|205923|-165258|619^7866|47 Cyg|205651|352508|461^7867|HR7867|205634|417722|649^7868|HR7868|205002|725317|627^7869|α Ind|206261|-472914|311^7870|HR7870|205653|466939|578^7871|ζ Del|205885|146742|468^7872|HR7872|206643|-629078|622^7873|70 Aql|206121|-25500|489^7874|26 Vul|206023|258825|641^7875|φ² Pav|206674|-605489|512^7876|HR7876|205807|518542|611^7877|HR7877|206311|-251089|636^7878|HR7878|206218|969|622^787)BSC5";
static const char BSC5_CHUNK_83[] PROGMEM = R"BSC5(9|73 Dra|205251|749547|520^7880|27 Vul|206180|264619|559^7881|υ Pav|206992|-667608|515^7882|Rotanev|206258|145953|363^7883|ι Del|206303|113778|543^7884|71 Aql|206390|-11053|432^7885|48 Cyg|206255|315725|632^7886|HR7886|206318|182692|625^7887|HR7887|206257|315219|649^7888|HR7888|206232|383286|620^7889|τ Cap|206546|-149547|522^7890|HR7890|206537|-24128|622^7891|29 Vul|206420|212011|482^7892|θ Del|206455|133150|572^7893|HR7893|206722|-334319|547^7894|28 Vul|206422|241161|504^7895|HR7895|206431|236806|591^7896|κ Del|206522|100861|505^7897|1 Aqr|206569|4864|516^7898|HR7898|206699|-237739|637^7899|HR7899|206514|158381|597^7900|υ Cap|206675|-181386|510^7901|75 Dra|204707|814228|546^7902|HR7902|206767|-266450|651^7903|HR7903|206529|218172|608^7904|HR7904|206499|303344|568^7905|HR7905|206757|-161242|580^7906|Sualocin|206606|159119|377^7907|HR7907|206644|112497|642^7908|74 Dra|204910|810914|596^7909|HR7909|206899|-315983|576^7910|HR7910|206900|-260000|628^7911|HR7911|206592|405794|606^7912|HR7912|206564|456669|658^7913|β Pav|207493|-662031|342^7914|HR7914|206792|199353|645^7915|HR7915|207147|-395586|629^7916|HR7916|206501|560050|648^7917|HR7917|206767|298053|608^7918|10 Del|206878|145831|599^7919|HR7919|206675|434586|595^7920|η Ind|207340|-519211|451^7921|49 Cyg|206841|323072|551^7922|HR7922|206834|390822|651^7923|HR7923|206995|175214|622^7924|Deneb|206905|452803|125^7925|HR7925|206716|605053|601^7926|HR7926|206990|417169|567^7927|HR7927|207062|354561|666^7928|δ Del|207243|150744|443^7929|51 Cyg|207035|503400|539^7930|HR7930|204842|836256|619^7931|HR7931|207537|-272472|650^7932|HR7932|207234|355878|647^7933|HR7933|207722|-391992|550^7934|σ Pav|208217|-687764|541^7935|HR7935|207718|-361203|649^7936|ψ Cap|207682|-252708|414^7937|17 Cap|207694|-215142|593^7938|HR7938|207110|606014|615^7939|30 Vul|207479|252706|491^7940|HR7940|207204|571142|632^7941|HR7941|207578|180903|638^7942|52 Cyg|207610|307197|422^7943|ι Mic|208081|-439886|511^7944|HR7944|207394|564881|578^7945|4 Cep|207197|666575|558^7946|HR7946|207843|-24867|627^7947|γ¹ Del|207774|161244|514^7948|γ² Del|207776|161242|427^7949|ε Cyg|207702|339703|246^7950|Albali|207946|-94958|377^7951|3 Aqr|207956|-50278|442^7952|ζ Ind|208247|-462269|489^7953|13 Del|207968|60083|558^7954|HR7954|207966|33067|640^7955|HR7955|207559|575797|451^7956|HR7956|207863|343742|492^7957|η Cep|207548|618389|343^7958|HR7958|207774|465317|630^7959|HR7959|208606|-624292|628^7960|HR7960|208608|-624292|659^7961|HR7961|208216|-257814|586^7962|HR7962|207726|529953|633^7963|λ Cyg|207901|364908|453^7964|HR7964|208224|-180358|621^7965|α Mic|208328|-337797|490^7966|HR7966|207891|455797|640^7967|HR7967|207425|697519|641^7968|ι Ind|208584|-516083|505^7969|HR7969|207970|478319|557^7970|HR7970|208464|-320544|636^7971|HR7971|208502|-379133|552^7972|HR7)BSC5";
static const char BSC5_CHUNK_84[] PROGMEM = R"BSC5(972|207980|524072|627^7973|15 Del|208272|125453|598^7974|14 Del|208301|78642|633^7975|HR7975|208331|55447|621^7976|HR7976|208449|-125450|588^7977|55 Cyg|208156|461142|484^7978|HR7978|208119|519106|629^7979|β Mic|208663|-331772|604^7980|ω Cap|208637|-269192|411^7981|HR7981|208436|180514|652^7982|4 Aqr|208571|-56264|599^7983|HR7983|208319|466611|633^7984|56 Cyg|208347|440594|504^7985|5 Aqr|208691|-55069|555^7986|β Ind|209135|-584542|365^7987|HR7987|208945|-398100|535^7988|HR7988|208578|282506|577^7989|HR7989|208837|-237831|633^7990|μ Aqr|208776|-89833|473^7991|HR7991|208903|-307186|635^7992|HR7992|209097|-507278|624^7993|HR7993|208215|640422|645^7994|HR7994|208849|-115736|638^7995|31 Vul|208688|270969|459^7996|HR7996|208668|328492|644^7997|HR7997|209019|-279256|641^7998|HR7998|208996|-68897|644^7999|HR7999|208854|296494|634^8000|19 Cap|209133|-179231|578^8001|57 Cyg|208874|443872|478^8002|76 Dra|207098|825311|575^8003|HR8003|208885|451819|545^8004|HR8004|208907|424103|666^8005|HR8005|208983|334378|547^8006|HR8006|209189|-13733|655^8007|HR8007|209062|285219|656^8008|32 Vul|209093|280575|501^8009|HR8009|209062|407031|670^8010|HR8010|209280|45328|605^8011|17 Del|209269|137214|517^8012|16 Del|209274|125686|558^8013|HR8013|209465|-262964|570^8014|HR8014|209384|-35614|657^8015|7 Aqr|209483|-96975|551^8016|HR8016|207926|805522|539^8017|HR8017|209529|4636|605^8018|HR8018|209613|-160317|587^8019|HR8019|210245|-682097|637^8020|HR8020|209305|474178|567^8021|α Oct|210786|-770239|515^8022|HR8022|209369|510750|663^8023|HR8023|209430|449250|596^8024|HR8024|209783|-144828|601^8025|HR8025|209404|507286|581^8026|HR8026|209405|491958|590^8027|HR8027|210060|-512653|576^8028|ν Cyg|209529|411672|394^8029|HR8029|209381|568875|623^8030|Musica|209739|108392|548^8031|HR8031|209999|-361297|611^8032|33 Vul|209712|223258|531^8033|20 Cap|209934|-190353|625^8034|ε Equ|209846|42936|523^8035|HR8035|209721|444717|555^8036|HR8036|209752|419403|616^8037|HR8037|209974|168242|666^8038|HR8038|210011|75164|599^8039|γ Mic|210215|-322578|467^8040|HR8040|209750|504622|561^8041|11 Aqr|210094|-47303|621^8042|HR8042|210368|-430019|664^8043|HR8043|209123|759256|605^8044|HR8044|210077|193294|565^8045|HR8045|210292|-268811|605^8046|HR8046|210409|-385306|594^8047|59 Cyg|209971|475211|474^8048|ζ Mic|210494|-386317|530^8049|HR8049|209904|594386|551^8050|HR8050|210528|-277319|625^8051|HR8051|210202|360261|597^8052|HR8052|211466|-762125|658^8053|60 Cyg|210197|461558|537^8054|HR8054|210499|-9247|650^8055|μ Ind|210873|-547272|516^8056|HR8056|210508|15319|625^8057|HR8057|210505|147300|631^8058|12 Aqr|210679|-58233|731^8059|12 Aqr|210680|-58231|589^8060|Armus|210734|-198550|484^8061|HR8061|211562|-731731|568^8062|HR8062|210400|447911|619^8063|HR8063|210513|386575|607^8064|HR8064|210468|458489|648^8065|HR8065|)BSC5";
static const char BSC5_CHUNK_85[] PROGMEM = R"BSC5(210359|566697|583^8066|3 Equ|210763|55028|561^8067|HR8067|210782|29419|642^8068|HR8068|210793|22697|633^8069|η Mic|211071|-413861|553^8070|δ Mic|211003|-301250|568^8071|HR8071|210645|416281|633^8072|HR8072|210572|503519|637^8073|HR8073|211425|-639289|576^8074|HR8074|210620|468619|632^8075|θ Cap|210991|-172328|407^8076|HR8076|211069|-323417|518^8077|4 Equ|210908|59583|594^8078|HR8078|210632|532861|590^8079|ξ Cyg|210822|439278|372^8080|24 Cap|211188|-250058|450^8081|HR8081|211891|-725442|620^8082|HR8082|211065|269244|612^8083|HR8083|211291|-174553|617^8084|HR8084|211084|311847|582^8085|61 Cyg|211152|387458|521^8086|61 Cyg|211154|387433|603^8087|χ Cap|211427|-211936|530^8088|HR8088|211260|156586|634^8089|63 Cyg|211100|476483|455^8090|HR8090|211412|69894|615^8091|27 Cap|211592|-205564|625^8092|ο Pav|212224|-701264|502^8093|ν Aqr|211599|-113717|451^8094|HR8094|211441|302058|559^8095|HR8095|211662|29433|645^8096|HR8096|211797|-93539|627^8097|γ Equ|211724|101317|469^8098|6 Equ|211753|100489|607^8099|HR8099|211065|714319|587^8100|HR8100|212038|-402694|583^8101|HR8101|211756|224547|668^8102|HR8102|211948|-144722|648^8103|HR8103|211663|455025|663^8104|HR8104|212175|-394253|526^8105|HR8105|211844|362992|654^8106|HR8106|211710|535633|573^8107|HR8107|211753|476919|646^8108|HR8108|212219|-364239|596^8109|HR8109|211580|632956|654^8110|HR8110|212215|-276194|542^8111|HR8111|213045|-753467|663^8112|HR8112|210915|781264|591^8113|HR8113|211589|684903|733^8114|HR8114|212628|-532631|575^8115|ζ Cyg|212156|302269|320^8116|HR8116|212247|159825|627^8117|HR8117|212541|-405064|621^8118|HR8118|212380|-106053|677^8119|HR8119|211967|599864|564^8120|HR8120|212240|366336|605^8121|HR8121|212436|922|638^8122|HR8122|212518|-173450|604^8123|δ Equ|212414|100069|449^8124|HR8124|212630|-362108|612^8125|HR8125|213001|-646817|631^8126|HR8126|212362|299011|617^8127|φ Cap|212605|-206517|524^8128|29 Cap|212625|-151714|528^8129|HR8129|215345|-848100|645^8130|τ Cyg|212465|380456|372^8131|Kitalpha|212637|52478|392^8132|HR8132|212777|-16078|648^8133|HR8133|212285|644039|639^8134|HR8134|212871|-132789|640^8135|ε Mic|212990|-321725|471^8136|HR8136|212602|479736|646^8137|30 Cap|212992|-179853|543^8138|HR8138|212749|422514|643^8139|31 Cap|213044|-174622|705^8140|θ Ind|213311|-534497|439^8141|15 Aqr|213031|-45194|582^8142|HR8142|213151|-287656|640^8143|σ Cyg|212903|393947|423^8144|HR8144|212898|426833|619^8145|HR8145|213360|-450222|600^8146|υ Cyg|212986|348969|443^8147|HR8147|212839|539975|613^8148|HR8148|213294|-263531|656^8149|HR8149|213144|112033|596^8150|HR8150|212873|557981|598^8151|θ¹ Mic|213460|-408097|482^8152|HR8152|213546|-499378|638^8153|HR8153|212886|586117|642^8154|68 Cyg|213076|439458|500^8155|HR8155|213154|410408|615^8156|HR8156|214046|-697342|641^8157|HR8157|213228|382375|583^8158|HR8158)BSC5";
static const char BSC5_CHUNK_86[] PROGMEM = R"BSC5(|213372|220264|629^8159|HR8159|214217|-717994|609^8160|16 Aqr|213512|-45600|587^8161|HR8161|213247|495103|576^8162|Alderamin|213097|625856|244^8163|9 Equ|213513|73544|582^8164|HR8164|213211|586236|566^8165|HR8165|213512|238558|557^8166|HR8166|213472|324528|568^8167|ι Cap|213708|-168344|428^8168|HR8168|212617|770122|595^8169|HR8169|213561|326128|604^8170|HR8170|213504|403456|640^8171|6 Cep|213228|648719|518^8172|HR8172|213835|-226689|560^8173|1 Peg|213681|198044|408^8174|HR8174|212226|812308|615^8175|17 Aqr|213823|-93194|599^8176|HR8176|215651|-826831|638^8177|HR8177|214058|-466150|631^8178|β Equ|213816|68111|516^8179|HR8179|213426|607569|611^8180|θ² Mic|214069|-410067|577^8181|γ Pav|214407|-653661|422^8182|HR8182|213783|303097|605^8183|33 Cap|214027|-208519|541^8184|HR8184|214022|-227469|638^8185|HR8185|213668|493889|569^8186|HR8186|213797|386342|663^8187|18 Aqr|214032|-128781|549^8188|γ Ind|214376|-546606|612^8189|HR8189|213897|374067|658^8190|HR8190|213997|242742|571^8191|HR8191|214068|101742|635^8192|20 Aqr|214144|-33983|636^8193|HR8193|213968|373514|647^8194|HR8194|214021|253122|615^8195|19 Aqr|214203|-97486|570^8196|HR8196|214791|-695053|534^8197|HR8197|214064|245289|632^8198|HR8198|214094|261744|568^8199|21 Aqr|214214|-35567|549^8200|HR8200|214397|-378294|563^8201|HR8201|215557|-800392|647^8202|HR8202|214504|-425478|551^8203|HR8203|214310|5344|646^8204|ζ Cap|214444|-224114|374^8205|HR8205|214411|11033|613^8206|HR8206|214154|493233|658^8207|35 Cap|214541|-211961|578^8208|HR8208|214221|467144|560^8209|69 Cyg|214297|366675|594^8210|HR8210|214408|193756|607^8211|HR8211|214834|-537058|639^8212|HR8212|214705|-115683|661^8213|36 Cap|214787|-218072|451^8214|5 PsA|214844|-312386|650^8215|70 Cyg|214559|371167|531^8216|HR8216|214477|488350|531^8217|35 Vul|214611|276086|541^8218|HR8218|214458|528986|603^8219|HR8219|214736|81956|640^8220|HR8220|214690|322253|580^8221|HR8221|214833|179058|644^8222|HR8222|214999|-191478|657^8223|HR8223|214833|221794|593^8224|HR8224|214570|597500|610^8225|2 Peg|214991|236389|457^8226|HR8226|214813|554186|612^8227|7 Cep|214628|668092|544^8228|71 Cyg|214908|465406|524^8229|ξ Gru|215350|-411792|529^8230|6 PsA|215374|-339447|597^8231|HR8231|215193|121375|608^8232|Sadalsuud|215260|-55711|291^8233|HR8233|215549|-527375|641^8234|HR8234|216489|-794425|618^8235|HR8235|215426|-245906|643^8236|HR8236|215565|-448486|557^8237|HR8237|215057|529581|602^8238|Alfirk|214777|705608|323^8239|HR8239|214138|805247|597^8240|HR8240|215409|233944|670^8241|HR8241|215714|-429250|632^8242|HR8242|215243|526200|616^8243|HR8243|215165|604594|553^8244|HR8244|215814|-296961|641^8245|37 Cap|215808|-200844|569^8246|HR8246|215491|499778|575^8247|HR8247|215878|-234542|640^8248|HR8248|215550|458542|625^8249|HR8249|216341|-648242|620^8250|HR8250|215761|227547|647^8)BSC5";
static const char BSC5_CHUNK_87[] PROGMEM = R"BSC5(251|HR8251|215882|-39831|577^8252|ρ Cyg|215664|455919|402^8253|8 PsA|216031|-261714|573^8254|ν Oct|216912|-773900|376^8255|72 Cyg|215796|385342|490^8256|7 PsA|216136|-330481|611^8257|HR8257|215886|281972|631^8258|HR8258|215908|244522|611^8259|HR8259|215743|516983|615^8260|Kastra|216180|-194661|468^8261|HR8261|216039|300556|636^8262|HR8262|216007|453747|553^8263|HR8263|216261|-3903|625^8264|Bunda|216292|-78542|469^8265|3 Peg|216288|66183|618^8266|74 Cyg|216158|404136|501^8267|5 Peg|216293|193186|545^8268|HR8268|216517|-336792|628^8269|HR8269|216666|-523592|621^8270|4 Peg|216422|57717|567^8271|HR8271|216760|-557375|633^8272|HR8272|216244|446967|620^8273|HR8273|216578|-105769|608^8274|HR8274|216458|254989|616^8275|HR8275|216274|540422|615^8276|HR8276|216503|202653|585^8277|25 Aqr|216592|22436|510^8278|Nashira|216682|-166622|368^8279|9 Cep|216320|620819|473^8280|λ Oct|218484|-827192|529^8281|HR8281|216493|574892|562^8282|HR8282|216961|-251019|649^8283|42 Cap|216925|-140475|518^8284|75 Cyg|216698|432739|511^8285|41 Cap|217002|-232628|524^8286|HR8286|217580|-710089|601^8287|26 Aqr|217028|12853|567^8288|κ Cap|217110|-188664|473^8289|7 Peg|217043|56800|530^8290|HR8290|216787|548722|620^8291|76 Cyg|216929|408053|611^8292|HR8292|217092|108247|609^8293|HR8293|217204|-196208|622^8294|HR8294|227579|-888183|657^8295|44 Cap|217179|-143997|588^8297|HR8297|217003|355103|607^8298|HR8298|217023|457658|617^8299|HR8299|217415|-385525|630^8300|77 Cyg|217064|410772|569^8301|Azelfafage|217016|511897|467^8302|45 Cap|217336|-147494|599^8303|HR8303|217553|-494986|645^8304|HR8304|217108|496003|609^8305|ι PsA|217491|-330258|434^8306|HR8306|217184|411550|549^8307|79 Cyg|217238|382839|565^8308|Enif|217364|98750|239^8309|μ¹ Cyg|217357|287428|473^8310|μ² Cyg|217356|287431|608^8311|46 Cap|217501|-90825|509^8312|HR8312|217126|592711|608^8313|9 Peg|217419|173500|434^8314|HR8314|217420|147719|594^8315|κ Peg|217441|256450|413^8316|Garnet Star|217251|587800|408^8317|11 Cep|216987|713114|456^8318|47 Cap|217712|-92758|600^8319|λ Cap|217756|-113658|558^8320|HR8320|217624|358572|640^8321|12 Peg|217679|229489|529^8322|Deneb Algedi|217840|-161272|287^8323|HR8323|218044|-473036|558^8324|HR8324|217178|723203|517^8325|HR8325|217733|255633|628^8326|θ PsA|217956|-308983|501^8327|HR8327|217481|624606|595^8328|11 Peg|217872|26861|564^8329|HR8329|217713|430608|654^8330|HR8330|217846|171942|621^8331|HR8331|218334|-647125|562^8332|HR8332|217939|-59172|617^8333|ο Ind|218464|-696294|553^8334|ν Cep|217575|611208|429^8335|π² Cyg|217799|493094|423^8336|HR8336|218023|365806|647^8337|HR8337|218281|-127231|631^8338|HR8338|218082|386486|612^8339|12 Cep|217904|606928|552^8340|HR8340|218370|-168447|638^8341|HR8341|218241|204625|629^8342|HR8342|217836|701508|629^8343|14 Peg|218307|301742|504^8344|13 Peg|218357|17285)BSC5";
static const char BSC5_CHUNK_88[] PROGMEM = R"BSC5(6|529^8345|HR8345|218278|411489|648^8346|HR8346|218616|-186231|616^8347|HR8347|218219|612728|617^8348|HR8348|218595|198267|577^8349|HR8349|218514|395367|617^8350|HR8350|218717|212731|689^8351|μ Cap|218883|-135517|508^8352|HR8352|219199|-618864|590^8353|Aldhanab|218988|-373650|301^8354|15 Peg|218750|287933|553^8355|HR8355|218933|-103117|659^8356|16 Peg|218844|259250|508^8357|HR8357|218669|557969|571^8358|HR8358|218937|196683|568^8359|HR8359|218994|68644|615^8360|HR8360|219029|-42761|571^8361|HR8361|218604|657528|637^8362|π Ind|219372|-578994|619^8363|HR8363|219100|-33011|620^8364|HR8364|219048|197183|639^8365|HR8365|219321|-306064|641^8366|HR8366|219397|-372536|546^8367|HR8367|219506|-377469|618^8368|δ Ind|219653|-549925|440^8369|κ¹ Ind|219750|-590122|612^8370|HR8370|220311|-776625|641^8371|13 Cep|219148|566114|580^8372|HR8372|219400|212397|640^8373|17 Peg|219490|120764|554^8374|HR8374|219224|615419|613^8375|HR8375|219253|653208|586^8376|HR8376|219704|-54247|633^8377|HR8377|219506|486686|642^8378|HR8378|219788|-211828|612^8379|HR8379|219883|-383953|550^8380|HR8380|220511|-761186|595^8381|HR8381|220067|-558828|601^8382|HR8382|219819|-43731|622^8383|HR8383|219442|636256|491^8384|HR8384|219531|661561|643^8385|18 Peg|220022|67175|600^8386|η PsA|220139|-284536|542^8387|ε Ind|220560|-567861|469^8388|HR8388|219815|626983|593^8389|HR8389|219897|576583|659^8390|28 Aqr|220181|6050|558^8391|HR8391|220074|330061|646^8392|20 Peg|220182|131197|560^8393|19 Peg|220192|82572|565^8394|HR8394|220366|-179036|628^8395|HR8395|219642|749967|635^8396|29 Aqr|220407|-169642|637^8397|HR8397|220337|109739|637^8398|HR8398|220548|-299042|710^8399|HR8399|220109|624881|666^8400|16 Cep|219875|731800|503^8401|30 Aqr|220546|-65225|554^8402|ο Aqr|220552|-21553|469^8403|HR8403|220307|528822|578^8404|21 Peg|220553|113864|580^8405|13 PsA|220733|-299167|647^8406|14 Cep|220346|580006|556^8407|HR8407|220491|446500|560^8408|HR8408|220769|-268225|596^8409|κ² Ind|220975|-596361|562^8410|32 Aqr|220798|-9067|530^8411|λ Gru|221019|-395433|446^8412|HR8412|220762|329419|638^8413|ν Peg|220947|50586|484^8414|Sadalmelik|220964|-3197|296^8415|HR8415|220865|266739|578^8416|18 Cep|220647|631197|529^8417|Kurhah|220632|646278|429^8418|ι Aqr|221073|-138697|427^8419|23 Peg|220930|289639|570^8420|HR8420|221785|-758806|655^8421|HR8421|220879|467447|613^8422|HR8422|220974|451122|644^8423|HR8423|219702|828697|698^8424|HR8424|221006|450144|514^8425|Alnair|221372|-469611|174^8426|20 Cep|220835|627856|527^8427|HR8427|220976|482317|627^8428|19 Cep|220858|622800|511^8429|HR8429|221034|452486|619^8430|ι Peg|221169|253450|376^8431|μ PsA|221397|-329886|450^8432|HR8432|221987|-761161|615^8433|υ PsA|221406|-340439|499^8434|HR8434|221038|563431|639^8435|HR8435|221246|194756|575^8436|HR8436|221250|180006|635^8437|HR8437|22145)BSC5";
static const char BSC5_CHUNK_89[] PROGMEM = R"BSC5(2|-331256|637^8438|25 Peg|221306|217028|578^8439|35 Aqr|221497|-185197|581^8440|HR8440|221661|-481072|643^8441|HR8441|221381|255436|611^8442|HR8442|221193|588408|632^8443|HR8443|221238|533072|614^8444|HR8444|221655|-340147|537^8445|HR8445|221379|497964|642^8446|HR8446|221667|-282925|644^8447|τ PsA|221691|-325483|492^8448|HR8448|221447|457419|611^8449|π¹ Peg|221538|331722|558^8450|Biham|221700|61978|353^8451|HR8451|221725|-38942|627^8452|38 Aqr|221771|-115650|546^8453|HR8453|221761|-42672|601^8454|π² Peg|221664|331783|429^8455|HR8455|221719|196169|618^8456|HR8456|221728|146300|633^8457|HR8457|221840|-212325|609^8458|HR8458|221771|116244|578^8459|28 Peg|221751|209781|646^8460|HR8460|221810|305531|632^8461|HR8461|221976|160406|595^8462|39 Aqr|222072|-141939|603^8463|HR8463|221861|508233|540^8464|HR8464|222160|-263278|617^8465|ζ Cep|221809|582011|335^8466|HR8466|222022|249500|592^8467|HR8467|222122|-47208|639^8468|24 Cep|221634|723411|479^8469|λ Cep|221919|594144|504^8470|HR8470|222290|-251808|558^8471|ψ Oct|222974|-775117|551^8472|HR8472|221969|568394|524^8473|HR8473|221709|721111|637^8474|HR8474|221775|701328|550^8475|HR8475|222133|346047|533^8476|HR8476|221991|590847|630^8477|HR8477|222441|-413817|623^8478|λ PsA|222386|-277669|543^8479|HR8479|222005|607594|535^8480|41 Aqr|222383|-210742|532^8481|ε Oct|223337|-804397|510^8482|HR8482|222274|286083|589^8483|HR8483|222062|632914|579^8484|HR8484|222598|-444519|610^8485|HR8485|222313|397150|449^8486|μ¹ Gru|222602|-413467|479^8487|HR8487|222304|454408|553^8488|μ² Gru|222741|-416275|510^8489|HR8489|222457|429539|571^8490|HR8490|222304|631625|611^8491|HR8491|222666|85494|621^8492|HR8492|222771|-258983|615^8493|HR8493|222147|733072|608^8494|ε Cep|222506|570436|419^8495|HR8495|222760|-15964|615^8496|42 Aqr|222800|-128314|534^8497|HR8497|222833|-231400|617^8498|1 Lac|222662|377489|413^8499|Ancha|222806|-77833|416^8500|HR8500|222813|-90400|579^8501|HR8501|223043|-536278|537^8502|α Tuc|223084|-602597|286^8503|HR8503|222749|278042|637^8504|44 Aqr|222851|-53872|575^8505|υ Oct|225271|-859672|577^8506|HR8506|222740|572203|588^8507|HR8507|223012|-2378|639^8508|45 Aqr|223169|-133050|595^8509|HR8509|223434|-575100|634^8510|HR8510|223156|377694|617^8511|25 Cep|223035|628044|575^8512|ρ Aqr|223366|-78211|537^8513|30 Peg|223410|57894|537^8514|HR8514|223488|81867|617^8515|ν Ind|224102|-722556|529^8516|47 Aqr|223599|-215983|513^8517|HR8517|223500|269353|647^8518|Sadachbia|223609|-13872|384^8519|HR8519|223443|509808|642^8520|31 Peg|223586|122053|501^8521|π¹ Gru|223789|-459478|662^8522|32 Peg|223554|283306|481^8523|2 Lac|223504|465367|457^8524|π² Gru|223856|-459286|562^8525|HR8525|223057|764881|666^8526|HR8526|224308|-750156|604^8527|HR8527|224196|-704317|578^8528|HR8528|223641|420783|641^8529|49 Aqr|223919|-247625|553^8530|HR853)BSC5";
static const char BSC5_CHUNK_90[] PROGMEM = R"BSC5(0|223922|-71944|593^8531|HR8531|224157|-577972|532^8532|33 Peg|223943|208483|604^8533|51 Aqr|224019|-48369|578^8534|50 Aqr|224075|-135294|576^8535|HR8535|223834|572844|616^8536|HR8536|223984|385736|622^8537|HR8537|223834|624200|604^8538|β Lac|223927|522292|443^8539|π Aqr|224213|13775|466^8540|δ Tuc|224556|-649664|448^8541|4 Lac|224086|494764|457^8542|HR8542|224363|-236825|629^8543|HR8543|224280|184444|626^8544|53 Aqr|224428|-167414|657^8545|53 Aqr|224429|-167425|635^8546|HR8546|222196|861081|527^8547|HR8547|224771|-674892|555^8548|34 Peg|224437|43936|575^8549|HR8549|224460|374439|646^8550|HR8550|223948|782433|676^8551|35 Peg|224643|46956|479^8552|ν Gru|224776|-391319|547^8553|HR8553|224574|398097|614^8554|HR8554|224498|564333|657^8555|HR8555|224628|318403|598^8556|δ¹ Gru|224878|-434956|397^8557|HR8557|224336|707708|547^8558|ζ¹ Aqr|224805|-203|459^8559|ζ² Aqr|224806|-200|442^8560|δ² Gru|224960|-437494|411^8561|26 Cep|224515|651322|546^8562|36 Peg|224856|91289|558^8563|HR8563|224961|-271072|595^8564|HR8564|224862|267631|579^8565|HR8565|225004|-129150|640^8566|37 Peg|224994|44317|548^8567|56 Aqr|225048|-145858|637^8568|HR8568|224721|640856|629^8569|HR8569|224955|357256|656^8570|ζ PsA|225149|-260736|643^8571|δ Cep|224862|584153|375^8572|5 Lac|224922|477069|436^8573|σ Aqr|225108|-106781|482^8574|38 Peg|225005|325725|565^8575|HR8575|225018|493561|640^8576|β PsA|225251|-323461|429^8577|HR8577|225907|-787717|615^8578|ρ¹ Cep|224451|787858|583^8579|6 Lac|225081|431233|451^8580|HR8580|225218|-29111|616^8581|HR8581|225218|-65550|614^8582|ν Tuc|225500|-619822|481^8583|58 Aqr|225281|-109056|638^8584|HR8584|225262|295428|635^8585|α Lac|225215|502825|377^8586|39 Peg|225432|202300|642^8587|HR8587|225464|158633|632^8588|HR8588|225407|397797|588^8589|HR8589|225386|540375|635^8590|60 Aqr|225675|-15742|589^8591|Al Kalb al Rai|224980|788242|550^8592|υ Aqr|225782|-207083|520^8593|HR8593|225980|-578836|623^8594|HR8594|225613|566250|571^8595|HR8595|225508|699136|660^8596|HR8596|225935|-239911|597^8597|η Aqr|225893|-1175|402^8598|HR8598|225547|703739|634^8599|HR8599|225378|762264|568^8600|σ¹ Gru|226081|-405828|628^8601|HR8601|226098|-316639|582^8602|σ² Gru|226163|-405911|586^8603|8 Lac|225979|396342|573^8604|HR8604|226022|355772|610^8605|HR8605|226101|116969|640^8606|HR8606|225982|500711|629^8607|HR8607|225977|560700|638^8608|HR8608|226180|125772|630^8609|HR8609|226135|356525|630^8610|Situla|226293|-42281|503^8611|HR8611|226523|-526922|665^8612|HR8612|226395|-78978|623^8613|9 Lac|226229|515453|463^8614|HR8614|226458|-287478|647^8615|31 Cep|225961|736431|508^8616|HR8616|226476|-330814|566^8617|HR8617|226382|451831|640^8618|40 Peg|226479|195222|582^8619|HR8619|226622|-283253|631^8620|HR8620|226802|-574222|597^8621|HR8621|226439|567958|521^8622|10 Lac|226544|390503|488^8623|HR86)BSC5";
static const char BSC5_CHUNK_91[] PROGMEM = R"BSC5(23|226729|-306589|587^8624|41 Peg|226631|196811|621^8625|HR8625|226203|753717|579^8626|HR8626|226595|375928|603^8627|30 Cep|226442|635844|519^8628|ε PsA|226776|-270436|417^8629|HR8629|226800|-35542|631^8630|β Oct|227676|-813817|415^8631|HR8631|226813|145494|571^8632|11 Lac|226753|442764|446^8633|HR8633|226718|538461|593^8634|Homam|226910|108314|340^8635|HR8635|227102|-472106|598^8636|β Gru|227111|-468847|210^8637|19 PsA|227061|-293608|617^8638|HR8638|226921|309658|634^8639|HR8639|227120|-442478|607^8640|12 Lac|226913|402256|525^8641|ο Peg|226959|293075|479^8642|HR8642|226993|145164|590^8643|HR8643|226934|415494|594^8644|ρ Gru|227250|-414144|485^8645|HR8645|227176|-83117|645^8646|HR8646|227379|-604994|630^8647|67 Aqr|227206|-69628|641^8648|HR8648|227058|539089|612^8649|66 Aqr|227265|-188303|469^8650|Matar|227167|302214|294^8651|HR8651|227154|378028|643^8652|HR8652|227179|471686|639^8653|HR8653|227285|109392|651^8654|HR8654|227348|394656|595^8655|η Gru|227605|-535003|485^8656|13 Lac|227349|418192|508^8657|HR8657|227613|-465475|551^8658|HR8658|227689|-489789|662^8659|HR8659|227745|-496858|648^8660|45 Peg|227578|193667|625^8661|HR8661|227470|525172|655^8662|HR8662|227788|-469394|656^8663|ξ Oct|228397|-801242|535^8664|HR8664|228280|-770506|673^8665|ξ Peg|227782|121728|419^8666|HR8666|227695|445461|576^8667|λ Peg|227755|235656|395^8668|HR8668|227886|-341611|628^8669|HR8669|228059|-616842|637^8670|68 Aqr|227925|-196133|526^8671|HR8671|227964|-382219|671^8672|HR8672|228215|-703478|634^8673|τ¹ Aqr|227952|-140564|566^8674|HR8674|227989|-259119|630^8675|ε Gru|228093|-513169|349^8676|70 Aqr|228084|-105556|619^8677|HR8677|227898|584828|636^8678|HR8678|228030|374167|590^8679|τ² Aqr|228265|-135925|401^8680|HR8680|228331|-328053|633^8681|HR8681|228256|104789|654^8682|HR8682|228133|544150|612^8683|HR8683|228123|629383|606^8684|Sadalbari|228334|246017|348^8685|HR8685|228506|-391569|542^8686|HR8686|228625|-598814|646^8687|HR8687|228169|685703|619^8688|HR8688|228295|559028|543^8689|HR8689|228694|-631886|612^8690|14 Lac|228394|419536|592^8691|HR8691|228442|191408|640^8692|HR8692|228362|506769|621^8693|21 PsA|228558|-295361|597^8694|ι Cep|228280|662006|352^8695|γ PsA|228754|-328756|446^8696|HR8696|228563|616969|560^8697|σ Peg|228734|98356|516^8698|λ Aqr|228769|-75797|374^8699|15 Lac|228672|433125|494^8700|τ¹ Gru|228939|-485981|604^8701|ρ Ind|229109|-700736|605^8702|HR8702|227914|831539|474^8703|HR8703|228840|168411|564^8704|74 Aqr|228913|-116167|580^8705|HR8705|228812|504119|646^8706|HR8706|228865|401672|634^8707|HR8707|228844|601011|601^8708|HR8708|228945|447492|581^8709|Skat|229108|-158208|327^8710|78 Aqr|229095|-72047|619^8711|77 Aqr|229126|-162719|556^8712|HR8712|229019|403769|581^8713|HR8713|229208|-363886|640^8714|HR8714|229099|169417|612^8715|1 Psc|229165|10647|611)BSC5";
static const char BSC5_CHUNK_92[] PROGMEM = R"BSC5(^8716|HR8716|229197|-49878|572^8717|ρ Peg|229205|88158|490^8718|HR8718|229174|370769|591^8719|HR8719|229309|-316331|610^8720|δ PsA|229325|-325397|421^8721|HR8721|229400|-315656|648^8722|τ³ Gru|229466|-479692|570^8723|HR8723|229290|363517|574^8724|HR8724|229476|118483|651^8725|16 Lac|229399|416039|559^8726|HR8726|229406|497336|495^8727|HR8727|229548|-48100|631^8728|Fomalhaut|229609|-296222|116^8729|Helvetios|229578|207689|549^8730|HR8730|229591|38103|628^8731|HR8731|229512|486842|543^8732|HR8732|229764|-355231|613^8733|HR8733|229613|393089|618^8734|HR8734|229710|-23953|616^8735|HR8735|229732|-14103|637^8736|HR8736|228506|853736|590^8737|HR8737|229764|93569|643^8738|HR8738|229785|73397|633^8739|52 Peg|229866|117289|575^8740|HR8740|229933|-294622|551^8741|HR8741|229932|-130708|607^8742|2 Psc|229909|9628|543^8743|HR8743|230016|-251642|565^8744|HR8744|229862|526544|629^8745|HR8745|229858|598147|643^8746|HR8746|230068|-256267|629^8747|ζ Gru|230147|-527542|412^8748|HR8748|229069|843461|471^8749|HR8749|230188|-509500|568^8750|3 Psc|230105|1858|621^8751|HR8751|230119|30117|583^8752|HR8752|230014|569453|500^8753|HR8753|230118|310831|660^8754|HR8754|230221|-288536|555^8755|HR8755|230096|453750|650^8756|HR8756|230231|-227908|628^8757|81 Aqr|230232|-70611|621^8758|HR8758|230152|387081|654^8759|HR8759|230255|-47114|594^8760|HR8760|230428|-364208|647^8761|HR8761|230252|571056|620^8762|ο And|230320|423261|362^8763|82 Aqr|230424|-65742|615^8764|HR8764|230456|-208706|597^8765|HR8765|230425|317806|657^8766|2 And|230434|427578|510^8767|π PsA|230583|-347494|511^8768|HR8768|230459|440589|639^8769|HR8769|230812|-688203|552^8770|HR8770|230455|552364|650^8771|HR8771|230665|-414783|579^8772|HR8772|230659|-47953|668^8773|Fum al Samakah|230646|38200|453^8774|κ Gru|230777|-539650|537^8775|Scheat|230629|280828|242^8776|HR8776|230669|66167|641^8777|HR8777|230566|604453|674^8778|HR8778|230560|585647|643^8779|HR8779|230591|672092|524^8780|3 And|230697|500522|465^8781|Markab|230794|152053|249^8782|83 Aqr|230861|-76936|543^8783|HR8783|230869|-170792|614^8784|HR8784|230851|165631|644^8785|HR8785|230882|13069|639^8786|HR8786|231399|-794808|612^8787|θ Gru|231147|-435206|428^8788|HR8788|231051|185175|613^8789|86 Aqr|231114|-237431|447^8790|υ Gru|231149|-388922|561^8791|HR8791|231193|-496067|633^8792|HR8792|231089|199108|630^8793|HR8793|231208|-506864|583^8794|HR8794|231432|-735864|615^8795|55 Peg|231168|94094|452^8796|56 Peg|231186|254683|476^8797|1 Cas|231102|594197|485^8798|HR8798|231244|328258|602^8799|HR8799|231246|211342|599^8800|HR8800|231217|460681|666^8801|HR8801|231195|528164|611^8802|HR8802|231392|-288233|560^8803|HR8803|231196|597275|640^8804|4 And|231276|463872|533^8805|5 And|231293|492958|570^8806|HR8806|231368|445617|656^8807|5 Psc|231447|21278|540^8808|HR8808|231299|636333|)BSC5";
static const char BSC5_CHUNK_93[] PROGMEM = R"BSC5(626^8809|HR8809|231699|-668575|647^8810|HR8810|232033|-809128|641^8811|HR8811|231326|642225|621^8812|88 Aqr|231574|-211725|366^8813|HR8813|231624|-280886|587^8814|HR8814|231659|-428606|581^8815|57 Peg|231587|86772|512^8816|HR8816|231638|-145106|642^8817|89 Aqr|231652|-224575|469^8818|HR8818|231694|-405917|583^8819|π Cep|231316|753875|441^8820|ι Gru|231727|-452467|390^8821|58 Peg|231671|98219|539^8822|2 Cas|231622|593331|570^8823|HR8823|231796|-295250|651^8824|HR8824|231785|175944|571^8825|6 And|231742|435442|594^8826|59 Peg|231956|87200|516^8827|60 Peg|231970|268472|617^8828|HR8828|232208|-496189|680^8829|HR8829|232352|-627000|612^8830|7 And|232092|494064|452^8831|HR8831|232178|294417|635^8832|HR8832|232214|571683|556^8833|HR8833|232240|110650|582^8834|φ Aqr|232387|-60489|422^8835|HR8835|232496|-411056|577^8836|HR8836|232445|-106886|612^8837|HR8837|232373|506178|631^8838|HR8838|232394|297717|641^8839|HR8839|232435|241031|636^8840|HR8840|232595|-34964|555^8841|ψ¹ Aqr|232649|-90878|421^8842|61 Peg|232629|282478|649^8843|HR8843|232827|-620011|566^8844|HR8844|232437|742311|584^8845|HR8845|232661|247711|660^8846|HR8846|232777|-444892|592^8847|HR8847|232805|-411944|647^8848|γ Tuc|232905|-582358|399^8849|HR8849|233189|-794728|633^8850|χ Aqr|232808|-77267|506^8851|HR8851|232605|708881|556^8852|γ Psc|232861|32822|369^8853|HR8853|232784|532136|554^8854|HR8854|232741|619631|653^8855|HR8855|233056|-674711|613^8856|HR8856|232944|-117131|634^8857|HR8857|232879|451642|643^8858|ψ² Aqr|232984|-91825|439^8859|φ Gru|233028|-408244|553^8860|8 And|232958|490153|485^8861|HR8861|232989|454889|648^8862|τ Oct|234677|-874822|549^8863|γ Scl|233137|-325319|441^8864|9 And|233065|417736|602^8865|ψ³ Aqr|233160|-96108|498^8866|94 Aqr|233185|-134589|508^8867|HR8867|232886|752992|638^8868|96 Aqr|233233|-51244|555^8869|HR8869|233234|-180753|593^8870|HR8870|233173|451372|650^8871|HR8871|233287|-337081|637^8872|ο Cep|233104|681117|475^8873|HR8873|233243|347933|632^8874|11 And|233249|486253|544^8875|HR8875|233282|483808|632^8876|10 And|233312|420781|579^8877|HR8877|233472|-503067|605^8878|7 Psc|233391|53814|505^8879|HR8879|233447|-59081|617^8880|Salm|233439|237403|460^8881|HR8881|233373|619700|645^8882|63 Peg|233471|304150|559^8883|HR8883|233543|-269867|564^8884|HR8884|233456|441164|613^8885|12 And|233481|381822|577^8886|HR8886|233429|622131|639^8887|64 Peg|233653|318125|532^8888|HR8888|233662|266089|662^8889|HR8889|233825|-600558|609^8890|97 Aqr|233776|-150392|520^8891|65 Peg|233779|208286|629^8892|98 Aqr|233828|-201006|397^8893|66 Peg|233846|123139|508^8894|HR8894|233757|601336|556^8895|HR8895|233984|-538083|615^8896|HR8896|233959|-431244|610^8897|HR8897|233922|2914|631^8898|HR8898|234036|-518914|575^8899|HR8899|233965|325314|669^8900|HR8900|234022|-186875|619^8901|HR8901|234221|-56849)BSC5";
static const char BSC5_CHUNK_94[] PROGMEM = R"BSC5(2|559^8902|HR8902|234097|411128|672^8903|67 Peg|234141|323850|557^8904|4 Cas|234140|622828|498^8905|υ Peg|234230|234042|440^8906|99 Aqr|234341|-206419|439^8907|ο Gru|234435|-527217|552^8908|HR8908|234520|-665811|645^8909|HR8909|234542|-584761|563^8910|HR8910|234525|-501572|620^8911|κ Psc|234489|12556|494^8912|9 Psc|234541|11225|625^8913|13 And|234521|429119|575^8914|HR8914|234669|-355444|632^8915|69 Peg|234612|251672|598^8916|θ Psc|234661|63789|428^8917|HR8917|234681|-114497|637^8918|HR8918|234546|703597|560^8919|HR8919|234836|-631108|568^8920|HR8920|234836|-444978|643^8921|HR8921|234835|-92661|618^8922|HR8922|234849|230478|635^8923|70 Peg|234859|127606|455^8924|HR8924|234923|-45328|625^8925|HR8925|235021|491331|617^8926|HR8926|235006|585489|491^8927|HR8927|235110|386619|605^8928|HR8928|235170|-62883|639^8929|HR8929|235242|-448436|602^8930|Veritate|235215|392364|522^8931|HR8931|235254|-40872|649^8932|100 Aqr|235284|-213694|629^8933|HR8933|235286|284036|641^8934|13 Psc|235327|-10858|638^8935|HR8935|235554|-773853|581^8936|HR8936|235402|349525|665^8937|β Scl|235495|-378183|437^8938|HR8938|234502|873075|558^8939|101 Aqr|235546|-209144|471^8940|71 Peg|235578|224989|532^8941|HR8941|235619|450581|624^8942|HR8942|235654|208408|606^8943|72 Peg|235659|313253|498^8944|14 Psc|235692|-12475|587^8945|HR8945|235869|-646894|740^8946|HR8946|235804|-152458|596^8947|15 And|235771|402364|559^8948|73 Peg|235773|334972|563^8949|ι Phe|235846|-426150|471^8950|HR8950|235796|380239|618^8951|HR8951|235922|-74644|639^8952|HR8952|235831|716422|584^8953|HR8953|235989|245611|645^8954|16 Psc|236065|21022|568^8955|HR8955|236085|329042|635^8956|HR8956|236182|-318708|652^8957|HR8957|236400|-768700|600^8958|HR8958|236277|-130603|565^8959|HR8959|236308|-454925|474^8960|74 Peg|236277|168256|626^8961|λ And|236261|464581|382^8962|HR8962|236256|444292|580^8963|75 Peg|236324|184006|553^8964|HR8964|236329|461997|658^8965|ι And|236356|432681|429^8966|θ Phe|236578|-466378|609^8967|18 And|236523|504717|530^8968|ω¹ Aqr|236631|-142217|500^8969|ι Psc|236658|56264|413^8970|HR8970|236653|96772|597^8971|HR8971|236529|752928|595^8972|HR8972|236559|740028|598^8973|HR8973|236674|376525|653^8974|Errai|236558|776325|321^8975|μ Scl|236773|-320731|531^8976|κ And|236735|443339|414^8977|HR8977|236779|367208|623^8978|HR8978|236853|-241603|660^8979|HR8979|236858|-116806|589^8980|103 Aqr|236929|-180272|534^8981|HR8981|236908|495122|626^8982|104 Aqr|236961|-178164|482^8983|HR8983|236991|72506|589^8984|λ Psc|237008|17800|450^8985|HR8985|236985|572600|624^8986|HR8986|237041|449919|657^8987|HR8987|237078|-154478|528^8988|ω² Aqr|237120|-145450|449^8989|HR8989|237058|645156|656^8990|HR8990|237087|616794|640^8991|77 Peg|237229|103314|506^8992|HR8992|237304|-152844|636^8993|HR8993|237337|-450833|609^8994|HR8994|237404|-)BSC5";
static const char BSC5_CHUNK_95[] PROGMEM = R"BSC5(704903|607^8995|HR8995|237446|-787914|575^8996|HR8996|237367|-644044|572^8997|78 Peg|237332|293617|493^8998|106 Aqr|237367|-182769|524^8999|HR8999|237414|-262464|617^9000|HR9000|237468|557997|651^9001|HR9001|237670|-401825|631^9002|107 Aqr|237669|-186781|529^9003|ψ And|237672|464203|495^9004|19 Psc|237732|34867|504^9005|HR9005|237769|667822|595^9006|σ Phe|237878|-502267|518^9007|HR9007|237898|-683942|689^9008|τ Cas|237843|586519|487^9009|HR9009|237878|-119108|573^9010|HR9010|237839|574514|551^9011|HR9011|237926|468325|607^9012|20 Psc|237990|-27617|549^9013|HR9013|237986|678069|504^9014|HR9014|238090|-63806|607^9015|HR9015|238137|22142|646^9016|δ Scl|238154|-281303|457^9017|HR9017|238108|648764|641^9018|6 Cas|238139|622144|543^9019|HR9019|238150|599789|634^9020|HR9020|238200|589631|633^9021|HR9021|238254|-158611|624^9022|21 Psc|238243|10761|577^9023|HR9023|238291|-628394|659^9024|HR9024|238281|364253|590^9025|79 Peg|238276|288425|597^9026|HR9026|238304|-253314|642^9027|HR9027|238374|-99742|594^9028|HR9028|238395|516217|644^9029|HR9029|238426|-144019|572^9030|80 Peg|238559|93133|579^9031|108 Aqr|238559|-189089|518^9032|γ¹ Oct|238684|-820189|511^9033|22 Psc|238661|29303|555^9034|HR9034|238660|775994|655^9035|HR9035|238732|216708|611^9036|φ Peg|238748|191203|508^9037|HR9037|238750|-142511|587^9038|HR9038|238736|755447|639^9039|82 Peg|238770|109475|530^9040|HR9040|238807|-89967|575^9041|24 Psc|238821|-31556|593^9042|25 Psc|238847|20906|628^9043|HR9043|238891|-242292|624^9044|HR9044|239059|-270422|635^9045|ρ Cas|239064|574994|454^9046|HR9046|239107|-403000|603^9047|HR9047|239129|1092|561^9048|26 Psc|239188|70711|621^9049|HR9049|239213|-319217|610^9050|HR9050|239213|-318842|683^9051|HR9051|239231|259550|654^9052|HR9052|239260|574122|600^9053|HR9053|239260|473558|600^9054|HR9054|239416|-247372|631^9055|HR9055|239449|226481|615^9056|HR9056|239410|831911|659^9057|HR9057|239510|426583|597^9058|HR9058|239523|-266236|626^9059|HR9059|239524|557058|555^9060|HR9060|239555|-629564|597^9061|γ² Oct|239591|-821700|573^9062|η Tuc|239598|-642983|500^9063|HR9063|239593|600236|647^9064|ψ Peg|239626|251414|466^9065|1 Cet|239726|-158475|626^9066|HR9066|239736|513886|480^9067|27 Psc|239779|-35561|486^9068|HR9068|239803|323817|652^9069|π Phe|239822|-527458|513^9070|HR9070|239796|464131|654^9071|σ Cas|239835|557550|488^9072|ω Psc|239885|68633|401^9073|HR9073|239911|-294850|562^9074|HR9074|239914|337244|658^9075|HR9075|239914|337244|658^9076|ε Tuc|239986|-655772|450^9077|HR9077|53|-442906|629^9078|HR9078|66|269183|646^9079|HR9079|86|595597|619^9080|HR9080|121|452533|638^9081|τ Phe|179|-488100|571^9082|HR9082|222|-503372|553^9083|HR9083|220|499817|622^9084|θ Oct|266|-770658|478^9085|HR9085|269|612231|555^9086|HR9086|288|423672|625^9087|29 Psc|304|-30275|510^9088|85 Peg|362|270819|)BSC5";
static const char BSC5_CHUNK_96[] PROGMEM = R"BSC5(575^9089|30 Psc|327|-60142|441^9090|HR9090|354|-146761|710^9091|ζ Scl|389|-297203|501^9092|31 Psc|401|89569|632^9093|32 Psc|416|84856|563^9094|HR9094|434|660989|586^9095|HR9095|493|-200461|625^9096|HR9096|521|-241453|644^9097|HR9097|571|636419|624^9098|2 Cet|623|-173361|455^9099|HR9099|644|667122|629^9100|9 Cas|704|622878|588^9101|HR9101|721|-165289|578^9102|HR9102|723|-292686|640^9103|3 Cet|750|-105094|494^9104|HR9104|783|671667|567^9105|HR9105|769|420922|601^9106|HR9106|752|-728978|731^9107|HR9107|816|346597|612^9108|HR9108|781|-714369|559^9109|HR9109|822|266489|625^9110|HR9110|851|613142|580)BSC5";
static const char* const BSC5_CHUNKS[] PROGMEM = {BSC5_CHUNK_0,BSC5_CHUNK_1,BSC5_CHUNK_2,BSC5_CHUNK_3,BSC5_CHUNK_4,BSC5_CHUNK_5,BSC5_CHUNK_6,BSC5_CHUNK_7,BSC5_CHUNK_8,BSC5_CHUNK_9,BSC5_CHUNK_10,BSC5_CHUNK_11,BSC5_CHUNK_12,BSC5_CHUNK_13,BSC5_CHUNK_14,BSC5_CHUNK_15,BSC5_CHUNK_16,BSC5_CHUNK_17,BSC5_CHUNK_18,BSC5_CHUNK_19,BSC5_CHUNK_20,BSC5_CHUNK_21,BSC5_CHUNK_22,BSC5_CHUNK_23,BSC5_CHUNK_24,BSC5_CHUNK_25,BSC5_CHUNK_26,BSC5_CHUNK_27,BSC5_CHUNK_28,BSC5_CHUNK_29,BSC5_CHUNK_30,BSC5_CHUNK_31,BSC5_CHUNK_32,BSC5_CHUNK_33,BSC5_CHUNK_34,BSC5_CHUNK_35,BSC5_CHUNK_36,BSC5_CHUNK_37,BSC5_CHUNK_38,BSC5_CHUNK_39,BSC5_CHUNK_40,BSC5_CHUNK_41,BSC5_CHUNK_42,BSC5_CHUNK_43,BSC5_CHUNK_44,BSC5_CHUNK_45,BSC5_CHUNK_46,BSC5_CHUNK_47,BSC5_CHUNK_48,BSC5_CHUNK_49,BSC5_CHUNK_50,BSC5_CHUNK_51,BSC5_CHUNK_52,BSC5_CHUNK_53,BSC5_CHUNK_54,BSC5_CHUNK_55,BSC5_CHUNK_56,BSC5_CHUNK_57,BSC5_CHUNK_58,BSC5_CHUNK_59,BSC5_CHUNK_60,BSC5_CHUNK_61,BSC5_CHUNK_62,BSC5_CHUNK_63,BSC5_CHUNK_64,BSC5_CHUNK_65,BSC5_CHUNK_66,BSC5_CHUNK_67,BSC5_CHUNK_68,BSC5_CHUNK_69,BSC5_CHUNK_70,BSC5_CHUNK_71,BSC5_CHUNK_72,BSC5_CHUNK_73,BSC5_CHUNK_74,BSC5_CHUNK_75,BSC5_CHUNK_76,BSC5_CHUNK_77,BSC5_CHUNK_78,BSC5_CHUNK_79,BSC5_CHUNK_80,BSC5_CHUNK_81,BSC5_CHUNK_82,BSC5_CHUNK_83,BSC5_CHUNK_84,BSC5_CHUNK_85,BSC5_CHUNK_86,BSC5_CHUNK_87,BSC5_CHUNK_88,BSC5_CHUNK_89,BSC5_CHUNK_90,BSC5_CHUNK_91,BSC5_CHUNK_92,BSC5_CHUNK_93,BSC5_CHUNK_94,BSC5_CHUNK_95,BSC5_CHUNK_96};

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
