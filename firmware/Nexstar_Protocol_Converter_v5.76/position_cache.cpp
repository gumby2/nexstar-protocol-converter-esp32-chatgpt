#include "position_cache.h"

#include "logging.h"
#include "mount_transport.h"
#include "nexstar_protocol.h"
#include "observer_time.h"
#include "settings.h"

unsigned long lastMountPollMs = 0;
unsigned long lastComputedCoordMs = 0;
unsigned long lastPositionDemandMs = 0;
const unsigned long POSITION_DEMAND_ACTIVE_MS = 15000;

static const uint8_t BACKGROUND_POLL_FAIL_LIMIT = 3;
uint8_t backgroundPollFailCount = 0;
bool backgroundPollingAutoDisabled = false;
unsigned long lastPollSchedulerLatencyMs = 0;
unsigned long maxPollSchedulerLatencyMs = 0;
unsigned long nextMountPollDueMs = 0;
unsigned long mountPollsStarted = 0;
unsigned long mountPollsDeferredBusy = 0;
unsigned long mountPollsMissedDeadline = 0;
unsigned long mountPollsSucceeded = 0;
unsigned long firstSuccessfulMountPollMs = 0;
unsigned long lastSuccessfulMountPollMs = 0;
int lastSuccessfulMountPollYear = 0;
int lastSuccessfulMountPollMonth = 0;
int lastSuccessfulMountPollDay = 0;
int lastSuccessfulMountPollHour = 0;
int lastSuccessfulMountPollMinute = 0;
int lastSuccessfulMountPollSecond = 0;

double cachedRA_deg = 0.0;
double cachedDec_deg = 0.0;
bool cacheValid = false;

double cachedAlt_deg = 0.0;
double cachedAz_deg = 0.0;
bool altAzCacheValid = false;
bool altAzComputed = false;

double mountCurrentRA_deg = 0.0;
double mountCurrentDec_deg = 0.0;
bool mountCurrentRaDecValid = false;
double mountCurrentAlt_deg = 0.0;
double mountCurrentAz_deg = 0.0;
bool mountCurrentAltAzValid = false;
unsigned long mountCurrentRaDecMs = 0;
unsigned long mountCurrentAltAzMs = 0;

extern uint32_t positionApiCacheReplies;
extern uint32_t positionApiCacheMisses;

bool positionDemandClientConnected();
bool mountPollingBlocked();

bool mountPositionApiRaDec(double &raDeg, double &decDeg, unsigned long *ageMs) {
  if (!mountCurrentRaDecValid) {
    positionApiCacheMisses++;
    return false;
  }
  raDeg = mountCurrentRA_deg;
  decDeg = mountCurrentDec_deg;
  if (ageMs) *ageMs = millis() - mountCurrentRaDecMs;
  positionApiCacheReplies++;
  return true;
}

bool mountPositionApiAltAz(double &altDeg, double &azDeg, unsigned long *ageMs) {
  if (!mountCurrentAltAzValid) {
    positionApiCacheMisses++;
    return false;
  }
  altDeg = mountCurrentAlt_deg;
  azDeg = mountCurrentAz_deg;
  if (ageMs) *ageMs = millis() - mountCurrentAltAzMs;
  positionApiCacheReplies++;
  return true;
}

bool mountPositionApiHasRaDec() {
  return mountCurrentRaDecValid;
}

bool mountPositionApiHasAltAz() {
  return mountCurrentAltAzValid;
}

void updateMountReportedRaDec(double raDeg, double decDeg, unsigned long nowMs) {
  if (nowMs == 0) nowMs = millis();
  cachedRA_deg = normalizeRA(raDeg);
  cachedDec_deg = decDeg;
  cacheValid = true;
  mountCurrentRA_deg = cachedRA_deg;
  mountCurrentDec_deg = cachedDec_deg;
  mountCurrentRaDecValid = true;
  mountCurrentRaDecMs = nowMs;
  lastMountPollMs = nowMs;
}

void updateMountReportedAltAz(double altDeg, double azDeg, unsigned long nowMs) {
  if (nowMs == 0) nowMs = millis();
  cachedAlt_deg = altDeg;
  cachedAz_deg = normalizeAz(azDeg);
  altAzCacheValid = true;
  altAzComputed = false;
  mountCurrentAlt_deg = cachedAlt_deg;
  mountCurrentAz_deg = cachedAz_deg;
  mountCurrentAltAzValid = true;
  mountCurrentAltAzMs = nowMs;
}

void primeGotoCompletionRaDec(double raDeg, double decDeg, unsigned long nowMs) {
  updateMountReportedRaDec(raDeg, decDeg, nowMs);
}

void invalidateCachesAfterGoto() {
  lastMountPollMs = 0;
}

void markMountPollTimestamp(unsigned long nowMs) {
  lastMountPollMs = nowMs == 0 ? millis() : nowMs;
}

void markPositionDemand(const char* reason) {
  lastPositionDemandMs = millis();
  if (nextMountPollDueMs == 0 || !mountCurrentRaDecValid) nextMountPollDueMs = lastPositionDemandMs;
  if (reason) LOG_MOUNT_T("Position demand active: %s", reason);
}

