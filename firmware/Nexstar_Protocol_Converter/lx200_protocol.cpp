#include "lx200_protocol.h"

#include "logging.h"
#include "nexstar_protocol.h"
#include "observer_time.h"
#include "position_cache.h"
#include "settings.h"

#include <math.h>

unsigned long lx200WifiLastRxMs = 0;
unsigned long lx200WifiLastTxMs = 0;
uint32_t lx200WifiRxCommands = 0;
uint32_t lx200WifiTxReplies = 0;
uint32_t lx200WifiUnhandledCommands = 0;

unsigned long lx200BtLastRxMs = 0;
unsigned long lx200BtLastTxMs = 0;
uint32_t lx200BtRxCommands = 0;
uint32_t lx200BtTxReplies = 0;
uint32_t lx200BtUnhandledCommands = 0;
String lx200BtLastCommand = "";
String lx200BtLastReply = "";
String lx200BtLastUnhandled = "";
bool lx200SuppressNextReplyLog = false;
uint32_t lx200BtGotoStageCommands = 0;
uint32_t lx200BtPollOnlyHintCount = 0;
uint32_t lx200CommonRouterCommands = 0;
unsigned long lx200BtLastCommandHandledMs = 0;

static const size_t LX200_MAX_CMD_LEN = 96;
static double lx200TargetRA_deg = 0.0;
static double lx200TargetDec_deg = 0.0;

extern bool asyncSlewRunning;
extern bool asyncSlewPending;
extern bool asyncAltAzSlewPending;
extern bool asyncNudgePending;
extern bool asyncRaDecReadPending;
extern bool asyncAltAzReadPending;
extern bool gotoCompletionWatchActive;
extern bool gotoCompletionVerifyPending;
extern bool lx200GotoUiActive;
extern uint32_t queuedGotoPositionCacheReplies;
extern uint32_t nudgeGotoQueueRequests;

void lx200Send(uint8_t source, const String &s);
bool queuedGotoOrSlewActive();
void markGotoQueueImmediateAck(const char* protocol, const char* detail);
void markLX200GotoUiStarted(uint8_t source, const char* detail);
void handleLX200StopUiRequest(uint8_t source);
bool mountCommandPathBusy();
bool enqueueGotoRaDec(double raDeg, double decDeg, uint8_t source, const char* reason);
bool enqueueGotoAltAz(double altDeg, double azDeg, uint8_t source, const char* reason);
void clearPendingReadRequestsForLX200Goto(const char* srcName);
void queueAsyncSlew(double raDeg, double decDeg);
void queueAsyncAltAzSlew(double altDeg, double azDeg);
bool hasQueuedGoto();
bool savePersistentSettings();

double parseLX200RA(String cmd) {
  String raw = cmd.substring(3);
  int c1 = raw.indexOf(':');
  int c2 = raw.indexOf(':', c1 + 1);
  if (c1 < 0) return 0.0;

  double h = raw.substring(0, c1).toFloat();
  double m = 0.0;
  double s = 0.0;

  if (c2 >= 0) {
    m = raw.substring(c1 + 1, c2).toFloat();
    s = raw.substring(c2 + 1).toFloat();
  } else {
    m = raw.substring(c1 + 1).toFloat();
  }

  return normalizeRA((h + m / 60.0 + s / 3600.0) * 15.0);
}

double parseLX200Dec(String cmd) {
  String raw = cmd.substring(3);
  raw.replace("*", ":");
  raw.replace("'", ":");
  raw.replace("\"", ":");

  int sign = raw.startsWith("-") ? -1 : 1;
  if (raw.startsWith("+") || raw.startsWith("-")) raw = raw.substring(1);

  int c1 = raw.indexOf(':');
  int c2 = raw.indexOf(':', c1 + 1);

  double d = c1 >= 0 ? raw.substring(0, c1).toFloat() : raw.toFloat();
  double m = c1 >= 0 ? raw.substring(c1 + 1, c2 >= 0 ? c2 : raw.length()).toFloat() : 0.0;
  double s = c2 >= 0 ? raw.substring(c2 + 1).toFloat() : 0.0;

  return sign * (d + m / 60.0 + s / 3600.0);
}

