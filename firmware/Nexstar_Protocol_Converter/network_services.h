#pragma once

#include <Arduino.h>

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  #include <WiFiUdp.h>
#else
  #include <WiFi.h>
  #include <WebServer.h>
  #include <WiFiUdp.h>
#endif

#if defined(ESP8266)
extern ESP8266WebServer *webServer;
extern ESP8266WebServer *alpacaServer;
extern ESP8266WebServer *activeServer;
#else
extern WebServer *webServer;
extern WebServer *alpacaServer;
extern WebServer *activeServer;
#endif
#define server (*activeServer)

extern WiFiClient lx200Client;
extern bool lx200WifiClientConnected;
extern WiFiClient stellariumClient;
extern bool stellariumClientConnected;
extern unsigned long stellariumLastRxMs;
extern unsigned long stellariumLastTxMs;
extern uint32_t stellariumRxPackets;
extern uint32_t stellariumTxPackets;
extern uint32_t stellariumBadPackets;

extern WiFiClient telnetClient;
extern WiFiServer tinySetupServer;
extern bool telnetClientConnected;
extern bool telnetAuthenticated;
extern String telnetLine;
extern bool telnetLastWasCR;
extern unsigned long telnetLastRxMs;
extern uint32_t telnetRxCommands;
extern uint32_t telnetAuthFailures;
extern bool telnetLiveLogEnabled;
extern uint32_t telnetLiveLogLines;
extern String telnetPendingConfirmCommand;
extern String telnetPendingConfirmDescription;
extern bool telnetMonitorActive;
extern bool telnetMonitorSavedLiveLog;
extern unsigned long telnetMonitorLastDrawMs;
extern unsigned long telnetMonitorRefreshMs;
extern bool telnetTasksActive;
extern bool telnetTasksSavedLiveLog;
extern unsigned long telnetTasksLastDrawMs;
extern unsigned long telnetTasksRefreshMs;
extern unsigned long telnetTasksLastIdle0;
extern unsigned long telnetTasksLastIdle1;
extern unsigned long telnetTasksLastRuntimeTotal;
extern unsigned long telnetTasksLastLoadMs;
extern int telnetTasksCpuLoadPct;

extern bool restartPending;
extern unsigned long restartAtMs;
extern bool apRestartPending;
extern unsigned long apRestartAtMs;
extern bool staConnected;
extern bool apRunning;
extern bool tinyWebServerRuntimeEnabled;
extern bool wifiRuntimeEnabled;
extern const bool BT_AUTO_WIFI_OFF_ON_CLIENT;
extern unsigned long btClientConnectedAtMs;
extern bool btAutoWifiOffDone;
extern String wifiModeText;
extern String lastWifiStatus;
extern String startupModeSource;
extern int startupModePinUsed;

extern const uint16_t HTTP_WEB_PORT;
extern const uint16_t ALPACA_DISCOVERY_PORT;

void serviceHttpServers();
void serviceNetworkClients();
void serviceNetworkDuringMountWait();
void allocateNetworkServiceObjects();
void startFullNetworkListeners();
void startTinySetupServer();
void startTelnetConsoleServer(const char* reason);
void serviceTelnetConsole();
void serviceRestart();
void scheduleRestart(const String &reason);
void scheduleApRestart(const String &reason);
void runtimeWifiOff();
void runtimeWifiOn();

void startFallbackAP();
bool connectConfiguredSTA();
void setupWiFiFromSavedConfig();
bool parseIpAddress(const String &s, IPAddress &ip);
String wifiStatusCodeText(wl_status_t st);
const char* bridgeModeName();
void applyGpioStartupModeOverride();
String gpioStartupModeText();

void handleAlpacaDiscovery();
void handleLX200Server();
void handleStellariumServer();

bool lx200WifiClientActive();
bool stellariumClientActive();
String apIpAddressText();
String staIpAddressText();
int apClientCount();
