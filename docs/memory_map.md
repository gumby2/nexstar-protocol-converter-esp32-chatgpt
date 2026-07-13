# Memory and storage map

This document is conceptual. Exact sizes and section usage should be taken from current build output, not hard-coded here.

## Compile-time flash data

- Flash application image: the compiled sketch and linked Arduino/ESP32 libraries.
- Huge APP partition scheme: selected by `esp32:esp32:esp32:PartitionScheme=huge_app` to provide a larger application partition.
- Static HTML/CSS/JavaScript strings: web UI pages and scripts embedded in firmware source.
- Bright Star Catalog static data: catalog constants from `bsc5_catalog_data.h`.
- HTTPS certificate/key static data: setup HTTPS credential strings from `https_credentials.h`.

## Runtime RAM

- RAM logging buffer: the circular log buffer and alert text in `logging.cpp`.
- Coordinate caches: current/cached RA/Dec and Alt/Az values, cache validity flags, and timestamps.
- Network client state: Wi-Fi, Bluetooth, TCP, HTTP, Alpaca, Stellarium, Telnet, and UDP discovery client/server objects and buffers.
- GOTO and polling state: queued GOTO fields, async slew flags, completion watcher state, poll scheduler timestamps, and fault counters.

## Persistent NVS

On ESP32, Preferences/NVS stores persistent settings such as site/time/location-related values, polling intervals, Wi-Fi station credentials, AP configuration, bridge mode, ports, Telnet password, Bluetooth-lite options, and GOTO queue timeout.

## Persistent LittleFS

LittleFS is used by non-ESP32 persistence paths and may also be mounted by firmware paths that support filesystem-backed settings or web/setup behavior. Do not treat LittleFS settings layout as interchangeable with ESP32 NVS.

## Generated host-side build output

Host build output includes `.bin`, `.elf`, `.map`, and related files emitted by Arduino CLI. These belong in the configured `OUTPUT_DIR`, normally `dist/`, and the build cache belongs in `BUILD_PATH`, normally a Linux-local directory such as `$HOME/nexstar-build/nexstar-protocol-converter`.

Do not place generated build output under `firmware/Nexstar_Protocol_Converter/build`.
