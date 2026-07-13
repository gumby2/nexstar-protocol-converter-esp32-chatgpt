#pragma once

#include <Arduino.h>
#include <stdint.h>

extern const char* AP_SSID;
extern const char* AP_PASS;
extern const char* WIFI_FILE;

extern const bool ESP32_FORCE_AP_DEFAULTS;
extern const bool ESP32_DISABLE_LITTLEFS_SETTINGS;
extern const bool ESP32_BOOT_AP_ONLY;
extern const bool ESP32_BOOT_DISABLE_BACKGROUND_POLLING;
extern const bool ESP32_BOOT_WEB_ONLY;

extern const uint8_t BRIDGE_MODE_WIFI_FULL;
extern const uint8_t BRIDGE_MODE_BT_MIN_WEB;
extern uint8_t bridgeMode;

extern uint16_t ALPACA_PORT;
extern uint16_t LX200_PORT;
extern uint16_t STELLARIUM_PORT;
extern uint16_t TELNET_PORT;

extern String staSsid;
extern String staPass;
extern String apSsid;
extern String apPass;
extern String apIp;
extern String lastStaIp;
extern bool staRuntimeDisabled;
extern bool staConfigured;
extern bool btLiteBootWebEnabled;
extern String telnetPassword;

extern bool ntpEnabled;
extern char ntpServer1[64];
extern char ntpServer2[64];
extern char tzRule[96];

extern double nudgeRateDeg[4];
extern double lx200CurrentNudgeStepDeg;
extern int webSelectedRateIndex;

extern double safeDecMinDeg;
extern double safeDecMaxDeg;
extern bool safeDecLimitEnabled;
extern double safeAltMinDeg;
extern double safeAltMaxDeg;
extern bool safeAltLimitEnabled;

extern unsigned long pollIntervalMs;
extern unsigned long idlePollIntervalMs;
extern unsigned long minClientPollIntervalMs;
extern unsigned long gotoQueueTimeoutMs;
extern unsigned long lastPersistentSaveMs;
extern uint32_t persistentSaveCount;

extern double siteLatitudeDeg;
extern double siteLongitudeDeg;
extern double siteElevationMeters;
extern int utcOffsetMinutes;
extern String utcOffsetSource;
extern bool siteValid;

extern const uint32_t SETTINGS_MAGIC;
extern const uint16_t SETTINGS_VERSION;
extern const char* SETTINGS_FILE;

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
bool saveSettingsNVS();
bool loadSettingsNVS();
bool clearSettingsNVS();
#endif

uint32_t checksumSettings(const void *settingsPtr);
bool savePersistentSettings();
bool loadPersistentSettings();
void clearPersistentSettings();

bool loadWiFiConfig();
bool saveWiFiConfig(const String &ssid, const String &pass);
void saveLastStaIp(const String &ip);
void clearWiFiConfig();
bool saveBluetoothLiteStaModeFlag(bool disableSta);
bool saveBluetoothLiteApOnlySettings();

bool loadBridgeMode();
bool saveBridgeMode(uint8_t mode);
bool saveBluetoothLiteWebBootEnabled(bool enabled);
bool saveBluetoothLitePollInterval(unsigned long ms);
bool loadBluetoothLitePollInterval();
bool saveGotoQueueTimeoutMs(unsigned long v);
