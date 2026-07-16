#pragma once

#include <Arduino.h>

String formatUptime();
String resetReasonText();
String basicSystemHealthText();
String systemHealthText();
String taskStatsSectionText(bool basicMode);
String sampleWebCpuLoadText();
String sampleBannerSystemText();
String taskRefreshText();
String monitorRefreshText();
void printTaskRuntimeStats(Print &out);
void telnetDrawMonitor(Print &out);
void telnetDrawTasks(Print &out);
