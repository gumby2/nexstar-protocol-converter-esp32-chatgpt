#include "bluetooth_services.h"

#include "logging.h"
#include "lx200_protocol.h"
#include "network_services.h"
#include "settings.h"
#include "slew_controller.h"

#if HAS_CLASSIC_BT
  #include <BluetoothSerial.h>
  #if defined(ESP32)
    extern "C" {
      #include "esp_coexist.h"
    }
  #endif
#endif

const bool BT_AUTO_WIFI_OFF_ON_CLIENT = false;

bool bluetoothRuntimeEnabled = false;
bool lx200BtClientConnected = false;
bool tinyWebServerRuntimeEnabled = true;
unsigned long btClientConnectedAtMs = 0;
bool btAutoWifiOffDone = false;

String coexPreferenceText = "default";
int coexPreferenceResult = 0;

unsigned long lx200BtLastRxMs = 0;
unsigned long lx200BtLastTxMs = 0;
uint32_t lx200BtRxCommands = 0;
uint32_t lx200BtTxReplies = 0;
uint32_t lx200BtUnhandledCommands = 0;
String lx200BtLastCommand = "";
String lx200BtLastReply = "";
String lx200BtLastUnhandled = "";
uint32_t lx200BtGotoStageCommands = 0;
uint32_t lx200BtPollOnlyHintCount = 0;
unsigned long lx200BtLastCommandHandledMs = 0;

static const char* BT_NAME = "NexStar_Bridge";

#if HAS_CLASSIC_BT
static BluetoothSerial SerialBT;
#endif

extern String appendMacSuffixToName(const String &base);
extern void resetMountPollFailures();
extern void invalidateCachesAfterGoto();

bool bluetoothClassicAvailable() {
  return HAS_CLASSIC_BT != 0;
}

bool bluetoothRuntimeIsEnabled() {
  return bluetoothRuntimeEnabled;
}

void setBluetoothRuntimeEnabled(bool enabled) {
  bluetoothRuntimeEnabled = enabled;
}

bool bluetoothClientConnected() {
#if HAS_CLASSIC_BT
  return bluetoothRuntimeEnabled && SerialBT.hasClient();
#else
  return false;
#endif
}

bool bluetoothLX200ClientConnected() {
  return lx200BtClientConnected;
}

const char* bluetoothBaseName() {
  return BT_NAME;
}

String runtimeBtName() {
  return appendMacSuffixToName(String(BT_NAME));
}

String bluetoothCoexPreferenceText() {
  return coexPreferenceText;
}

int bluetoothCoexPreferenceResult() {
  return coexPreferenceResult;
}

bool bluetoothTinyWebRuntimeEnabled() {
  return tinyWebServerRuntimeEnabled;
}

void setBluetoothTinyWebRuntimeEnabled(bool enabled) {
  tinyWebServerRuntimeEnabled = enabled;
}

void toggleBluetoothTinyWebRuntimeEnabled() {
  tinyWebServerRuntimeEnabled = !tinyWebServerRuntimeEnabled;
}

void setupBluetoothService(bool skipStartup) {
  if (skipStartup) {
    Serial.println("[WEB_ONLY] Bluetooth init skipped");
    return;
  }

#if HAS_CLASSIC_BT
  bool btOk = SerialBT.begin(runtimeBtName(), false, true);
  if (!btOk) {
    Serial.println("Bluetooth SPP first start failed; retrying once");
    SerialBT.end();
    delay(500);
    btOk = SerialBT.begin(runtimeBtName(), false, true);
  }
  if (btOk) {
    bluetoothRuntimeEnabled = true;
    Serial.printf("Bluetooth SPP name: %s\n", runtimeBtName().c_str());
#if defined(ESP32)
    coexPreferenceResult = (int)esp_coex_preference_set(ESP_COEX_PREFER_BT);
    coexPreferenceText = "prefer_bt";
    Serial.printf("[COEX] preference=%s result=%d version=%s\n",
                  coexPreferenceText.c_str(),
                  coexPreferenceResult,
                  esp_coex_version_get());
#endif
  } else {
    Serial.println("Bluetooth SPP failed to start");
  }
#else
  Serial.println("Bluetooth SPP disabled on this board");
#endif
}

void stopBluetoothService() {
#if HAS_CLASSIC_BT
  if (bluetoothRuntimeEnabled) {
    SerialBT.disconnect();
    delay(100);
    SerialBT.end();
    delay(250);
  }
#endif
}

void handleBluetoothLX200() {
#if HAS_CLASSIC_BT
  if (!bluetoothRuntimeEnabled) return;
  bool nowConnected = SerialBT.hasClient();

  if (nowConnected && !lx200BtClientConnected) {
    LOG_BT_I("Bluetooth LX200/SkySafari client connected");
    LOG_BT_D("Bluetooth SkySafari connected: idle background mount polling remains enabled; SkySafari polls use latest cache");
    LOG_BT_I("Bluetooth client active; WiFi/Telnet will remain enabled unless wifi off is requested");
    btClientConnectedAtMs = millis();
    btAutoWifiOffDone = false;
    lx200BtLastRxMs = 0;
    lx200BtLastTxMs = 0;
    lx200BtLastCommand = "";
    lx200BtLastReply = "";
    lx200BtLastUnhandled = "";
    lx200BtGotoStageCommands = 0;
    lx200BtPollOnlyHintCount = 0;
    resetMountPollFailures();
    invalidateCachesAfterGoto();
  } else if (!nowConnected && lx200BtClientConnected) {
    LOG_BT_I("Bluetooth LX200/SkySafari client disconnected");
    btClientConnectedAtMs = 0;
  }

  lx200BtClientConnected = nowConnected;
  if (!nowConnected) return;

  // Match the stable Full WiFi LX200 behavior: process one complete command
  // per loop pass through the same shared LX200-to-mount command core used by WiFi.
  bluetoothProcessLX200Stream();
#endif
}

void serviceBluetoothClientWifiCoexistence() {
#if HAS_CLASSIC_BT
  if (BT_AUTO_WIFI_OFF_ON_CLIENT &&
      bridgeMode == BRIDGE_MODE_BT_MIN_WEB &&
      bluetoothRuntimeEnabled &&
      SerialBT.hasClient() &&
      wifiRuntimeEnabled &&
      !btAutoWifiOffDone &&
      btClientConnectedAtMs != 0 &&
      millis() - btClientConnectedAtMs > 1500UL) {
    btAutoWifiOffDone = true;
    Serial.println("[BT_MIN] Bluetooth client active; turning WiFi off for BT stability");
    runtimeWifiOff();
  }
#endif
}

void bluetoothSendLX200Response(const String &s) {
#if HAS_CLASSIC_BT
  if (SerialBT.hasClient()) {
    SerialBT.print(s);
    SerialBT.flush();
    lx200BtLastTxMs = millis();
    lx200BtTxReplies++;
    lx200BtLastReply = s;
    if (!lx200SuppressNextReplyLog) {
      LOG_SKY_D("LX200 BT TX reply #%lu: %s", (unsigned long)lx200BtTxReplies, s.c_str());
    }
  }
#endif
}

bool bluetoothProcessLX200Stream() {
#if HAS_CLASSIC_BT
  return processLX200Stream(SerialBT, LX_SRC_BT);
#else
  return false;
#endif
}
