#include "slew_controller.h"

#include "logging.h"
#include "lx200_protocol.h"
#include "mount_transport.h"
#include "nexstar_protocol.h"
#include "observer_time.h"
#include "position_cache.h"
#include "settings.h"

bool asyncSlewPending = false;
bool asyncSlewRunning = false;
static unsigned long asyncSlewEarliestMs = 0;
unsigned long lastGotoAcceptedMs = 0;

QueuedGotoType queuedGotoType = QUEUED_GOTO_NONE;
static double queuedGotoRA_deg = 0.0;
static double queuedGotoDec_deg = 0.0;
static double queuedGotoAlt_deg = 0.0;
static double queuedGotoAz_deg = 0.0;
static uint8_t queuedGotoSource = LX_SRC_WIFI;
unsigned long queuedGotoMs = 0;
static unsigned long queuedGotoLastLogMs = 0;
static const unsigned long GOTO_QUEUE_LOG_MS = 5000UL;
uint32_t gotoQueueAccepted = 0;
uint32_t gotoQueueStarted = 0;
uint32_t gotoQueueTimedOut = 0;
uint32_t gotoQueueReplaced = 0;
uint32_t nudgeGotoQueueRequests = 0;
uint32_t gotoQueueImmediateAcks = 0;
uint32_t queuedGotoPositionCacheReplies = 0;

// SkySafari/LX200 UI slew-state helper.
// This is intentionally separate from the real NexStar mount busy/watch flags:
// - Real mount GOTO remains single-command and cannot be aborted.
// - SkySafari can get stuck showing Stop if our app-side GOTO state stays active
//   through post-GOTO verification or after a harmless :Q# UI stop request.
bool lx200GotoUiActive = false;
uint8_t lx200GotoUiSource = LX_SRC_WIFI;
unsigned long lx200GotoUiStartedMs = 0;
static unsigned long lx200GotoUiCompletedMs = 0;
uint32_t lx200GotoUiStartedCount = 0;
uint32_t lx200GotoUiCompletedCount = 0;
uint32_t lx200GotoUiStopRequests = 0;

// Non-blocking SkySafari GOTO completion watcher.
// Original NexStar GOTO is still single-command: after R payload is sent,
// no E/Z mount query is allowed until final '@' arrives.
static bool gotoCompletionWatchActive = false;
static bool gotoCompletionVerifyPending = false;
static unsigned long gotoCompletionWatchStartMs = 0;
static unsigned long gotoCompletionWatchLastLogMs = 0;
static unsigned long gotoCompletionVerifyDueMs = 0;
static double gotoCompletionTargetRA_deg = 0.0;
static double gotoCompletionTargetDec_deg = 0.0;
static const unsigned long GOTO_COMPLETION_WATCH_LOG_MS = 15000;
static const unsigned long POST_GOTO_VERIFY_DELAY_MS = 750;
const unsigned long BT_POST_GOTO_FAST_POLL_WINDOW_MS = 20000;
bool lastGotoCommandAccepted = false;

double targetRA_deg = 0.0;
double targetDec_deg = 0.0;
static double pendingRA_deg = 0.0;
static double pendingDec_deg = 0.0;
static double pendingAlt_deg = 0.0;
static double pendingAz_deg = 0.0;
static double pendingNudgeAltDelta = 0.0;
static double pendingNudgeAzDelta = 0.0;
bool asyncAltAzSlewPending = false;
static unsigned long asyncAltAzSlewEarliestMs = 0;
bool asyncNudgePending = false;
bool asyncRaDecReadPending = false;
bool asyncAltAzReadPending = false;

static void clearQueuedGoto(const char* reason) {
  if (queuedGotoType != QUEUED_GOTO_NONE) {
    LOG_MOUNT_I("Clearing queued GOTO type=%s reason=%s",
                queuedGotoTypeName(queuedGotoType),
                reason ? reason : "none");
  }
  queuedGotoType = QUEUED_GOTO_NONE;
  queuedGotoLastLogMs = 0;
}

bool nudgeRelativeAltAz(double altDelta, double azDelta) {
  // Web/Alpaca/local nudges use the same GOTO queue path as SkySafari nudges.
  return queueRelativeAltAzGoto(altDelta, azDelta, LX_SRC_WIFI, "relative nudge while mount busy");
}

