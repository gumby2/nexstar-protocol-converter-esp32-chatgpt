# Changelog

## v5.87

- Added the hardware-tested Telnet `red` command to toggle a Telnet session between normal and red ANSI text.
- Identified v5.87 as the current tested working baseline.
- Removed obsolete duplicate source copies from the working tree without rewriting Git history.

## v5.86

- Completed the v5.77-v5.81 modular refactor series for slew web handlers, console commands, task diagnostics, HTTPS setup server, and remaining main-program cleanup.
- Restored full Telnet menu/status behavior in Wi-Fi mode while preserving plain-command Telnet behavior in BT mode.
- Aligned web and Telnet status views, improved Telnet status scrolling/refresh behavior, and identified v5.86 as the current hardware-tested working baseline.

## v5.76

- Refactored remaining time/NTP, time-formatting, UTC/local conversion, observer-time, and location utilities into `time_services.h` and `time_services.cpp`.
- Restored the visible Wi-Fi/Web GPS Sync button and identified v5.76 as the current hardware-tested working baseline.

## v5.75

- Completed the working refactor baseline from the current modularized source.
- Preserved existing Bluetooth/SkySafari, Telnet, Wi-Fi, web, mount, and protocol behavior with no intended functional changes.
