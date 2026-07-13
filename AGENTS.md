# AGENTS.md

Mandatory project rules for Codex and other automated agents working in this repository.

## Repository Workflow

- `main` contains stable, tested releases only.
- `develop` contains integrated development work.
- Feature and refactor work occurs on dedicated branches.
- Never commit directly to `main` unless explicitly instructed.
- Do not perform unrelated cleanup or formatting.
- Keep commits narrowly scoped.
- Stop after the requested commit and push.

## Build Requirements

- Primary target is classic ESP32 Dev Module.
- Arduino ESP32 core version is 3.3.10.
- FQBN:
  `esp32:esp32:esp32:PartitionScheme=huge_app`
- Build output and cache must remain outside the firmware source directory.
- Default Arduino CLI path:
  `$HOME/nexstar-portable-dev/arduino-cli`
- Allow `ARDUINO_CLI` to override the CLI path.
- Use a Linux-local build directory for speed.
- Do not create `firmware/Nexstar_Protocol_Converter/build`.

## Required Validation Before Commit

- `python3 tools/browser-test/static_check.py`
- ESP32 compile
- `git diff --check`
- Git status review

## Original NexStar Mount Rules

- The mount is strictly single-command.
- Never accept or forward a new mount command while a previous command is active.
- The `?` handshake is required before applicable commands.
- The mount replies `#` to the handshake.
- `@` marks completion of a command.
- Do not poll `E` or `Z` while a GOTO is active.
- AbortSlew is unsupported.
- During an active GOTO, report slewing and use cached or estimated coordinates.
- Do not introduce position polling during GOTO.
- Preserve big-endian coordinate encoding and existing tested behavior.

## Compatibility Requirements

- Preserve Bluetooth behavior.
- Preserve SkySafari behavior.
- Preserve Wi-Fi, LX200, Alpaca, Stellarium, and web-interface behavior.
- Preserve ESP8266 conditional compilation unless explicitly instructed otherwise.
- Do not rename public endpoints or protocol commands without approval.
- Do not change saved-settings formats without migration handling.

## Firmware-Change Rules

- Return complete implementations, not placeholder code.
- Do not silently remove features.
- Do not change firmware version unless the task explicitly requests a release.
- Avoid broad formatting changes.
- Keep source compatible with Arduino preprocessing.
- Prefer incremental, compile-verified refactors.

## Security Rules

- Do not print or commit credentials, PATs, Wi-Fi passwords, private user data, build caches, or generated binaries unless explicitly intended as release artifacts.
- Do not modify existing certificate behavior in this task.