bool queueRelativeAltAzGoto(double altDelta, double azDelta, uint8_t source, const char* reason) {
  double alt = 0.0;
  double az = 0.0;
  unsigned long ageMs = 0;

  // Application-side nudges must not perform their own Z/E mount reads.
  // They use the same cached position API used by SkySafari/Stellarium polling.
  if (!mountPositionApiAltAz(alt, az, &ageMs)) {
    LOG_MOUNT_W("Nudge-to-GOTO rejected: mount-position API Alt/Az cache is not valid");
    return false;
  }

  double newAlt = clampAlt(alt + altDelta);
  double newAz = normalizeAz(az + azDelta);
  nudgeGotoQueueRequests++;

  LOG_MOUNT_I("Nudge-to-GOTO request source=%s age=%lu ms: Alt %.6f -> %.6f, Az %.6f -> %.6f",
              lx200SourceName(source),
              (unsigned long)ageMs,
              alt,
              newAlt,
              az,
              newAz);

  if (mountCommandPathBusy()) {
    return enqueueGotoAltAz(newAlt, newAz, source, reason ? reason : "nudge while mount busy");
  }

  queueAsyncAltAzSlew(newAlt, newAz);
  return true;
}

bool mountPollingBlocked() {
  return asyncSlewRunning || asyncSlewPending || asyncAltAzSlewPending ||
         asyncNudgePending || asyncRaDecReadPending || asyncAltAzReadPending ||
         gotoCompletionWatchActive || gotoCompletionVerifyPending;
}

void markLX200GotoUiStarted(uint8_t source, const char* detail) {
  lx200GotoUiActive = true;
  lx200GotoUiSource = source;
  lx200GotoUiStartedMs = millis();
  lx200GotoUiStartedCount++;
  LOG_SKY_I("LX200 %s UI GOTO state STARTED: %s", lx200SourceName(source), detail ? detail : "goto accepted");
}

static void clearLX200GotoUiState(const char* reason) {
  if (lx200GotoUiActive) {
    unsigned long ageMs = millis() - lx200GotoUiStartedMs;
    LOG_SKY_I("LX200 %s UI GOTO state CLEARED after %lu ms: %s",
              lx200SourceName(lx200GotoUiSource),
              (unsigned long)ageMs,
              reason ? reason : "complete");
    lx200GotoUiCompletedCount++;
  } else {
    LOG_SKY_D("LX200 UI GOTO state already idle: %s", reason ? reason : "clear requested");
  }
  lx200GotoUiActive = false;
  lx200GotoUiCompletedMs = millis();
}

void handleLX200StopUiRequest(uint8_t source) {
  lx200GotoUiStopRequests++;

  // Cancel only app-side GOTO work that has not yet reached the mount.
  // Never send an abort command and never clear the real completion watch while
  // the original NexStar is executing a GOTO, because that mount must run to '@'.
  bool cancelledPending = false;
  if (!asyncSlewRunning && !gotoCompletionWatchActive && !gotoCompletionVerifyPending) {
    if (hasQueuedGoto()) {
      clearQueuedGoto("LX200 :Q# stop before mount command started");
      cancelledPending = true;
    }
    if (asyncSlewPending) {
      asyncSlewPending = false;
      cancelledPending = true;
      LOG_SKY_I("LX200 %s :Q# cleared pending RA/Dec async slew before mount command started", lx200SourceName(source));
    }
    if (asyncAltAzSlewPending) {
      asyncAltAzSlewPending = false;
      cancelledPending = true;
      LOG_SKY_I("LX200 %s :Q# cleared pending Alt/Az async slew before mount command started", lx200SourceName(source));
    }
  }

  clearLX200GotoUiState(cancelledPending ? "LX200 :Q# stop/cancel before mount command" : "LX200 :Q# UI stop; real NexStar abort unsupported");
}

void clearPendingReadRequestsForLX200Goto(const char* srcName) {
  bool hadPending = asyncRaDecReadPending || asyncAltAzReadPending;

  asyncRaDecReadPending = false;
  asyncAltAzReadPending = false;

  if (hadPending) {
    LOG_SKY_D("LX200 %s GOTO cleared pending read request(s) before mount command", srcName);
  }
}