double parseSignedDegMin(String raw) {
  raw.replace("*", ":");
  raw.replace("'", ":");
  int sign = 1;
  if (raw.startsWith("-")) sign = -1;
  if (raw.startsWith("+") || raw.startsWith("-")) raw = raw.substring(1);

  int c = raw.indexOf(':');
  double deg = c >= 0 ? raw.substring(0, c).toFloat() : raw.toFloat();
  double min = c >= 0 ? raw.substring(c + 1).toFloat() : 0.0;
  return sign * (deg + min / 60.0);
}

String lx200SiteLat() {
  return lx200Dec(siteLatitudeDeg);
}

String lx200SiteLong() {
  // LX200/Meade longitudes are west-positive. Internally we use east-positive.
  double lon = -siteLongitudeDeg;
  char sign = lon >= 0 ? '+' : '-';
  double dabs = fabs(lon);
  int totalMinutes = (int)round(dabs * 60.0);
  int deg = totalMinutes / 60;
  int minutes = totalMinutes % 60;

  char buf[18];
  snprintf(buf, sizeof(buf), "%c%03d*%02d#", sign, deg, minutes);
  return String(buf);
}

String lx200Time() {
  unsigned long elapsed = timeValid ? (millis() - timeSetMillis) / 1000 : 0;
  long sec = localHour * 3600L + localMinute * 60L + localSecond + elapsed;
  sec %= 86400L;
  if (sec < 0) sec += 86400L;

  int h = sec / 3600;
  sec %= 3600;
  int m = sec / 60;
  int s = sec % 60;

  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d#", h, m, s);
  return String(buf);
}

String lx200Date() {
  char buf[16];
  int yy = localYear % 100;
  snprintf(buf, sizeof(buf), "%02d/%02d/%02d#", localMonth, localDay, yy);
  return String(buf);
}

String lx200UtcOffset() {
  // LX200 :GG/:SG use hours added to local time to get UTC.
  // Internally utcOffsetMinutes is local-minus-UTC.
  int totalHours = -utcOffsetMinutes / 60;
  char buf[8];
  snprintf(buf, sizeof(buf), "%+03d#", totalHours);
  return String(buf);
}

void lx200MarkRxCommand(uint8_t source, const String &cmd, bool lineTerminated) {
  if (source == LX_SRC_WIFI) {
    lx200WifiLastRxMs = millis();
    lx200WifiRxCommands++;
    if (!lx200SourceIsPollCommand(cmd)) {
      LOG_SKY_D("LX200 WiFi %s command #%lu: %s%s",
                lineTerminated ? "line" : "complete",
                (unsigned long)lx200WifiRxCommands,
                cmd.c_str(),
                lineTerminated ? "" : "#");
    }
  }

  if (source == LX_SRC_BT) {
    lx200BtLastRxMs = millis();
    lx200BtRxCommands++;
    if (!lx200SourceIsPollCommand(cmd)) {
      LOG_SKY_D("LX200 BT %s command #%lu: %s%s",
                lineTerminated ? "line" : "complete",
                (unsigned long)lx200BtRxCommands,
                cmd.c_str(),
                lineTerminated ? "" : "#");
    }
  }
}

void setLX200NudgeRate(const String &cmd) {
  double oldRate = lx200CurrentNudgeStepDeg;

  if (cmd == ":RS") lx200CurrentNudgeStepDeg = nudgeRateDeg[0];
  else if (cmd == ":RM") lx200CurrentNudgeStepDeg = nudgeRateDeg[1];
  else if (cmd == ":RC") lx200CurrentNudgeStepDeg = nudgeRateDeg[2];
  else if (cmd == ":RG") lx200CurrentNudgeStepDeg = nudgeRateDeg[3];

  if (fabs(lx200CurrentNudgeStepDeg - oldRate) > 0.000001) {
    LOG_SKY_I("LX200 nudge rate selected %s -> %.4f deg", cmd.c_str(), lx200CurrentNudgeStepDeg);
  } else {
    LOG_SKY_T("LX200 nudge rate repeat %s -> %.4f deg", cmd.c_str(), lx200CurrentNudgeStepDeg);
  }
}

