#pragma once

#include <Arduino.h>
#include <stdint.h>

double normalizeRA(double ra);
double normalizeAz(double az);
double clampAlt(double alt);

double numberToAngle(int16_t number);
int16_t angleToNumber(double angleDeg);

bool testInit();
bool getRaDec(double &ra, double &dec, bool forcePoll = false);
bool getAltAzFromMount(double &alt, double &az);
bool getAltAz(double &alt, double &az, bool forceMountPoll = false);
bool gotoRaDec(double ra, double dec);
bool gotoRaDecAndWait(double ra, double dec, const char* reasonName = nullptr);
bool gotoAltAz(double alt, double az);
bool gotoAltAzAndWait(double alt, double az, const char* reasonName = nullptr);
