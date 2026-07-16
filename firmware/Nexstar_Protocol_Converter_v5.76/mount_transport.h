#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#if defined(ESP8266)
  #define MOUNT_RX_PIN D5
  #define MOUNT_TX_PIN D7
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
  #define MOUNT_RX_PIN 16
  #define MOUNT_TX_PIN 17
#else
  #define MOUNT_RX_PIN 16
  #define MOUNT_TX_PIN 17
#endif

extern const char* BOARD_NAME;

extern const unsigned long MOUNT_BAUD;
extern const unsigned long HANDSHAKE_TIMEOUT_MS;
extern const unsigned long READ_TIMEOUT_MS;
extern const unsigned long GOTO_TIMEOUT_MS;
extern const unsigned long COOLDOWN_MS;
extern const unsigned long AFTER_HANDSHAKE_TX_DELAY_MS;
extern const unsigned long AFTER_COMMAND_TX_DELAY_MS;
extern const unsigned long BEFORE_PAYLOAD_DELAY_MS;

extern unsigned long lastMountResponseMs;
extern unsigned long lastMountFaultMs;
extern bool mountCommFault;
extern String lastMountFault;
extern bool mountBusy;
extern uint8_t backgroundPollFailCount;
extern bool suppressNextMountFault;

void mountTransportBegin();
int mountRawAvailable();
int mountReadRawByte();
void mountWriteRawByte(uint8_t b);
void mountFlush();

void markMountResponse();
void markMountFault(const String &reason);
bool mountAlive();
unsigned long mountLastResponseAge();

bool isPrintableByte(uint8_t b);
void safeDelay(unsigned long ms);

void printRxByte(uint8_t b);
void printTxByte(uint8_t b);
void mountWriteByte(uint8_t b);
void mountWriteBE16(int16_t value);

bool mountReadByte(uint8_t &b, unsigned long timeoutMs);
bool mountReadByteQuiet(uint8_t &b, unsigned long timeoutMs);
bool mountReadExact(uint8_t* buf, size_t len, unsigned long timeoutMs);
void drainMount();

bool beginMountCommand(const char* name);
void endMountCommand(const char* name);
bool nexstarHandshakeLocked(const char* name);
bool waitForNexStarCompletion(const char* name, unsigned long timeoutMs);
