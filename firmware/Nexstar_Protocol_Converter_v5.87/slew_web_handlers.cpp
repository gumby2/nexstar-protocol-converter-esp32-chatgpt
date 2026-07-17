#include "slew_web_handlers.h"

#include "logging.h"
#include "network_services.h"
#include "observer_time.h"
#include "position_cache.h"
#include "settings.h"
#include "slew_controller.h"
#include <math.h>

extern void logHttpRequest(const char* where);
extern bool wantsAjax();
extern void sendAjaxOK(const String &message);
extern void sendAjaxFail(const String &message);

bool decWithinSafeLimits(double decDeg) {
  if (!safeDecLimitEnabled) return true;
  return decDeg >= safeDecMinDeg && decDeg <= safeDecMaxDeg;
}

bool altWithinSafeLimits(double altDeg) {
  if (altDeg < 0.0) return false;
  if (!safeAltLimitEnabled) return true;
  return altDeg >= safeAltMinDeg && altDeg <= safeAltMaxDeg;
}

bool allowSlewRaDecBySafety(double raDeg, double decDeg, String &reason) {
  double targetAlt = 0.0;
  if (!targetAltitudeFromRaDec(raDeg, decDeg, targetAlt)) {
    reason = "Blocked: valid site/time required for horizon/altitude safety check";
    return false;
  }

  if (!altWithinSafeLimits(targetAlt)) {
    if (targetAlt < 0.0) reason = "Blocked by horizon safety: target Alt ";
    else reason = "Blocked by Alt safety limit: target Alt ";
    reason += String(targetAlt, 2);
    reason += " deg";
    if (safeAltLimitEnabled && targetAlt >= 0.0) {
      reason += " allowed ";
      reason += String(safeAltMinDeg, 2);
      reason += " to ";
      reason += String(safeAltMaxDeg, 2);
    }
    return false;
  }

  LOG_MOUNT_D("RA/Dec altitude safety OK: targetRA=%.3f targetDec=%.3f computedAlt=%.3f",
              normalizeRA(raDeg), decDeg, targetAlt);
  return true;
}

bool allowSlewAltAzBySafety(double altDeg, double azDeg, String &reason, double *outRaDeg, double *outDecDeg) {
  if (!altWithinSafeLimits(altDeg)) {
    if (altDeg < 0.0) reason = "Blocked by horizon safety: target Alt ";
    else reason = "Blocked by Alt safety limit: target Alt ";
    reason += String(altDeg, 2);
    reason += " deg";
    if (safeAltLimitEnabled && altDeg >= 0.0) {
      reason += " allowed ";
      reason += String(safeAltMinDeg, 2);
      reason += " to ";
      reason += String(safeAltMaxDeg, 2);
    }
    return false;
  }

  if (outRaDeg || outDecDeg) {
    double r = 0.0, d = 0.0;
    if (targetRaDecFromAltAz(altDeg, azDeg, r, d)) {
      if (outRaDeg) *outRaDeg = r;
      if (outDecDeg) *outDecDeg = d;
    }
  }

  LOG_MOUNT_D("Alt/Az altitude safety OK: targetAlt=%.3f targetAz=%.3f",
              altDeg, normalizeAz(azDeg));
  return true;
}

double sanitizeRateValue(double v, double fallback) {
  if (isnan(v) || v <= 0.0) return fallback;
  if (v > 5.0) return 5.0;
  return v;
}