bool mountCommandPathBusy() {
  return mountBusy || asyncSlewRunning || asyncSlewPending || asyncAltAzSlewPending ||
         asyncNudgePending || asyncRaDecReadPending || asyncAltAzReadPending ||
         gotoCompletionWatchActive || gotoCompletionVerifyPending;
}

bool gotoQueueSlewInProgress() {
  return asyncSlewRunning || gotoCompletionWatchActive || gotoCompletionVerifyPending;
}

unsigned long effectiveGotoQueueTimeoutMs() {
  // Default timeout is 10 seconds unless a slew is already in progress.
  // During an active slew/completion watch, timeout is deferred until that slew clears.
  if (gotoQueueSlewInProgress()) return 0UL;
  return gotoQueueTimeoutMs;
}

unsigned long sanitizeGotoQueueTimeoutMs(unsigned long v) {
  if (v < 1000UL) v = 1000UL;
  if (v > 600000UL) v = 600000UL;
  return v;
}

const char* queuedGotoTypeName(QueuedGotoType t) {
  if (t == QUEUED_GOTO_RADEC) return "RA/Dec";
  if (t == QUEUED_GOTO_ALTAZ) return "Alt/Az";
  return "none";
}

bool hasQueuedGoto() {
  return queuedGotoType != QUEUED_GOTO_NONE;
}

unsigned long queuedGotoAgeMs() {
  return hasQueuedGoto() ? millis() - queuedGotoMs : 0UL;
}

bool queuedGotoOrSlewActive() {
  return hasQueuedGoto() || asyncSlewPending || asyncAltAzSlewPending ||
         asyncSlewRunning || gotoCompletionWatchActive || gotoCompletionVerifyPending;
}

bool slewControllerBusy() {
  return mountCommandPathBusy();
}

bool slewControllerPendingOrRunning() {
  return asyncSlewPending || asyncAltAzSlewPending || asyncNudgePending ||
         asyncRaDecReadPending || asyncAltAzReadPending || asyncSlewRunning;
}

bool lx200GotoUiIsActive() {
  return lx200GotoUiActive;
}

bool gotoCompletionWatchIsActive() {
  return gotoCompletionWatchActive;
}

bool gotoCompletionVerifyIsPending() {
  return gotoCompletionVerifyPending;
}

void markGotoQueueImmediateAck(const char* protocol, const char* detail) {
  gotoQueueImmediateAcks++;
  LOG_MOUNT_I("Queued GOTO caller acknowledged immediately: protocol=%s detail=%s activeQueue=%s",
              protocol ? protocol : "unknown",
              detail ? detail : "",
              hasQueuedGoto() ? "yes" : "no");
}

bool enqueueGotoRaDec(double raDeg, double decDeg, uint8_t source, const char* reason) {
  if (hasQueuedGoto()) {
    gotoQueueReplaced++;
    LOG_MOUNT_W("Replacing existing queued GOTO type=%s with new RA/Dec GOTO; old queued age=%lu ms",
                queuedGotoTypeName(queuedGotoType),
                (unsigned long)(millis() - queuedGotoMs));
  }

  queuedGotoType = QUEUED_GOTO_RADEC;
  queuedGotoRA_deg = normalizeRA(raDeg);
  queuedGotoDec_deg = decDeg;
  queuedGotoSource = source;
  queuedGotoMs = millis();
  queuedGotoLastLogMs = 0;
  gotoQueueAccepted++;

  targetRA_deg = queuedGotoRA_deg;
  targetDec_deg = queuedGotoDec_deg;
  lastGotoAcceptedMs = millis();

  LOG_MOUNT_I("Queued RA/Dec GOTO waiting for mount idle: RA_deg=%.6f DEC_deg=%.6f source=%s reason=%s timeout=%lu ms",
              queuedGotoRA_deg,
              queuedGotoDec_deg,
              lx200SourceName(source),
              reason ? reason : "busy",
              gotoQueueTimeoutMs);
  return true;
}

