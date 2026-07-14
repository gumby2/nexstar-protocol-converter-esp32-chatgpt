#include "nexstar_protocol.h"

#include "logging.h"
#include "mount_transport.h"
#include "observer_time.h"
#include "settings.h"

#include <math.h>

extern unsigned long lastMountPollMs;
extern bool asyncSlewRunning;
extern unsigned long lastGotoAcceptedMs;
extern bool lastGotoCommandAccepted;

extern double cachedRA_deg;
extern double cachedDec_deg;
extern bool cacheValid;
extern double cachedAlt_deg;
extern double cachedAz_deg;
extern bool altAzCacheValid;
extern bool altAzComputed;

extern double mountCurrentRA_deg;
extern double mountCurrentDec_deg;
extern bool mountCurrentRaDecValid;
extern double mountCurrentAlt_deg;
extern double mountCurrentAz_deg;
extern bool mountCurrentAltAzValid;
extern unsigned long mountCurrentRaDecMs;
extern unsigned long mountCurrentAltAzMs;

static int16_t readBE16(const uint8_t* payload) {
  return (int16_t)((payload[0] << 8) | payload[1]);
}

static double signedRAForMount(double ra) {
  ra = normalizeRA(ra);
  if (ra > 180.0) return 0.0 - (360.0 - ra);
  return ra;
}

static double signedAzForMount(double az) {
  az = normalizeAz(az);
  if (az > 180.0) return az - 360.0;
  return az;
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

bool getRaDec(double &ra, double &dec, bool forcePoll) {
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
      int16_t ran = readBE16(&payload[0]);
      int16_t decn = readBE16(&payload[2]);

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
      int16_t azn = readBE16(&payload[0]);
      int16_t altn = readBE16(&payload[2]);

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

bool getAltAz(double &alt, double &az, bool forceMountPoll) {
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
