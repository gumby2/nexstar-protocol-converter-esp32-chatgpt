#include "logging.h"

int LOG_LEVEL = LOG_WARN;
uint16_t LOG_SUBSYSTEM_MASK = LOG_CAT_ALL;

bool logAlertActive = false;
String logAlertText = "";
unsigned long logAlertMs = 0;

const int LOG_BUFFER_LINES = 100;
String logBuffer[LOG_BUFFER_LINES];
int logWriteIndex = 0;
bool logWrapped = false;