bool enqueueGotoAltAz(double altDeg, double azDeg, uint8_t source, const char* reason) {
  if (hasQueuedGoto()) {
    gotoQueueReplaced++;
    LOG_MOUNT_W("Replacing existing queued GOTO type=%s with new Alt/Az GOTO; old queued age=%lu ms",
                queuedGotoTypeName(queuedGotoType),
                (unsigned long)(millis() - queuedGotoMs));
  }

  queuedGotoType = QUEUED_GOTO_ALTAZ;
  queuedGotoAlt_deg = clampAlt(altDeg);
  queuedGotoAz_deg = normalizeAz(azDeg);
  queuedGotoSource = source;
  queuedGotoMs = millis();
  queuedGotoLastLogMs = 0;
  gotoQueueAccepted++;

  lastGotoAcceptedMs = millis();

  LOG_MOUNT_I("Queued Alt/Az GOTO waiting for mount idle: ALT_deg=%.6f AZ_deg=%.6f source=%s reason=%s timeout=%lu ms",
              queuedGotoAlt_deg,
              queuedGotoAz_deg,
              lx200SourceName(source),
              reason ? reason : "busy",
              gotoQueueTimeoutMs);
  return true;
}

void serviceGotoQueue() {
  if (!hasQueuedGoto()) return;

  unsigned long nowMs = millis();
  unsigned long ageMs = nowMs - queuedGotoMs;
  unsigned long timeoutMs = effectiveGotoQueueTimeoutMs();

  if (timeoutMs > 0UL && ageMs > timeoutMs) {
    gotoQueueTimedOut++;
    LOG_MOUNT_W("Queued %s GOTO timed out after %lu ms waiting for mount idle; timeout=%lu ms; command dropped",
                queuedGotoTypeName(queuedGotoType),
                (unsigned long)ageMs,
                timeoutMs);
    clearQueuedGoto("timeout");
    clearLX200GotoUiState("queued GOTO timed out before mount command started");
    return;
  }

  if (mountCommandPathBusy()) {
    if (queuedGotoLastLogMs == 0 || nowMs - queuedGotoLastLogMs >= GOTO_QUEUE_LOG_MS) {
      queuedGotoLastLogMs = nowMs;
      LOG_MOUNT_I("Queued %s GOTO still waiting for mount idle; age=%lu ms timeout=%lu ms%s",
                  queuedGotoTypeName(queuedGotoType),
                  (unsigned long)ageMs,
                  timeoutMs,
                  timeoutMs == 0UL ? " (deferred while slew in progress)" : "");
    }
    return;
  }

  QueuedGotoType type = queuedGotoType;
  double ra = queuedGotoRA_deg;
  double dec = queuedGotoDec_deg;
  double alt = queuedGotoAlt_deg;
  double az = queuedGotoAz_deg;

  clearPendingReadRequestsForLX200Goto(lx200SourceName(queuedGotoSource));
  clearQueuedGoto("starting");
  gotoQueueStarted++;

  if (type == QUEUED_GOTO_RADEC) {
    LOG_MOUNT_I("Starting queued RA/Dec GOTO now after wait: RA_deg=%.6f DEC_deg=%.6f", ra, dec);
    queueAsyncSlew(ra, dec);
  } else if (type == QUEUED_GOTO_ALTAZ) {
    LOG_MOUNT_I("Starting queued Alt/Az GOTO now after wait: ALT_deg=%.6f AZ_deg=%.6f", alt, az);
    queueAsyncAltAzSlew(alt, az);
  }
}

void queueAsyncSlew(double raDeg, double decDeg) {
  pendingRA_deg = normalizeRA(raDeg);
  pendingDec_deg = decDeg;
  targetRA_deg = pendingRA_deg;
  targetDec_deg = pendingDec_deg;
  asyncSlewEarliestMs = millis() + 750;
  lastGotoAcceptedMs = millis();
  asyncSlewPending = true;
  LOG_MOUNT_I("Queued async slew RA_deg=%.6f DEC_deg=%.6f", pendingRA_deg, pendingDec_deg);
}

