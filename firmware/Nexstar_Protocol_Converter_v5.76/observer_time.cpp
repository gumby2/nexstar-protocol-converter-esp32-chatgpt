#include "observer_time.h"

#include "logging.h"
#include "position_cache.h"
#include "settings.h"

#include <math.h>

double siteLatitudeDeg = 0.0;
double siteLongitudeDeg = 0.0;
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
unsigned long lastNtpSyncMs = 0;
bool ntpSyncValid = false;
TimeSource currentTimeSource = TIME_SRC_NONE;
String currentLocationSource = "None";

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

double degToRad(double d) { return d * PI / 180.0; }
double radToDeg(double r) { return r * 180.0 / PI; }

long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int)(doe) - 719468;
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

int estimateUtcOffsetMinutesFromLongitude(double lonDeg) {
  // Alpaca sends UTCDate, but it does not normally send a time-zone offset.
  // For display and local-time math, estimate the standard-zone offset from longitude.
  // Example: Colorado longitude around -104.8 gives -420 minutes.
  int offset = (int)round(lonDeg / 15.0) * 60;
  if (offset < -720) offset = -720;
  if (offset > 840) offset = 840;
  return offset;
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

  civilFromDays(utcDays, y, mo, d);
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

  civilFromDays(outDays, y, mo, d);
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

double localSiderealDegrees() {
  return normalizeRA(gmstDegrees() + siteLongitudeDeg);
}

double hourAngleDegrees(double raDeg) {
  double haDeg = normalizeRA(localSiderealDegrees() - normalizeRA(raDeg));
  if (haDeg > 180.0) haDeg -= 360.0;
  return haDeg;
}

bool raDecToAltAz(double raDeg, double decDeg, double &altDeg, double &azDeg) {
  if (!siteValid || !timeValid) return false;

  double ha = degToRad(hourAngleDegrees(raDeg));
  double dec = degToRad(decDeg);
  double lat = degToRad(siteLatitudeDeg);

  double sinAlt = sin(dec) * sin(lat) + cos(dec) * cos(lat) * cos(ha);
  sinAlt = constrain(sinAlt, -1.0, 1.0);
  double alt = asin(sinAlt);

  double cosAz = (sin(dec) - sin(alt) * sin(lat)) / (cos(alt) * cos(lat));
  cosAz = constrain(cosAz, -1.0, 1.0);

  double az = acos(cosAz);
  if (sin(ha) > 0) az = 2.0 * PI - az;

  altDeg = radToDeg(alt);
  azDeg = normalizeAz(radToDeg(az));
  return true;
}

bool altAzToRaDec(double altDeg, double azDeg, double &raDeg, double &decDeg) {
  if (!siteValid || !timeValid) return false;

  double lstDeg = localSiderealDegrees();
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

bool targetAltitudeFromRaDec(double raDeg, double decDeg, double &altDeg) {
  double azDeg = 0.0;
  return raDecToAltAz(raDeg, decDeg, altDeg, azDeg);
}

bool targetRaDecFromAltAz(double altDeg, double azDeg, double &raDeg, double &decDeg) {
  return altAzToRaDec(altDeg, azDeg, raDeg, decDeg);
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

void markSkySafariTimeSource() {
  markTimeSource(TIME_SRC_SKYSAFARI);
}

void markLocationSource(const String &src) {
  currentLocationSource = src;
}

void applyManualSiteValue(double latDeg, double lonDeg) {
  siteLatitudeDeg = latDeg;
  siteLongitudeDeg = lonDeg;
  siteValid = true;
}

void applySiteElevationValue(double elevationMeters) {
  siteElevationMeters = elevationMeters;
}

void applyUtcOffsetValue(int offsetMinutes, const String &source) {
  utcOffsetMinutes = offsetMinutes;
  utcOffsetSource = source;
}

void applyLocalDateTimeValue(int year, int month, int day, int hour, int minute, int second, TimeSource source) {
  localYear = year;
  localMonth = month;
  localDay = day;
  localHour = hour;
  localMinute = minute;
  localSecond = second;
  timeValid = true;
  markTimeSource(source);
  timeSetMillis = millis();
}

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
                                         int second) {
  if (gotLocation) {
    applyManualSiteValue(latDeg, lonDeg);
    markLocationSource("Browser HTTPS");
  }
  if (hasElevation) applySiteElevationValue(elevationMeters);
  if (hasOffset) applyUtcOffsetValue(offsetMinutes, "browser/https");
  if (hasYear) localYear = year;
  if (hasMonth) localMonth = month;
  if (hasDay) localDay = day;
  if (hasHour) localHour = hour;
  if (hasMinute) localMinute = minute;
  if (hasSecond) localSecond = second;

  timeValid = true;
  markTimeSource(TIME_SRC_WEB);
  timeSetMillis = millis();
  computeAltAzFromRaDec();
}
