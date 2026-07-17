#pragma once

#include <Arduino.h>

bool decWithinSafeLimits(double decDeg);
bool altWithinSafeLimits(double altDeg);
bool allowSlewRaDecBySafety(double raDeg, double decDeg, String &reason);
bool allowSlewAltAzBySafety(double altDeg, double azDeg, String &reason, double *outRaDeg = nullptr, double *outDecDeg = nullptr);
double sanitizeRateValue(double v, double fallback);

void handleSetAltLimitsPage();
void handleSetDecLimitsPage();
void handleSetRatesPage();
void handleSetWebRatePage();
void handleWebNudgePage();
void handleGetRaDecWebPage();
void handleGetAltAzWebPage();
void handleWebGotoRaDecPage();
void handleWebGotoAltAzPage();
void handleResetRatesPage();
