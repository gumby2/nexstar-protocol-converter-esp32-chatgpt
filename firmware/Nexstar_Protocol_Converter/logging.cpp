#include "logging.h"
#if defined(ESP32)
  #include <WiFi.h>
#endif

#if defined(ESP32) || defined(ESP8266)
extern const bool SERIAL_MIRROR_FULL_WIFI_LOGS;
#endif

#if defined(ESP32)
extern WiFiClient telnetClient;
extern bool telnetAuthenticated;
extern bool telnetLiveLogEnabled;
extern uint32_t telnetLiveLogLines;
#endif

int LOG_LEVEL = LOG_WARN;
uint16_t LOG_SUBSYSTEM_MASK = LOG_CAT_ALL;

bool logAlertActive = false;
String logAlertText = "";
unsigned long logAlertMs = 0;

const int LOG_BUFFER_LINES = 100;
String logBuffer[LOG_BUFFER_LINES];
int logWriteIndex = 0;
bool logWrapped = false;

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