void queueAsyncAltAzSlew(double altDeg, double azDeg) {
  pendingAlt_deg = clampAlt(altDeg);
  pendingAz_deg = normalizeAz(azDeg);
  asyncAltAzSlewEarliestMs = millis() + 750;
  lastGotoAcceptedMs = millis();
  asyncAltAzSlewPending = true;
  LOGI("Queued async Alt/Az slew ALT_deg=%.6f AZ_deg=%.6f", pendingAlt_deg, pendingAz_deg);
}

void queueAsyncNudge(double altDelta, double azDelta) {
  pendingNudgeAltDelta = altDelta;
  pendingNudgeAzDelta = azDelta;
  asyncNudgePending = true;
  LOGI("Queued async nudge ALT_delta=%.6f AZ_delta=%.6f", pendingNudgeAltDelta, pendingNudgeAzDelta);
}

void queueAsyncRaDecRead() {
  asyncRaDecReadPending = true;
}

void queueAsyncAltAzRead() {
  asyncAltAzReadPending = true;
}

void serviceGotoCompletionWatch() {
  if (!gotoCompletionWatchActive && !gotoCompletionVerifyPending) return;

  unsigned long nowMs = millis();

  if (gotoCompletionWatchActive) {
    while (mountRawAvailable()) {
      uint8_t b = (uint8_t)mountReadRawByte();
      printRxByte(b);
      markMountResponse();

      if (b == '@') {
        LOG_MOUNT_I("SkySafari GOTO completion '@' received; clearing LX200 UI slew state before real E/Z verification reads");

        // The original NexStar is now done with the GOTO.  Do not wait for the
        // slower post-GOTO E/Z verification before telling app clients that the
        // slew is complete.  SkySafari may keep its Goto/Stop button stuck on
        // Stop if :D# continues to imply slewing or if GR/GD still show the old
        // pre-GOTO cached coordinates.
        asyncSlewRunning = false;
        clearLX200GotoUiState("NexStar final '@' received");

        // Prime only the RA/Dec app-facing cache with the accepted GOTO target
        // so SkySafari immediately sees coordinates near the requested target.
        // The real mount-reported E/Z verification below will overwrite this
        // estimate as soon as it succeeds.
        primeGotoCompletionRaDec(gotoCompletionTargetRA_deg, gotoCompletionTargetDec_deg, nowMs);
        LOG_MOUNT_I("SkySafari app-facing RA/Dec cache primed at GOTO completion: RA=%.6f Dec=%.6f until E/Z verification",
                    cachedRA_deg, cachedDec_deg);

        gotoCompletionWatchActive = false;
        gotoCompletionVerifyPending = true;
        gotoCompletionVerifyDueMs = nowMs + POST_GOTO_VERIFY_DELAY_MS;
        gotoCompletionWatchLastLogMs = 0;
        return;
      }

      LOG_MOUNT_W("SkySafari GOTO completion watch ignored byte 0x%02X '%c'",
                  b, isPrintableByte(b) ? b : '.');
    }

    if (gotoCompletionWatchLastLogMs == 0 || nowMs - gotoCompletionWatchLastLogMs >= GOTO_COMPLETION_WATCH_LOG_MS) {
      gotoCompletionWatchLastLogMs = nowMs;
      LOG_MOUNT_I("SkySafari GOTO still waiting for NexStar final '@' before post-GOTO E/Z query; elapsed=%lu ms",
                  (unsigned long)(nowMs - gotoCompletionWatchStartMs));
    }

    if (nowMs - gotoCompletionWatchStartMs > GOTO_TIMEOUT_MS) {
      LOG_MOUNT_W("SkySafari GOTO timed out waiting for final '@'; not sending E/Z verification because mount completion is unknown");
      gotoCompletionWatchActive = false;
      gotoCompletionVerifyPending = false;
      asyncSlewRunning = false;
      clearLX200GotoUiState("GOTO completion watch timed out");
      lastMountPollMs = nowMs;
    }
    return;
  }

  if (gotoCompletionVerifyPending) {
    if ((long)(nowMs - gotoCompletionVerifyDueMs) < 0) return;
    if (mountBusy) return;

    gotoCompletionVerifyPending = false;

    LOG_MOUNT_I("Post-GOTO verification: querying actual mount position now with E then Z");
    double verifyRa = 0.0;
    double verifyDec = 0.0;
    double verifyAlt = 0.0;
    double verifyAz = 0.0;

    if (getRaDec(verifyRa, verifyDec, true)) {
      LOG_MOUNT_I("Post-GOTO actual RA/Dec FROM MOUNT: RA_deg=%.6f DEC_deg=%.6f RA_hours=%.6f",
                  verifyRa, verifyDec, verifyRa / 15.0);
    } else {
      LOG_MOUNT_W("Post-GOTO actual RA/Dec read failed; keeping previous cache");
    }

    if (getAltAzFromMount(verifyAlt, verifyAz)) {
      LOG_MOUNT_I("Post-GOTO actual Alt/Az FROM MOUNT: ALT=%.6f AZ=%.6f", verifyAlt, verifyAz);
    } else {
      LOG_MOUNT_W("Post-GOTO actual Alt/Az read failed; keeping previous Alt/Az cache");
    }

    asyncSlewRunning = false;
    markMountPollTimestamp();
    LOG_MOUNT_I("Post-GOTO verification finished");
  }
}