void handleSetAltLimitsPage() {
  logHttpRequest("SET_ALT_LIMITS");
  LOG_SET_I("Setup button pressed: Save Alt Safety Limits");

  bool enabled = server.hasArg("enabled") && server.arg("enabled") == "1";
  double mn = server.hasArg("min") ? server.arg("min").toFloat() : safeAltMinDeg;
  double mx = server.hasArg("max") ? server.arg("max").toFloat() : safeAltMaxDeg;

  if (isnan(mn) || isnan(mx) || mn < 0.0 || mx > 90.0 || mn >= mx) {
    if (wantsAjax()) sendAjaxFail("Invalid Alt limits. Use 0..90 and min < max.");
    else server.send(400, "text/plain", "Invalid Alt limits");
    return;
  }

  safeAltLimitEnabled = enabled;
  safeAltMinDeg = mn;
  safeAltMaxDeg = mx;
  bool ok = savePersistentSettings();

  LOG_SET_I("Alt safety limits %s: min=%.2f max=%.2f save=%s",
            safeAltLimitEnabled ? "enabled" : "disabled",
            safeAltMinDeg, safeAltMaxDeg, ok ? "ok" : "failed");

  if (wantsAjax()) {
    if (ok) sendAjaxOK("Alt safety limits saved");
    else sendAjaxFail("Alt safety limits changed but save failed");
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

void handleSetDecLimitsPage() {
  logHttpRequest("SET_DEC_LIMITS_DEPRECATED");
  safeDecLimitEnabled = false;
  savePersistentSettings();
  LOG_SET_W("Deprecated Dec safety limit request ignored; use Altitude Safety Limits instead");
  if (wantsAjax()) sendAjaxOK("Dec limits are deprecated; use Altitude Safety Limits");
  else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

void handleSetRatesPage() {
  logHttpRequest("SET_RATES");
  LOG_SET_I("Setup button pressed: Save Rates");
  if (server.hasArg("r1")) nudgeRateDeg[0] = sanitizeRateValue(server.arg("r1").toFloat(), nudgeRateDeg[0]);
  if (server.hasArg("r2")) nudgeRateDeg[1] = sanitizeRateValue(server.arg("r2").toFloat(), nudgeRateDeg[1]);
  if (server.hasArg("r3")) nudgeRateDeg[2] = sanitizeRateValue(server.arg("r3").toFloat(), nudgeRateDeg[2]);
  if (server.hasArg("r4")) nudgeRateDeg[3] = sanitizeRateValue(server.arg("r4").toFloat(), nudgeRateDeg[3]);

  lx200CurrentNudgeStepDeg = nudgeRateDeg[1];

  savePersistentSettings();

  LOG_SET_D("Nudge rates saved: RS=%.3f RM=%.3f RC=%.3f RG=%.3f",
       nudgeRateDeg[0], nudgeRateDeg[1], nudgeRateDeg[2], nudgeRateDeg[3]);

  if (wantsAjax()) sendAjaxOK("Nudge rates saved");
  else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

void handleSetWebRatePage() {
  logHttpRequest("SET_WEB_RATE");
  if (server.hasArg("rate")) {
    int r = server.arg("rate").toInt();
    if (r < 1) r = 1;
    if (r > 4) r = 4;
    webSelectedRateIndex = r - 1;
    lx200CurrentNudgeStepDeg = nudgeRateDeg[webSelectedRateIndex];
    savePersistentSettings();
    LOGI("Web selected nudge rate changed to %d = %.3f deg",
         r, nudgeRateDeg[webSelectedRateIndex]);
  }

  if (wantsAjax()) sendAjaxOK("Selected rate set");
  else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}

void handleWebNudgePage() {
  logHttpRequest("WEB_NUDGE");
  String dir = server.hasArg("dir") ? server.arg("dir") : "";
  double step = nudgeRateDeg[webSelectedRateIndex];

  double altDelta = 0.0;
  double azDelta = 0.0;

  if (dir == "n") altDelta = step;
  else if (dir == "s") altDelta = -step;
  else if (dir == "e") azDelta = step;
  else if (dir == "w") azDelta = -step;
  else {
    if (wantsAjax()) sendAjaxFail("Bad direction");
    else server.send(400, "text/plain", "Bad direction");
    return;
  }

  if (slewControllerPendingOrRunning()) {
    if (wantsAjax()) sendAjaxFail("Another mount command is already queued; nudge not queued");
    else {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "Mount busy");
    }
    return;
  }

  queueAsyncNudge(altDelta, azDelta);

  // Optimistically update the displayed cache so the UI reflects the requested motion.
  if (altAzCacheValid) {
    cachedAlt_deg = clampAlt(cachedAlt_deg + altDelta);
    cachedAz_deg = normalizeAz(cachedAz_deg + azDelta);
    computeRaDecFromAltAz();
  }

  if (wantsAjax()) {
    sendAjaxOK("Nudge queued");
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Nudge queued");
  }
}

void handleGetRaDecWebPage() {
  logHttpRequest("GET_RADEC_WEB");
  if (slewControllerPendingOrRunning()) {
    if (wantsAjax()) sendAjaxFail("Another mount command is already queued; RA/Dec read not queued");
    else {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "Mount busy");
    }
    return;
  }

  queueAsyncRaDecRead();

  if (wantsAjax()) {
    sendAjaxOK("RA/Dec read queued");
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "RA/Dec read queued");
  }
}

void handleGetAltAzWebPage() {
  logHttpRequest("GET_ALTAZ_WEB");
  if (slewControllerPendingOrRunning()) {
    if (wantsAjax()) sendAjaxFail("Another mount command is already queued; Alt/Az read not queued");
    else {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "Mount busy");
    }
    return;
  }

  queueAsyncAltAzRead();

  if (wantsAjax()) {
    sendAjaxOK("Alt/Az read queued");
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Alt/Az read queued");
  }
}

