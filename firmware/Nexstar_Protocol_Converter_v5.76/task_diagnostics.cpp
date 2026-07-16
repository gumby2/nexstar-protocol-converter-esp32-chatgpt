#include "task_diagnostics.h"

#include "bluetooth_services.h"
#include "logging.h"
#include "lx200_protocol.h"
#include "mount_transport.h"
#include "network_services.h"
#include "nexstar_protocol.h"
#include "observer_time.h"
#include "position_cache.h"
#include "settings.h"
#include "slew_controller.h"
#include "time_services.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
  #include "esp_system.h"
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
#endif

extern const char* FW_VERSION;
extern const char* FW_NAME;
extern uint32_t positionApiCacheReplies;
extern uint32_t positionApiCacheMisses;
extern bool alpacaConnected;
extern uint32_t alpacaHttpRequests;
extern unsigned long webCpuLastIdle0;
extern unsigned long webCpuLastIdle1;
extern unsigned long webCpuLastRuntimeTotal;
extern int webCpuLoadPct;
extern unsigned long lastLoopTimeMs;
extern unsigned long maxLoopTimeMs;
extern unsigned long lastLoopLatencyMs;
extern unsigned long maxLoopLatencyMs;
extern String lastWifiStatus;
extern bool staConfigured;
extern String staSsid;
extern String apSsid;
extern bool ntpSyncValid;
extern unsigned long lastNtpSyncMs;
extern unsigned long lastPersistentSaveMs;

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

String sampleBannerSystemText() {
  // Refresh the interval CPU load and then collect the current FreeRTOS
  // percentages for the three tasks most useful in the compact banner.
  String cpuText = sampleWebCpuLoadText();
  cpuText.replace("CPU Load ", "");

  char loopPct[8] = "--%";
  char idle0Pct[8] = "--%";
  char idle1Pct[8] = "--%";
#if defined(ESP32) && defined(configGENERATE_RUN_TIME_STATS) && (configGENERATE_RUN_TIME_STATS == 1) && defined(configUSE_STATS_FORMATTING_FUNCTIONS) && (configUSE_STATS_FORMATTING_FUNCTIONS == 1)
  static char taskStats[2048];
  memset(taskStats, 0, sizeof(taskStats));
  vTaskGetRunTimeStats(taskStats);
  char *line = taskStats;
  while (line && *line) {
    char *next = line;
    while (*next && *next != '\n' && *next != '\r') next++;
    char saved = *next;
    *next = '\0';

    char taskName[24] = "";
    char percentTime[16] = "";
    unsigned long absTime = 0;
    if (parseTaskStatsLine(line, taskName, sizeof(taskName), absTime,
                           percentTime, sizeof(percentTime))) {
      char *dest = nullptr;
      if (strcmp(taskName, "loopTask") == 0) dest = loopPct;
      else if (strcmp(taskName, "IDLE0") == 0) dest = idle0Pct;
      else if (strcmp(taskName, "IDLE1") == 0) dest = idle1Pct;
      if (dest) {
        strncpy(dest, percentTime, 7);
        dest[7] = '\0';
      }
    }

    if (!saved) break;
    line = next + 1;
    while (*line == '\n' || *line == '\r') line++;
  }
#endif

  char row[72];
  // Compact labels keep all four values visible in a standard 79-column
  // Windows Telnet session. Values are percentages.
  // Keep related scheduler values together and use stable short labels.
  // Example: CPU:12% Loop:1% I0:48% I1:49%
  snprintf(row, sizeof(row), "CPU:%s Loop:%s I0:%s I1:%s",
           cpuText.c_str(), loopPct, idle0Pct, idle1Pct);
  return String(row);
}

String taskCpuLoadText() {
  if (telnetTasksCpuLoadPct < 0) return "CPU Load --%";
  return String("CPU Load ") + String(telnetTasksCpuLoadPct) + "%";
}

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

String monitorRefreshText() {
  if (telnetMonitorRefreshMs % 1000UL == 0) return String(telnetMonitorRefreshMs / 1000UL) + "s";
  return String(telnetMonitorRefreshMs) + "ms";
}

void telnetDrawMonitor(Print &out) {
  bool btConnected = false;
#if HAS_CLASSIC_BT
  btConnected = bluetoothClientConnected();
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
             btConnected ? "connected" : (bluetoothRuntimeIsEnabled() ? "waiting" : "disabled"),
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
