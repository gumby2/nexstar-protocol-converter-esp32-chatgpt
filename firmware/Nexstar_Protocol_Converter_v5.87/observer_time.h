#pragma once

#include <Arduino.h>

enum TimeSource {
  TIME_SRC_NONE = 0,
  TIME_SRC_MANUAL = 1,
  TIME_SRC_SKYSAFARI = 2,
  TIME_SRC_WEB = 3,
  TIME_SRC_NTP = 4
};

extern double siteLatitudeDeg;
extern double siteLongitudeDeg;
extern double siteElevationMeters;
extern int utcOffsetMinutes;
extern String utcOffsetSource;
extern bool siteValid;

extern bool timeValid;
extern int localYear;
extern int localMonth;
extern int localDay;
extern int localHour;
extern int localMinute;
extern int localSecond;
extern unsigned long timeSetMillis;
extern unsigned long lastNtpSyncMs;
extern bool ntpSyncValid;
extern TimeSource currentTimeSource;
extern String currentLocationSource;

double normalizeRA(double ra);
double normalizeAz(double az);
double clampAlt(double alt);
double degToRad(double d);
double radToDeg(double r);

long daysFromCivil(int y, unsigned m, unsigned d);
void civilFromDays(long z, int &y, int &mo, int &d);
int estimateUtcOffsetMinutesFromLongitude(double lonDeg);
void setLocalFromUtcWithOffset(int uy, int umo, int ud, int uh, int umi, int usec, int offsetMinutes);
void currentUtcParts(int &y, int &mo, int &d, int &h, int &mi, int &se);
void currentLocalParts(int &y, int &mo, int &d, int &h, int &mi, int &se);

double julianDateUtc();
double gmstDegrees();
double localSiderealDegrees();
double hourAngleDegrees(double raDeg);

bool raDecToAltAz(double raDeg, double decDeg, double &altDeg, double &azDeg);
bool altAzToRaDec(double altDeg, double azDeg, double &raDeg, double &decDeg);
bool targetAltitudeFromRaDec(double raDeg, double decDeg, double &altDeg);
bool targetRaDecFromAltAz(double altDeg, double azDeg, double &raDeg, double &decDeg);

const char* timeSourceName(TimeSource src);
void markTimeSource(TimeSource src);
void markSkySafariTimeSource();
void markLocationSource(const String &src);

void applyManualSiteValue(double latDeg, double lonDeg);
void applySiteElevationValue(double elevationMeters);
void applyUtcOffsetValue(int offsetMinutes, const String &source);
void applyLocalDateTimeValue(int year, int month, int day, int hour, int minute, int second, TimeSource source);
void applyHttpsBrowserTimeLocationValues(bool gotLocation,
                                         double latDeg,
                                         double lonDeg,
                                         bool hasElevation,
                                         double elevationMeters,
                                         bool hasOffset,
                                         int offsetMinutes,
                                         bool hasYear,
                                         int year,
                                         bool hasMonth,
                                         int month,
                                         bool hasDay,
                                         int day,
                                         bool hasHour,
                                         int hour,
                                         bool hasMinute,
                                         int minute,
                                         bool hasSecond,
                                         int second);
