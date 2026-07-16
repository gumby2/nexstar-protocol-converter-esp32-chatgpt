#pragma once

#include <Arduino.h>
#include <stdint.h>

extern unsigned long lastMountPollMs;
extern unsigned long lastComputedCoordMs;
extern unsigned long lastPositionDemandMs;
extern const unsigned long POSITION_DEMAND_ACTIVE_MS;

extern bool backgroundPollingAutoDisabled;
extern unsigned long lastPollSchedulerLatencyMs;
extern unsigned long maxPollSchedulerLatencyMs;
extern unsigned long nextMountPollDueMs;
extern unsigned long mountPollsStarted;
extern unsigned long mountPollsDeferredBusy;
extern unsigned long mountPollsMissedDeadline;
extern unsigned long mountPollsSucceeded;
extern unsigned long firstSuccessfulMountPollMs;
extern unsigned long lastSuccessfulMountPollMs;
extern int lastSuccessfulMountPollYear;
extern int lastSuccessfulMountPollMonth;
extern int lastSuccessfulMountPollDay;
extern int lastSuccessfulMountPollHour;
extern int lastSuccessfulMountPollMinute;
extern int lastSuccessfulMountPollSecond;

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

bool mountPositionApiRaDec(double &raDeg, double &decDeg, unsigned long *ageMs = nullptr);
bool mountPositionApiAltAz(double &altDeg, double &azDeg, unsigned long *ageMs = nullptr);
bool mountPositionApiHasRaDec();
bool mountPositionApiHasAltAz();

void updateMountReportedRaDec(double raDeg, double decDeg, unsigned long nowMs = 0);
void updateMountReportedAltAz(double altDeg, double azDeg, unsigned long nowMs = 0);
void primeGotoCompletionRaDec(double raDeg, double decDeg, unsigned long nowMs = 0);
void invalidateCachesAfterGoto();
void markMountPollTimestamp(unsigned long nowMs = 0);

void markPositionDemand(const char* reason = nullptr);
bool positionDemandActive();
unsigned long effectiveMountPollIntervalMs();
void resetMountPollScheduler();
void resetMountPollFailures();

void serviceMountPolling();
void computeAltAzFromRaDec();
void computeRaDecFromAltAz();
void invalidateComputedAltAz();
