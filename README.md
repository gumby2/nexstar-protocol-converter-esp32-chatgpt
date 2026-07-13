# NexStar Protocol Converter

ESP32 firmware for bridging and translating telescope-control protocols for an original Celestron NexStar mount.

## Current baseline

- Firmware: **v5.29**
- Target: **ESP32 Dev Module**
- ESP32 Arduino core: **3.3.10**
- Partition scheme: **Huge APP**
- UART2: RX GPIO 16, TX GPIO 17

## Build

```bash
arduino-cli core install esp32:esp32@3.3.10
./tools/compile-linux.sh
```

Windows PowerShell:

```powershell
./tools/compile-windows.ps1
```

## Development workflow

Run the static checks:

```bash
./scripts/test.sh
```

Compile the ESP32 firmware:

```bash
./scripts/build.sh
```

Run the full local verification:

```bash
./scripts/verify.sh
```

The development scripts support these environment overrides:

- `ARDUINO_CLI`
- `BUILD_PATH`
- `OUTPUT_DIR`
- `JOBS`

## Important mount constraints

The original NexStar mount is strictly single-command. No new command may be accepted or forwarded while an earlier mount command is active. A completed command returns `@`. Position polling must not occur during an active GOTO, and AbortSlew is unsupported.

## Repository layout

- `firmware/Nexstar_Protocol_Converter/` — Arduino-compatible stable sketch path
- `releases/` — versioned source snapshots
- `tools/` — local compile and browser-test utilities
- `.github/workflows/` — automated ESP32 build and browser checks
- `docs/` — user and protocol documentation
- `hardware/` — wiring notes and diagrams
