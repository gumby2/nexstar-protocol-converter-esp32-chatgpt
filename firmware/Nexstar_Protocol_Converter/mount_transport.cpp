#include "mount_transport.h"

#include "logging.h"
#include "settings.h"

#if defined(ESP8266)
  #include <SoftwareSerial.h>
#endif

#if defined(ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S2)
  #ifndef ENABLE_CLASSIC_BT
    #define ENABLE_CLASSIC_BT 1
  #endif
  #define HAS_MOUNT_TRANSPORT_CLASSIC_BT ENABLE_CLASSIC_BT
#else
  #define HAS_MOUNT_TRANSPORT_CLASSIC_BT 0
#endif

#if defined(ESP8266)
  SoftwareSerial MountSerial(MOUNT_RX_PIN, MOUNT_TX_PIN);
  const char* BOARD_NAME = "ESP8266 ESP-12E / D1 mini style";
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
  HardwareSerial MountSerial(1);
  const char* BOARD_NAME = "ESP32-S2";
#else
  HardwareSerial MountSerial(2);
  const char* BOARD_NAME = "Regular ESP32";
#endif

extern const unsigned long MOUNT_BAUD = 9600;
extern const unsigned long HANDSHAKE_TIMEOUT_MS = 3000;
extern const unsigned long READ_TIMEOUT_MS = 3000;
extern const unsigned long GOTO_TIMEOUT_MS = 180000;

extern const unsigned long COOLDOWN_MS = 100;
extern const unsigned long AFTER_HANDSHAKE_TX_DELAY_MS = 50;
extern const unsigned long AFTER_COMMAND_TX_DELAY_MS = 50;
extern const unsigned long BEFORE_PAYLOAD_DELAY_MS = 100;

unsigned long lastMountResponseMs = 0;
unsigned long lastMountFaultMs = 0;
bool mountCommFault = false;
String lastMountFault = "No mount communication yet";
bool mountBusy = false;
bool suppressNextMountFault = false;

void serviceNetworkDuringMountWait();

void mountTransportBegin() {
#if defined(ESP8266)
  MountSerial.begin(MOUNT_BAUD);
#else
  MountSerial.begin(MOUNT_BAUD, SERIAL_8N1, MOUNT_RX_PIN, MOUNT_TX_PIN);
#endif
}

int mountRawAvailable() {
  return MountSerial.available();
}

int mountReadRawByte() {
  return MountSerial.read();
}

void mountWriteRawByte(uint8_t b) {
  MountSerial.write(b);
}

void mountFlush() {
  MountSerial.flush();
}

void markMountResponse() {
  lastMountResponseMs = millis();
  mountCommFault = false;
  lastMountFault = "";
  backgroundPollFailCount = 0;
}

void markMountFault(const String &reason) {
  lastMountFaultMs = millis();
  lastMountFault = reason;

  if (suppressNextMountFault) {
    LOGD("Suppressed transient mount fault: %s", reason.c_str());
    suppressNextMountFault = false;
    return;
  }

  mountCommFault = true;
}

bool mountAlive() {
  return lastMountResponseMs > 0 && !mountCommFault;
}

unsigned long mountLastResponseAge() {
  if (lastMountResponseMs == 0) return 0;
  return millis() - lastMountResponseMs;
}

bool isPrintableByte(uint8_t b) {
  return b >= 32 && b <= 126;
}

void safeDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    delay(1);
    yield();
  }
}

void printRxByte(uint8_t b) {
  LOG_MOUNT_T("RX MOUNT 0x%02X '%c'", b, isPrintableByte(b) ? b : '.');
}

void printTxByte(uint8_t b) {
  LOG_MOUNT_T("TX MOUNT 0x%02X '%c'", b, isPrintableByte(b) ? b : '.');
}

void mountWriteByte(uint8_t b) {
  printTxByte(b);
  MountSerial.write(b);
  MountSerial.flush();
}

void mountWriteBE16(int16_t value) {
  mountWriteByte((uint8_t)((value >> 8) & 0xFF));
  mountWriteByte((uint8_t)(value & 0xFF));
}

bool mountReadByte(uint8_t &b, unsigned long timeoutMs) {
  unsigned long start = millis();

  while (millis() - start < timeoutMs) {
    if (MountSerial.available()) {
      b = MountSerial.read();
      printRxByte(b);
      markMountResponse();
      return true;
    }

    serviceNetworkDuringMountWait();
  }

  String reason = "mountReadByte timeout after " + String(timeoutMs) + " ms";
  LOGW("%s", reason.c_str());
  markMountFault(reason);
  return false;
}