bool isLX200GotoCommand(const String &cmd) {
  String c = cmd;
  c.trim();
  if (c.endsWith("#")) c.remove(c.length() - 1);
  c.toUpperCase();
  return c == ":MS" || c == ":MS0" || c == ":MS1" || c == ":MA" || c == ":MA0" || c == ":MA1";
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

bool runLX200Nudge(const String &cmd, uint8_t source) {
  double altDelta = 0.0;
  double azDelta = 0.0;

  if (cmd == ":Mn") altDelta = lx200CurrentNudgeStepDeg;
  else if (cmd == ":Ms") altDelta = -lx200CurrentNudgeStepDeg;
  else if (cmd == ":Me") azDelta = lx200CurrentNudgeStepDeg;
  else if (cmd == ":Mw") azDelta = -lx200CurrentNudgeStepDeg;
  else return false;

  LOG_SKY_I("LX200 %s nudge converted to queued Alt/Az GOTO: %s -> Alt delta %.4f, Az delta %.4f",
       lx200SourceName(source), cmd.c_str(), altDelta, azDelta);

  return queueRelativeAltAzGoto(altDelta, azDelta, source, "LX200/SkySafari nudge while mount busy");
}

const char* lx200SourceName(uint8_t source) {
  return source == LX_SRC_BT ? "BT" : "WiFi";
}

bool lx200SourceIsPollCommand(const String &cmd) {
  return cmd == ":GR" || cmd == ":GD" || cmd == ":GA" || cmd == ":GZ";
}

String lx200Ra(double ra) {
  ra = normalizeRA(ra);
  double totalMinutes = (ra / 15.0) * 60.0;
  int hours = ((int)(totalMinutes / 60.0)) % 24;
  double mf = totalMinutes - hours * 60.0;
  int minutes = (int)mf;
  int tenths = (int)round((mf - minutes) * 10.0);

  if (tenths >= 10) {
    tenths = 0;
    minutes++;
  }
  if (minutes >= 60) {
    minutes = 0;
    hours = (hours + 1) % 24;
  }

  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d.%d#", hours, minutes, tenths);
  return String(buf);
}

String lx200Dec(double dec) {
  char sign = dec >= 0 ? '+' : '-';
  double dabs = fabs(dec);
  int totalMinutes = (int)round(dabs * 60.0);
  int deg = totalMinutes / 60;
  int minutes = totalMinutes % 60;

  if (deg > 90) {
    deg = 90;
    minutes = 0;
  }

  char buf[16];
  snprintf(buf, sizeof(buf), "%c%02d*%02d#", sign, deg, minutes);
  return String(buf);
}

String lx200Alt(double alt) {
  return lx200Dec(alt);
}

String lx200Az(double az) {
  az = normalizeAz(az);
  int totalMinutes = (int)round(az * 60.0);
  int deg = totalMinutes / 60;
  int minutes = totalMinutes % 60;
  if (deg >= 360) deg = 0;

  char buf[16];
  snprintf(buf, sizeof(buf), "%03d*%02d#", deg, minutes);
  return String(buf);
}

String readLX200CommandFromStream(Stream &io, uint8_t source, String &buffer) {
  const char* srcName = lx200SourceName(source);

  while (io.available()) {
    char c = (char)io.read();

    if ((uint8_t)c == 0x06 && buffer.length() == 0) {
      if (source == LX_SRC_WIFI) {
        lx200WifiLastRxMs = millis();
        lx200WifiRxCommands++;
        LOG_SKY_D("LX200 WiFi ACK query #%lu", (unsigned long)lx200WifiRxCommands);
      } else if (source == LX_SRC_BT) {
        lx200BtLastRxMs = millis();
        lx200BtRxCommands++;
        LOG_SKY_D("LX200 BT ACK query #%lu", (unsigned long)lx200BtRxCommands);
      }
      return String((char)0x06);
    }

    if (c == '\r' || c == '\n') {
      if (buffer.length() > 0 && buffer.charAt(0) == ':') {
        String cmd = buffer;
        buffer = "";
        cmd.trim();
        if (cmd.endsWith("#")) cmd.remove(cmd.length() - 1);
        lx200MarkRxCommand(source, cmd, true);
        return cmd;
      }
      buffer = "";
      continue;
    }

    buffer += c;

    if (buffer.length() > LX200_MAX_CMD_LEN) {
      LOG_SKY_W("LX200 %s command buffer overflow; clearing partial data: %s", srcName, buffer.c_str());
      buffer = "";
      continue;
    }

    if (c == '#') {
      String cmd = buffer;
      buffer = "";
      cmd.trim();
      if (cmd.endsWith("#")) cmd.remove(cmd.length() - 1);
      lx200MarkRxCommand(source, cmd, false);
      return cmd;
    }
  }

  return "";
}

bool processLX200Stream(Stream &io, uint8_t source) {
  static String wifiBuffer = "";
  static String btBuffer = "";
  String &buffer = (source == LX_SRC_BT) ? btBuffer : wifiBuffer;
  String cmd = readLX200CommandFromStream(io, source, buffer);
  if (cmd.length() == 0) return false;

  lx200CommonRouterCommands++;
  LOG_SKY_T("LX200 common router #%lu source=%s cmd=%s",
            (unsigned long)lx200CommonRouterCommands,
            lx200SourceName(source),
            cmd.c_str());

  // This is the single shared LX200-to-mount command core.
  // WiFi and Bluetooth differ only at the transport read/write layer.
  handleLX200Command(cmd, source);
  return true;
}

void handleLX200Command(const String &rawCmd, uint8_t source) {
  String cmd = rawCmd;
  if (cmd.length() == 0) return;

  const char* srcName = source == LX_SRC_BT ? "BT" : "WiFi";
  int colonPos = cmd.indexOf(':');
  if (colonPos > 0) {
    LOG_SKY_D("LX200 %s command had prefix noise, normalized %s -> %s",
              srcName, cmd.c_str(), cmd.substring(colonPos).c_str());
    cmd = cmd.substring(colonPos);
  }
  if (source == LX_SRC_BT) {
    lx200BtLastCommand = cmd;
    lx200BtLastCommandHandledMs = millis();
  }

  if ((uint8_t)cmd[0] == 0x06) {
    LOG_SKY_D("LX200 %s ACK query", srcName);
    lx200Send(source, "P");
    return;
  }

  bool lx200PollCommand = (cmd == ":GR" || cmd == ":GD" || cmd == ":GA" || cmd == ":GZ");
  if (lx200PollCommand) markPositionDemand(srcName);
  if (!lx200PollCommand) {
    LOG_SKY_I("LX200 %s RX: %s#", srcName, cmd.c_str());
  }

  if (cmd == ":GVP" || cmd == ":GVN" || cmd == ":GVD" || cmd == ":GVT") {
    lx200Send(source, "NexStarBridge#");
    return;
  }

  if (cmd == ":GW") {
    lx200Send(source, "n#");
    return;
  }

  if (cmd == ":D") {
    // LX200 distance-bars query. SkySafari uses this to decide whether the
    // Goto button should remain in Stop mode. Return distance bars only while
    // we are still physically waiting for the original NexStar final '@'.
    // As soon as '@' has arrived, even if post-GOTO E/Z verification is still
    // pending, return a bare # so SkySafari exits Stop mode.
    bool lx200ReportsSlewing = lx200GotoUiActive && gotoCompletionWatchActive;
    const char* reply = lx200ReportsSlewing ? "|#" : "#";
    LOG_SKY_D("LX200 %s :D# slew-status reply: %s  uiActive=%s watchActive=%s verifyPending=%s asyncSlewRunning=%s",
              srcName,
              reply,
              lx200GotoUiActive ? "true" : "false",
              gotoCompletionWatchActive ? "true" : "false",
              gotoCompletionVerifyPending ? "true" : "false",
              asyncSlewRunning ? "true" : "false");
    lx200Send(source, reply);
    return;
  }

  if (cmd == ":Gm" || cmd == ":Gd") {
    lx200Send(source, "00*00#");
    return;
  }

  if (cmd == ":U") return;

  if (cmd == ":RS" || cmd == ":RM" || cmd == ":RC" || cmd == ":RG") {
    setLX200NudgeRate(cmd);
    return;
  }

  if (cmd == ":Mn" || cmd == ":Ms" || cmd == ":Me" || cmd == ":Mw") {
    LOG_SKY_I("LX200 %s nudge command converted to queued Alt/Az GOTO: %s#", srcName, cmd.c_str());
    bool ok = runLX200Nudge(cmd, source);
    if (ok) markGotoQueueImmediateAck(srcName, "LX200 nudge accepted into Alt/Az GOTO queue path");
    if (!ok) LOG_SKY_W("LX200 %s nudge command failed: %s#", srcName, cmd.c_str());
    return;
  }

  if (cmd == ":Q" || cmd.startsWith(":Q")) {
    LOG_SKY_I("LX200 %s stop requested; original NexStar protocol has no abort command; clearing SkySafari UI/queued state only", srcName);
    handleLX200StopUiRequest(source);
    return;
  }

  if (cmd == ":GR" || cmd == ":GD") {
    // Application position polling uses the cached mount-position API only.
    // The real mount is refreshed by serviceMountPolling() on pollIntervalMs.
    // Do not perform E/Z serial transactions from SkySafari/Stellarium polling.
    double apiRa = 0.0;
    double apiDec = 0.0;
    unsigned long ageMs = 0;
    bool ok = mountPositionApiRaDec(apiRa, apiDec, &ageMs);
    if (!ok) {
      LOG_SKY_W("LX200 %s RA/Dec requested but mount-position API cache is not valid; returning safe defaults", srcName);
    }
    if (queuedGotoOrSlewActive()) {
      queuedGotoPositionCacheReplies++;
      LOG_SKY_T("LX200 %s RA/Dec poll answered from cache/API while queued/slewing; age=%lu ms ok=%d",
                srcName, (unsigned long)ageMs, ok ? 1 : 0);
    }

    lx200SuppressNextReplyLog = true;
    if (cmd == ":GR") lx200Send(source, lx200Ra(ok ? apiRa : 0.0));
    else lx200Send(source, lx200Dec(ok ? apiDec : 0.0));
    lx200SuppressNextReplyLog = false;

    return;
  }

  if (cmd == ":GA" || cmd == ":GZ") {
    // Application position polling uses the cached mount-position API only.
    // No direct Z command is sent to the mount from this client path.
    double apiAlt = 0.0;
    double apiAz = 0.0;
    unsigned long ageMs = 0;
    bool ok = mountPositionApiAltAz(apiAlt, apiAz, &ageMs);
    if (!ok) {
      LOG_SKY_W("LX200 %s Alt/Az requested but mount-position API cache is not valid; returning safe defaults", srcName);
    }
    if (queuedGotoOrSlewActive()) {
      queuedGotoPositionCacheReplies++;
      LOG_SKY_T("LX200 %s Alt/Az poll answered from cache/API while queued/slewing; age=%lu ms ok=%d",
                srcName, (unsigned long)ageMs, ok ? 1 : 0);
    }

    lx200SuppressNextReplyLog = true;
    if (cmd == ":GA") lx200Send(source, lx200Alt(ok ? apiAlt : 0.0));
    else lx200Send(source, lx200Az(ok ? apiAz : 0.0));
    lx200SuppressNextReplyLog = false;
    return;
  }

  if (cmd == ":Gt") {
    lx200Send(source, siteValid ? lx200SiteLat() : "+00*00#");
    return;
  }

  if (cmd == ":Gg") {
    lx200Send(source, siteValid ? lx200SiteLong() : "+000*00#");
    return;
  }

  if (cmd == ":GL") {
    lx200Send(source, lx200Time());
    return;
  }

  if (cmd == ":GC") {
    lx200Send(source, lx200Date());
    return;
  }

  if (cmd == ":GG") {
    lx200Send(source, lx200UtcOffset());
    return;
  }

  if (cmd.startsWith(":St")) {
    siteLatitudeDeg = parseSignedDegMin(cmd.substring(3));
    siteValid = true;
    markLocationSource("SkySafari");
    computeAltAzFromRaDec();
    savePersistentSettings();
    LOG_TIME_I("SkySafari/LX200 set latitude %.6f source=SkySafari", siteLatitudeDeg);
    lx200Send(source, "1");
    return;
  }

  if (cmd.startsWith(":Sg")) {
    // LX200/Meade longitudes are west-positive; convert to internal east-positive.
    siteLongitudeDeg = -parseSignedDegMin(cmd.substring(3));
    siteValid = true;
    markLocationSource("SkySafari");
    computeAltAzFromRaDec();
    savePersistentSettings();
    LOG_TIME_I("SkySafari/LX200 set longitude %.6f east-positive source=SkySafari", siteLongitudeDeg);
    lx200Send(source, "1");
    return;
  }

  if (cmd.startsWith(":SG")) {
    String raw = cmd.substring(3);
    // LX200 :SG is hours added to local time to get UTC.
    utcOffsetMinutes = -raw.toInt() * 60;
    utcOffsetSource = "SkySafari/LX200";
    timeValid = true;
    markSkySafariTimeSource();
    computeAltAzFromRaDec();
    savePersistentSettings();
    LOG_TIME_I("SkySafari/LX200 set UTC offset minutes %d source=SkySafari", utcOffsetMinutes);
    lx200Send(source, "1");
    return;
  }

  if (cmd.startsWith(":SL")) {
    String raw = cmd.substring(3);
    int c1 = raw.indexOf(':');
    int c2 = raw.indexOf(':', c1 + 1);
    if (c1 > 0 && c2 > c1) {
      localHour = raw.substring(0, c1).toInt();
      localMinute = raw.substring(c1 + 1, c2).toInt();
      localSecond = raw.substring(c2 + 1).toInt();
      timeSetMillis = millis();
      timeValid = true;
      markSkySafariTimeSource();
      computeAltAzFromRaDec();
      savePersistentSettings();
      LOG_TIME_I("SkySafari/LX200 set local time %02d:%02d:%02d source=SkySafari", localHour, localMinute, localSecond);
      lx200Send(source, "1");
    } else {
      lx200Send(source, "0");
    }
    return;
  }

  if (cmd.startsWith(":SC")) {
    String raw = cmd.substring(3);
    int s1 = raw.indexOf('/');
    int s2 = raw.indexOf('/', s1 + 1);
    if (s1 > 0 && s2 > s1) {
      localMonth = raw.substring(0, s1).toInt();
      localDay = raw.substring(s1 + 1, s2).toInt();
      int yy = raw.substring(s2 + 1).toInt();
      localYear = yy < 80 ? 2000 + yy : 1900 + yy;
      timeSetMillis = millis();
      timeValid = true;
      markSkySafariTimeSource();
      computeAltAzFromRaDec();
      savePersistentSettings();
      LOG_TIME_I("SkySafari/LX200 set date %04d-%02d-%02d source=SkySafari", localYear, localMonth, localDay);
      lx200Send(source, "1");
    } else {
      lx200Send(source, "0");
    }
    return;
  }

  if (cmd.startsWith(":Sr") || cmd.startsWith(":sr")) {
    if (source == LX_SRC_BT) lx200BtGotoStageCommands++;
    lx200TargetRA_deg = parseLX200RA(cmd);
    LOG_SKY_I("LX200 %s target RA staged %.6f deg", srcName, lx200TargetRA_deg);
    lx200Send(source, "1");
    return;
  }

  if (cmd.startsWith(":Sd") || cmd.startsWith(":sd")) {
    if (source == LX_SRC_BT) lx200BtGotoStageCommands++;
    lx200TargetDec_deg = parseLX200Dec(cmd);
    LOG_SKY_I("LX200 %s target Dec staged %.6f deg", srcName, lx200TargetDec_deg);
    lx200Send(source, "1");
    return;
  }

  if (isLX200GotoCommand(cmd)) {
    if (source == LX_SRC_BT) lx200BtGotoStageCommands++;
    LOG_SKY_I("LX200 %s slew command %s to RA=%.6f Dec=%.6f; reply 0 now; command will queue if mount is busy",
              srcName, cmd.c_str(), lx200TargetRA_deg, lx200TargetDec_deg);

    markLX200GotoUiStarted(source, "LX200 :MS#/:MA# accepted");

    if (mountCommandPathBusy()) {
      enqueueGotoRaDec(lx200TargetRA_deg, lx200TargetDec_deg, source, "LX200 GOTO while mount busy");
    } else {
      clearPendingReadRequestsForLX200Goto(srcName);
      queueAsyncSlew(lx200TargetRA_deg, lx200TargetDec_deg);
    }

    // LX200/SkySafari expects an immediate response. We accept the command here;
    // if the mount remains busy too long, serviceGotoQueue() drops it after timeout.
    lx200Send(source, "0");
    markGotoQueueImmediateAck(srcName, hasQueuedGoto() ? "LX200 GOTO accepted into queue" : "LX200 GOTO accepted for async start");
    return;
  }

  if (cmd == ":CM") {
    lx200Send(source, "Coordinates matched.#");
    return;
  }

  if (source == LX_SRC_BT) {
    lx200BtUnhandledCommands++;
    lx200BtLastUnhandled = cmd;
  } else {
    lx200WifiUnhandledCommands++;
  }
  LOG_SKY_W("Unsupported LX200 %s command ignored #%lu: %s#",
       srcName,
       (unsigned long)(source == LX_SRC_BT ? lx200BtUnhandledCommands : lx200WifiUnhandledCommands),
       cmd.c_str());
}