bool positionDemandActive() {
  unsigned long nowMs = millis();
  if (lastPositionDemandMs && nowMs - lastPositionDemandMs < POSITION_DEMAND_ACTIVE_MS) return true;
  return positionDemandClientConnected();
}

unsigned long effectiveMountPollIntervalMs() {
  if (!timeValid) return 0;
  if (positionDemandActive()) return pollIntervalMs;
  return idlePollIntervalMs;
}

void resetMountPollScheduler() {
  nextMountPollDueMs = 0;
  backgroundPollingAutoDisabled = false;
}

void resetMountPollFailures() {
  backgroundPollFailCount = 0;
}

void markSuccessfulMountPoll(unsigned long pollMs) {
  mountPollsSucceeded++;
  if (firstSuccessfulMountPollMs == 0) firstSuccessfulMountPollMs = pollMs;
  lastSuccessfulMountPollMs = pollMs;
  currentLocalParts(lastSuccessfulMountPollYear, lastSuccessfulMountPollMonth, lastSuccessfulMountPollDay,
                    lastSuccessfulMountPollHour, lastSuccessfulMountPollMinute, lastSuccessfulMountPollSecond);
}

void computeAltAzFromRaDec() {
  if (!cacheValid || !siteValid || !timeValid) return;

  double altDeg = 0.0;
  double azDeg = 0.0;
  if (!raDecToAltAz(cachedRA_deg, cachedDec_deg, altDeg, azDeg)) return;

  const unsigned long nowMs = millis();
  cachedAlt_deg = altDeg;
  cachedAz_deg = azDeg;
  altAzCacheValid = true;
  altAzComputed = true;
  lastComputedCoordMs = nowMs;

  // The original mount is normally polled in RA/Dec.  Alt/Az is therefore
  // derived from the current RA/Dec plus valid site/time data.  Publish that
  // derived current position through the same display/API cache used by the
  // web page and Telnet banner; otherwise both interfaces remain blank even
  // though a valid Alt/Az calculation exists.
  mountCurrentAlt_deg = cachedAlt_deg;
  mountCurrentAz_deg = cachedAz_deg;
  mountCurrentAltAzValid = true;
  mountCurrentAltAzMs = nowMs;
}

void computeRaDecFromAltAz() {
  if (!altAzCacheValid || !siteValid || !timeValid) return;

  double ra = 0.0;
  double dec = 0.0;
  if (!altAzToRaDec(cachedAlt_deg, cachedAz_deg, ra, dec)) return;

  cachedRA_deg = ra;
  cachedDec_deg = dec;
  cacheValid = true;
  lastComputedCoordMs = millis();

  LOGD("Computed RA/Dec from Alt/Az: RA_deg=%.6f DEC_deg=%.6f RA_hours=%.6f",
       cachedRA_deg, cachedDec_deg, cachedRA_deg / 15.0);
}

void invalidateComputedAltAz() {
  if (altAzComputed) altAzCacheValid = false;
  altAzComputed = false;
}

void serviceMountPolling() {
  unsigned long activePollMs = effectiveMountPollIntervalMs();
  if (activePollMs == 0) {
    nextMountPollDueMs = 0;
    return;
  }

  if (!timeValid) {
    nextMountPollDueMs = 0;
    return;
  }

  unsigned long nowMs = millis();
  if (nextMountPollDueMs == 0) {
    nextMountPollDueMs = nowMs + activePollMs;
    return;
  }

  if ((long)(nowMs - nextMountPollDueMs) < 0) return;

  if (mountBusy || mountPollingBlocked()) {
    mountPollsDeferredBusy++;
    return;
  }

  lastPollSchedulerLatencyMs = nowMs - nextMountPollDueMs;
  if (lastPollSchedulerLatencyMs > maxPollSchedulerLatencyMs) maxPollSchedulerLatencyMs = lastPollSchedulerLatencyMs;
  if (lastPollSchedulerLatencyMs > activePollMs) mountPollsMissedDeadline++;
  mountPollsStarted++;

  LOG_MOUNT_D("Top-priority mount poll due: interval=%lu ms latency=%lu ms; querying actual mount RA/Dec with E",
              activePollMs, lastPollSchedulerLatencyMs);

  double ra = 0.0;
  double dec = 0.0;

  bool gotRaDec = getRaDec(ra, dec, true);
  unsigned long finishMs = millis();
  lastMountPollMs = finishMs;
  if (gotRaDec) {
    markSuccessfulMountPoll(finishMs);
    computeAltAzFromRaDec();
  }

  nextMountPollDueMs += activePollMs;
  while ((long)(finishMs - nextMountPollDueMs) >= 0) {
    nextMountPollDueMs += activePollMs;
    mountPollsMissedDeadline++;
  }

  if (gotRaDec) {
    backgroundPollFailCount = 0;
  } else {
    backgroundPollFailCount++;
    if (backgroundPollFailCount < BACKGROUND_POLL_FAIL_LIMIT) {
      LOG_MOUNT_D("Top-priority mount poll failed count=%lu; polling remains enabled",
                  (unsigned long)backgroundPollFailCount);
    } else {
      LOG_MOUNT_W("Top-priority mount poll failed count=%lu; polling remains enabled",
                  (unsigned long)backgroundPollFailCount);
    }
  }
}
