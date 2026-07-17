#pragma once

#include <Arduino.h>

uint16_t httpsSetupPort();
bool httpsWebAutoStartEnabled();
String urlEncodeSimple(const String &s);
String cleanGpsReturnUrl(const String &raw);
String gpsReturnUrlWithCacheBuster();
void scheduleHttpsSetupStop(unsigned long delayMs);
void serviceHttpsSetupStop();
void handleStartHttpsPage();
void handleHttpsSetupRedirectPage();
bool requestCameFromApSubnet();
void redirectToHttpsSetup443();
void startHttpsSetupServer();