bool mountReadByteQuiet(uint8_t &b, unsigned long timeoutMs) {
  unsigned long start = millis();

  while (millis() - start < timeoutMs) {
    if (MountSerial.available()) {
      b = MountSerial.read();
      printRxByte(b);
      markMountResponse();
      return true;
    }

    serviceNetworkDuringMountWait();
  }

  return false;
}

bool mountReadExact(uint8_t* buf, size_t len, unsigned long timeoutMs) {
  size_t got = 0;
  unsigned long start = millis();

  while (got < len && millis() - start < timeoutMs) {
    if (MountSerial.available()) {
      uint8_t b = MountSerial.read();
      buf[got++] = b;
      printRxByte(b);
      markMountResponse();
      start = millis();
    } else {
      serviceNetworkDuringMountWait();
    }
  }

  if (got != len) {
    String reason = "mountReadExact timeout: expected " + String((unsigned)len) + " got " + String((unsigned)got);
    LOG_MOUNT_W("%s", reason.c_str());
    markMountFault(reason);
    return false;
  }

  return true;
}

void drainMount() {
  int count = 0;
  while (MountSerial.available()) {
    uint8_t b = MountSerial.read();
    LOG_MOUNT_I("[DRAIN] 0x%02X '%c'", b, isPrintableByte(b) ? b : '.');
    markMountResponse();
    count++;
  }
  LOG_MOUNT_I("Drained %d byte(s)", count);
}

bool beginMountCommand(const char* name) {
  if (mountBusy) {
    LOG_MOUNT_D("Blocked mount command '%s': mountBusy=true", name);
    return false;
  }

  mountBusy = true;
  LOG_MOUNT_D("Begin mount command: %s", name);
  return true;
}

void endMountCommand(const char* name) {
  LOG_MOUNT_D("End mount command: %s", name);
  mountBusy = false;
}

bool nexstarHandshakeLocked(const char* name) {
  safeDelay(COOLDOWN_MS);

  LOG_MOUNT_D("%s: sending handshake '?'", name);
  mountWriteByte('?');

  safeDelay(AFTER_HANDSHAKE_TX_DELAY_MS);

  uint8_t resp = 0;
  if (!mountReadByte(resp, HANDSHAKE_TIMEOUT_MS)) {
    String reason = String(name) + ": handshake failed, no '#'";
    if (suppressNextMountFault) LOGW("%s", reason.c_str());
    else LOGE("%s", reason.c_str());
    markMountFault(reason);
    return false;
  }

  if (resp != '#') {
    String reason = String(name) + ": handshake failed, expected '#', got 0x" + String(resp, HEX);
    if (suppressNextMountFault) LOGW("%s", reason.c_str());
    else LOGE("%s", reason.c_str());
    markMountFault(reason);
    return false;
  }

  LOG_MOUNT_D("%s: handshake OK", name);
  markMountResponse();
  safeDelay(COOLDOWN_MS);
  return true;
}

bool waitForNexStarCompletion(const char* name, unsigned long timeoutMs) {
  unsigned long start = millis();
  LOG_MOUNT_D("%s: waiting for NexStar completion '@'", name);

  while (millis() - start < timeoutMs) {
    uint8_t b = 0;
    if (mountReadByteQuiet(b, 250)) {
      if (b == '@') {
        LOG_MOUNT_I("%s: completion '@' received", name);
        markMountResponse();
        return true;
      }
      LOG_MOUNT_W("%s: ignored byte while waiting for '@': 0x%02X '%c'",
                  name, b, isPrintableByte(b) ? b : '.');
    }

#if HAS_MOUNT_TRANSPORT_CLASSIC_BT
    // Keep the Bluetooth stack alive, but do not process new SkySafari commands
    // while the original NexStar mount is completing this single-command move.
    if (bridgeMode == BRIDGE_MODE_BT_MIN_WEB) {
      delay(1);
      yield();
    } else
#endif
    {
      serviceNetworkDuringMountWait();
    }
  }

  String reason = String(name) + ": timed out waiting for completion '@'";
  LOG_MOUNT_W("%s", reason.c_str());
  markMountFault(reason);
  return false;
}