void serviceAsyncSlew() {
  if (mountBusy || asyncSlewRunning) return;

  if (asyncRaDecReadPending) {
    LOGI("Starting queued RA/Dec read now");
    asyncRaDecReadPending = false;
    double ra = 0.0;
    double dec = 0.0;
    if (getRaDec(ra, dec, true)) {
      // Do not compute/overwrite mount-reported Alt/Az after RA/Dec read.
      LOGI("Queued RA/Dec read complete");
    } else {
      LOGW("Queued RA/Dec read failed");
    }
    return;
  }

  if (asyncAltAzReadPending) {
    LOGI("Starting queued Alt/Az read now");
    asyncAltAzReadPending = false;
    double alt = 0.0;
    double az = 0.0;
    if (getAltAz(alt, az, true)) {
      // Do not compute/overwrite mount-reported RA/Dec after Alt/Az read.
      LOGI("Queued Alt/Az read complete");
    } else {
      LOGW("Queued Alt/Az read failed");
    }
    return;
  }

  if (asyncNudgePending) {
    LOGI("Starting queued nudge request now; it will route through the Alt/Az GOTO queue path");
    asyncNudgePending = false;
    if (nudgeRelativeAltAz(pendingNudgeAltDelta, pendingNudgeAzDelta)) {
      LOGI("Queued nudge converted to Alt/Az GOTO request");
      invalidateCachesAfterGoto();
    } else {
      LOGW("Queued nudge failed");
    }
    return;
  }

  if (asyncSlewPending) {
    if ((long)(millis() - asyncSlewEarliestMs) < 0) return;
    LOG_MOUNT_I("Starting queued async RA/Dec slew now");
    asyncSlewPending = false;
    if (gotoRaDec(pendingRA_deg, pendingDec_deg)) {
      gotoCompletionWatchActive = true;
      gotoCompletionVerifyPending = false;
      gotoCompletionWatchStartMs = millis();
      gotoCompletionWatchLastLogMs = 0;
      gotoCompletionVerifyDueMs = 0;
      gotoCompletionTargetRA_deg = pendingRA_deg;
      gotoCompletionTargetDec_deg = pendingDec_deg;
      asyncSlewRunning = true;
      LOG_MOUNT_I("SkySafari GOTO payload sent; watching non-blocking for NexStar final '@' before E/Z verification reads");
    } else {
      asyncSlewRunning = false;
      clearLX200GotoUiState("queued RA/Dec slew failed before mount command accepted");
      LOG_MOUNT_W("Queued RA/Dec slew failed before completion watch could start");
    }
    return;
  }

  if (asyncAltAzSlewPending) {
    if ((long)(millis() - asyncAltAzSlewEarliestMs) < 0) return;
    LOGI("Starting queued async Alt/Az slew now");
    asyncAltAzSlewPending = false;
    if (gotoAltAz(pendingAlt_deg, pendingAz_deg)) {
      LOGI("Queued Alt/Az slew accepted; pausing mount polls while SkySafari stays responsive");
      markMountPollTimestamp();
    }
    return;
  }
}
