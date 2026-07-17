#pragma once

#include <Arduino.h>
#include <stdint.h>

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

constexpr uint16_t LOG_CAT_ALL =
    LOG_CAT_MOUNT |
    LOG_CAT_SKYSAFARI |
    LOG_CAT_ALPACA |
    LOG_CAT_STELLARIUM |
    LOG_CAT_WEB |
    LOG_CAT_WIFI |
    LOG_CAT_TIMELOC |
    LOG_CAT_SETTINGS |
    LOG_CAT_SYSTEM |
    LOG_CAT_BLUETOOTH;

extern int LOG_LEVEL;
extern uint16_t LOG_SUBSYSTEM_MASK;
extern bool logAlertActive;
extern String logAlertText;
extern unsigned long logAlertMs;

extern const int LOG_BUFFER_LINES;
extern String logBuffer[];
extern int logWriteIndex;
extern bool logWrapped;

const char* logLevelName(int level);
const char* logCategoryName(uint16_t category);
String shortLogTimestamp();
bool logLineAlreadyTimestamped(const String &line);
void addLogLine(const String &line);
void logPrintfCat(int level, uint16_t category, const char* format, ...);
void logPrintf(int level, const char* tag, const char* format, ...);

#define LOGE(fmt, ...) logPrintf(LOG_ERROR, "ERR", fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) logPrintf(LOG_WARN,  "WRN", fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) logPrintf(LOG_INFO,  "INF", fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) logPrintf(LOG_DEBUG, "DBG", fmt, ##__VA_ARGS__)
#define LOGT(fmt, ...) logPrintf(LOG_TRACE, "TRC", fmt, ##__VA_ARGS__)

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

#define LOG_SYS_E(fmt, ...) logPrintfCat(LOG_ERROR, LOG_CAT_SYSTEM, fmt, ##__VA_ARGS__)
#define LOG_SYS_W(fmt, ...) logPrintfCat(LOG_WARN,  LOG_CAT_SYSTEM, fmt, ##__VA_ARGS__)
#define LOG_SYS_I(fmt, ...) logPrintfCat(LOG_INFO,  LOG_CAT_SYSTEM, fmt, ##__VA_ARGS__)
#define LOG_SYS_D(fmt, ...) logPrintfCat(LOG_DEBUG, LOG_CAT_SYSTEM, fmt, ##__VA_ARGS__)
#define LOG_SYS_T(fmt, ...) logPrintfCat(LOG_TRACE, LOG_CAT_SYSTEM, fmt, ##__VA_ARGS__)

#define LOG_BT_E(fmt, ...) logPrintfCat(LOG_ERROR, LOG_CAT_BLUETOOTH, fmt, ##__VA_ARGS__)
#define LOG_BT_W(fmt, ...) logPrintfCat(LOG_WARN,  LOG_CAT_BLUETOOTH, fmt, ##__VA_ARGS__)
#define LOG_BT_I(fmt, ...) logPrintfCat(LOG_INFO,  LOG_CAT_BLUETOOTH, fmt, ##__VA_ARGS__)
#define LOG_BT_D(fmt, ...) logPrintfCat(LOG_DEBUG, LOG_CAT_BLUETOOTH, fmt, ##__VA_ARGS__)
#define LOG_BT_T(fmt, ...) logPrintfCat(LOG_TRACE, LOG_CAT_BLUETOOTH, fmt, ##__VA_ARGS__)
