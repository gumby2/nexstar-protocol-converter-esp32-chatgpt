#pragma once

#include <Arduino.h>
#include <stdint.h>

String consoleLogMaskNames(uint16_t mask);
void printConsoleLogStatus();
uint16_t consoleLogMaskFromList(String cats);
void printHelpLog();
void printHelpMode();
void printHelpWifi();
void printHelpMount();
void printHelpStatus();
void printHelpTopic(const String &topicRaw);
void printHelp();
void printConsoleStatus();

bool consoleConfirmYes(const String &cmd);
bool consoleConfirmNo(const String &cmd);
bool commandRequiresDisconnectConfirm(const String &cmd, bool telnetSession, String &description);
void printCommandConfirmPrompt(Print &out, const String &line, const String &description);
void printMountPollValue(Print &out);
bool setMountPollValue(long valueMs);
String formatAgeSeconds(unsigned long timestampMs);

void printConsoleHealth(Print &out);
void handleConsole();
