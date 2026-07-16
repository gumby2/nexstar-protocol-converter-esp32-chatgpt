#include "settings.h"
#include "bluetooth_services.h"
#include "logging.h"
#include "position_cache.h"
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
#if defined(ESP32)
  #include <Preferences.h>
#endif

const char* AP_SSID = "NexStar_Bridge";
const char* AP_PASS = "12345678";
const char* WIFI_FILE = "/wifi.cfg";

const uint8_t BRIDGE_MODE_WIFI_FULL = 0;
const uint8_t BRIDGE_MODE_BT_MIN_WEB = 1;
uint8_t bridgeMode = BRIDGE_MODE_WIFI_FULL;

uint16_t ALPACA_PORT = 11111;
uint16_t LX200_PORT = 4030;
uint16_t STELLARIUM_PORT = 10001;
uint16_t TELNET_PORT = 23;

String staSsid = "";
String staPass = "";
String apSsid = "NexStar_Bridge";
String apPass = "12345678";
String apIp = "192.168.4.1";
String lastStaIp = "";
bool staRuntimeDisabled = false;
bool staConfigured = false;
bool btLiteBootWebEnabled = false;
String telnetPassword = "nexstar";

bool ntpEnabled = true;
char ntpServer1[64] = "pool.ntp.org";
char ntpServer2[64] = "time.nist.gov";
char tzRule[96] = "MST7MDT,M3.2.0/2,M11.1.0/2";

double nudgeRateDeg[4] = {0.10, 0.25, 0.50, 1.00};
double lx200CurrentNudgeStepDeg = 0.25;
int webSelectedRateIndex = 1;

double safeDecMinDeg = -90.0;
double safeDecMaxDeg = 90.0;
bool safeDecLimitEnabled = false;
double safeAltMinDeg = 0.0;
double safeAltMaxDeg = 90.0;
bool safeAltLimitEnabled = false;

unsigned long pollIntervalMs = 2000;
unsigned long idlePollIntervalMs = 8000;
unsigned long minClientPollIntervalMs = 1000;
unsigned long gotoQueueTimeoutMs = 10000UL;
unsigned long lastPersistentSaveMs = 0;
uint32_t persistentSaveCount = 0;

extern bool staConnected;
extern unsigned long nextMountPollDueMs;
extern unsigned long sanitizeGotoQueueTimeoutMs(unsigned long v);

const uint32_t SETTINGS_MAGIC = 0x4E504333; // "NPC3"
const uint16_t SETTINGS_VERSION = 5;
const char* SETTINGS_FILE = "/settings.bin";

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
  setBluetoothTinyWebRuntimeEnabled(btLiteBootWebEnabled);

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
  setBluetoothTinyWebRuntimeEnabled(enabled);
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
  resetMountPollScheduler();
  LOG_SET_I("BT mount poll interval loaded separately: %lu ms", pollIntervalMs);
  return true;
#else
  return false;
#endif
}

bool saveGotoQueueTimeoutMs(unsigned long v) {
  gotoQueueTimeoutMs = sanitizeGotoQueueTimeoutMs(v);
  return savePersistentSettings();
}
