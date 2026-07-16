#pragma once

#include <Arduino.h>
#include <stdint.h>

#if defined(ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S2)
  #ifndef ENABLE_CLASSIC_BT
    #define ENABLE_CLASSIC_BT 1
  #endif
  #define HAS_CLASSIC_BT ENABLE_CLASSIC_BT
#else
  #define HAS_CLASSIC_BT 0
#endif

bool bluetoothClassicAvailable();
bool bluetoothRuntimeIsEnabled();
void setBluetoothRuntimeEnabled(bool enabled);
bool bluetoothClientConnected();
bool bluetoothLX200ClientConnected();

String runtimeBtName();
const char* bluetoothBaseName();
String bluetoothCoexPreferenceText();
int bluetoothCoexPreferenceResult();

bool bluetoothTinyWebRuntimeEnabled();
void setBluetoothTinyWebRuntimeEnabled(bool enabled);
void toggleBluetoothTinyWebRuntimeEnabled();

void setupBluetoothService(bool skipStartup);
void stopBluetoothService();
void handleBluetoothLX200();
void serviceBluetoothClientWifiCoexistence();
void bluetoothSendLX200Response(const String &s);
bool bluetoothProcessLX200Stream();
