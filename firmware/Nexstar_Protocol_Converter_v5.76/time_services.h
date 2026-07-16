#pragma once

#include <Arduino.h>

extern double approxIpLatitudeDeg;
extern double approxIpLongitudeDeg;
extern String approxIpLocationText;
extern String approxIpLocationStatus;
extern bool approxIpLocationValid;

String currentUtcIsoString();
String twoDigits(int v);
String formatDurationHoursMinutes(unsigned long spanMs);
String formatLastSuccessfulMountPollTime();
String formatSuccessfulMountPollSpan();
String formatMountUptime();
String formatHmsFromHours(double hours, int decimals = 1);
String observerTimeText();

bool syncTimeFromNTP(bool forceLog = true);
void serviceNtpSync();

bool extractJsonNumber(const String &body, const String &key, double &out);
String extractJsonString(const String &body, const String &key);
bool fetchApproxLocationFromInternet();

bool browserTimeLocationEstablished();
String currentTimezoneAbbrev();

#if defined(ESP32)
String httpParamValue(const String &url, const String &name);
void applyHttpsBrowserTimeLocationFromUrl(const String &url, bool &gotLocation);
#endif
