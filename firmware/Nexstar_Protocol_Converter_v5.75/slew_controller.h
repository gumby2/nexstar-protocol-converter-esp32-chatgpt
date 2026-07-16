#pragma once

#include <Arduino.h>
#include <stdint.h>

enum QueuedGotoType {
  QUEUED_GOTO_NONE = 0,
  QUEUED_GOTO_RADEC = 1,
  QUEUED_GOTO_ALTAZ = 2
};

extern bool asyncSlewPending;
extern bool asyncAltAzSlewPending;
extern bool asyncSlewRunning;
extern bool asyncNudgePending;
extern bool asyncRaDecReadPending;
extern bool asyncAltAzReadPending;
extern bool lastGotoCommandAccepted;
extern unsigned long lastGotoAcceptedMs;
extern double targetRA_deg;
extern double targetDec_deg;
extern QueuedGotoType queuedGotoType;
extern unsigned long queuedGotoMs;
extern uint32_t gotoQueueAccepted;
extern uint32_t gotoQueueStarted;
extern uint32_t gotoQueueTimedOut;
extern uint32_t gotoQueueReplaced;
extern uint32_t nudgeGotoQueueRequests;
extern uint32_t gotoQueueImmediateAcks;
extern uint32_t queuedGotoPositionCacheReplies;
extern bool lx200GotoUiActive;
extern uint8_t lx200GotoUiSource;
extern unsigned long lx200GotoUiStartedMs;
extern uint32_t lx200GotoUiStartedCount;
extern uint32_t lx200GotoUiCompletedCount;
extern uint32_t lx200GotoUiStopRequests;
extern const unsigned long BT_POST_GOTO_FAST_POLL_WINDOW_MS;

bool nudgeRelativeAltAz(double altDelta, double azDelta);
bool queueRelativeAltAzGoto(double altDelta, double azDelta, uint8_t source, const char* reason);
bool enqueueGotoRaDec(double raDeg, double decDeg, uint8_t source, const char* reason);
bool enqueueGotoAltAz(double altDeg, double azDeg, uint8_t source, const char* reason);
void queueAsyncSlew(double raDeg, double decDeg);
void queueAsyncAltAzSlew(double altDeg, double azDeg);
void queueAsyncNudge(double altDelta, double azDelta);
void queueAsyncRaDecRead();
void queueAsyncAltAzRead();
void serviceGotoQueue();
void serviceGotoCompletionWatch();
void serviceAsyncSlew();
bool mountPollingBlocked();
bool mountCommandPathBusy();
bool gotoQueueSlewInProgress();
bool hasQueuedGoto();
bool queuedGotoOrSlewActive();
bool slewControllerBusy();
bool slewControllerPendingOrRunning();
bool lx200GotoUiIsActive();
bool gotoCompletionWatchIsActive();
bool gotoCompletionVerifyIsPending();
const char* queuedGotoTypeName(QueuedGotoType t);
unsigned long queuedGotoAgeMs();
unsigned long effectiveGotoQueueTimeoutMs();
unsigned long sanitizeGotoQueueTimeoutMs(unsigned long v);
void markGotoQueueImmediateAck(const char* protocol, const char* detail);
void markLX200GotoUiStarted(uint8_t source, const char* detail);
void handleLX200StopUiRequest(uint8_t source);
void clearPendingReadRequestsForLX200Goto(const char* srcName);
