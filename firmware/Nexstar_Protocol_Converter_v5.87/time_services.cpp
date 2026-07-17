#include "time_services.h"

#include "logging.h"
#include "network_services.h"
#include "observer_time.h"
#include "position_cache.h"
#include "settings.h"
#include <math.h>
#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266HTTPClient.h>
#else
  #include <WiFi.h>
  #include <HTTPClient.h>
#endif

double approxIpLatitudeDeg = 0.0;
double approxIpLongitudeDeg = 0.0;
String approxIpLocationText = "";
String approxIpLocationStatus = "Not fetched";
bool approxIpLocationValid = false;

extern String urlDecodeSimple(String s);

String currentUtcIsoString() {
  int y, mo, d, h, mi, se;
  currentUtcParts(y, mo, d, h, mi, se);
  char buf[28];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d", y, mo, d, h, mi, se);
  return String(buf);
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

String formatHmsFromHours(double hours, int decimals) {
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

bool syncTimeFromNTP(bool forceLog) {
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

bool browserTimeLocationEstablished() {
  return timeValid && siteValid;
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
  double latDeg = siteLatitudeDeg;
  double lonDeg = siteLongitudeDeg;
  bool hasElevation = false;
  double elevationMeters = siteElevationMeters;
  bool hasOffset = false;
  int offsetMinutes = utcOffsetMinutes;
  bool hasYear = false;
  int year = localYear;
  bool hasMonth = false;
  int month = localMonth;
  bool hasDay = false;
  int day = localDay;
  bool hasHour = false;
  int hour = localHour;
  bool hasMinute = false;
  int minute = localMinute;
  bool hasSecond = false;
  int second = localSecond;

  String v;
  v = httpParamValue(url, "lat");
  if (v.length()) {
    latDeg = v.toFloat();
    gotLocation = true;
  }
  v = httpParamValue(url, "lon");
  if (v.length()) {
    lonDeg = v.toFloat();
    gotLocation = true;
  }

  v = httpParamValue(url, "elev");
  if (v.length()) {
    hasElevation = true;
    elevationMeters = v.toFloat();
  }

  v = httpParamValue(url, "offset");
  if (v.length()) {
    hasOffset = true;
    offsetMinutes = v.toInt();
  }

  v = httpParamValue(url, "year");
  if (v.length()) {
    hasYear = true;
    year = v.toInt();
  }
  v = httpParamValue(url, "month");
  if (v.length()) {
    hasMonth = true;
    month = v.toInt();
  }
  v = httpParamValue(url, "day");
  if (v.length()) {
    hasDay = true;
    day = v.toInt();
  }
  v = httpParamValue(url, "hour");
  if (v.length()) {
    hasHour = true;
    hour = v.toInt();
  }
  v = httpParamValue(url, "minute");
  if (v.length()) {
    hasMinute = true;
    minute = v.toInt();
  }
  v = httpParamValue(url, "second");
  if (v.length()) {
    hasSecond = true;
    second = v.toInt();
  }

  applyHttpsBrowserTimeLocationValues(
    gotLocation, latDeg, lonDeg,
    hasElevation, elevationMeters,
    hasOffset, offsetMinutes,
    hasYear, year,
    hasMonth, month,
    hasDay, day,
    hasHour, hour,
    hasMinute, minute,
    hasSecond, second
  );
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
#endif
