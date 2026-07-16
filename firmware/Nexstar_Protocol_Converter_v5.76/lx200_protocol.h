#pragma once

#include <Arduino.h>
#include <stdint.h>

enum LX200Source {
  LX_SRC_WIFI = 0,
  LX_SRC_BT   = 1
};

extern unsigned long lx200WifiLastRxMs;
extern unsigned long lx200WifiLastTxMs;
extern uint32_t lx200WifiRxCommands;
extern uint32_t lx200WifiTxReplies;
extern uint32_t lx200WifiUnhandledCommands;

extern unsigned long lx200BtLastRxMs;
extern unsigned long lx200BtLastTxMs;
extern uint32_t lx200BtRxCommands;
extern uint32_t lx200BtTxReplies;
extern uint32_t lx200BtUnhandledCommands;
extern String lx200BtLastCommand;
extern String lx200BtLastReply;
extern String lx200BtLastUnhandled;
extern bool lx200SuppressNextReplyLog;
extern uint32_t lx200BtGotoStageCommands;
extern uint32_t lx200BtPollOnlyHintCount;
extern uint32_t lx200CommonRouterCommands;
extern unsigned long lx200BtLastCommandHandledMs;

const char* lx200SourceName(uint8_t source);
bool lx200SourceIsPollCommand(const String &cmd);
String readLX200CommandFromStream(Stream &io, uint8_t source, String &buffer);
bool processLX200Stream(Stream &io, uint8_t source);
void handleLX200Command(const String &rawCmd, uint8_t source);
bool queueRelativeAltAzGoto(double altDelta, double azDelta, uint8_t source, const char* reason);

String lx200Ra(double ra);
String lx200Dec(double dec);
String lx200Alt(double alt);
String lx200Az(double az);