void handleWebGotoRaDecPage() {
  logHttpRequest("WEB_GOTO_RADEC");
  double raHours = server.hasArg("ra_hours") ? server.arg("ra_hours").toFloat() : 0.0;
  double decDeg = server.hasArg("dec_deg") ? server.arg("dec_deg").toFloat() : 0.0;
  double raDeg = normalizeRA(raHours * 15.0);
  LOG_MOUNT_D("Web RA/Dec GOTO request safety: dec=%.3f limitEnabled=%d min=%.3f max=%.3f", decDeg, safeDecLimitEnabled ? 1 : 0, safeDecMinDeg, safeDecMaxDeg);

  String safetyReason;
  if (!allowSlewRaDecBySafety(raDeg, decDeg, safetyReason)) {
    LOG_MOUNT_W("Web RA/Dec GOTO blocked: %s", safetyReason.c_str());
    if (wantsAjax()) sendAjaxFail(safetyReason);
    else server.send(400, "text/plain", safetyReason);
    return;
  }

  if (slewControllerPendingOrRunning()) {
    if (wantsAjax()) sendAjaxFail("Another mount command is already queued; GOTO not queued");
    else {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "Mount busy");
    }
    return;
  }

  // Queue the RA/Dec GOTO and return immediately. Do NOT overwrite cached/current
  // position with the target here; the top banner must show actual/current
  // mount position, not the requested target.
  markPositionDemand("web RA/Dec GOTO");
  queueAsyncSlew(raDeg, decDeg);

  if (wantsAjax()) {
    sendAjaxOK("GOTO RA/Dec queued");
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "GOTO RA/Dec queued");
  }
}

void handleWebGotoAltAzPage() {
  logHttpRequest("WEB_GOTO_ALTAZ");
  double altDeg = server.hasArg("alt_deg") ? server.arg("alt_deg").toFloat() : 0.0;
  double azDeg = server.hasArg("az_deg") ? server.arg("az_deg").toFloat() : 0.0;

  altDeg = clampAlt(altDeg);
  azDeg = normalizeAz(azDeg);

  String safetyReason;
  if (!allowSlewAltAzBySafety(altDeg, azDeg, safetyReason)) {
    LOG_MOUNT_W("Web Alt/Az GOTO blocked: %s", safetyReason.c_str());
    if (wantsAjax()) sendAjaxFail(safetyReason);
    else server.send(400, "text/plain", safetyReason);
    return;
  }

  if (slewControllerPendingOrRunning()) {
    if (wantsAjax()) sendAjaxFail("Another mount command is already queued; Alt/Az GOTO not queued");
    else {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "Mount busy");
    }
    return;
  }

  // Queue the Alt/Az GOTO and return immediately. Do NOT overwrite cached/current
  // Alt/Az with the target here; the top banner must remain actual/current
  // position until the mount reports or the slew completes.
  markPositionDemand("web Alt/Az GOTO");
  queueAsyncAltAzSlew(altDeg, azDeg);

  if (wantsAjax()) {
    sendAjaxOK("GOTO Alt/Az queued");
  } else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "GOTO Alt/Az queued");
  }
}

void handleResetRatesPage() {
  logHttpRequest("RESET_RATES");
  LOG_SET_I("Setup button pressed: Reset Rate Defaults");
  nudgeRateDeg[0] = 0.10;
  nudgeRateDeg[1] = 0.25;
  nudgeRateDeg[2] = 0.50;
  nudgeRateDeg[3] = 1.00;
  lx200CurrentNudgeStepDeg = nudgeRateDeg[1];
  webSelectedRateIndex = 1;
  savePersistentSettings();

  if (wantsAjax()) sendAjaxOK("Rates reset");
  else {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting");
  }
}
