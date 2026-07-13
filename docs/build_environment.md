# Build environment

The primary build target is a classic ESP32 Dev Module using Arduino ESP32 core 3.3.10.

## Host workflow

- Windows 11 host.
- WSL Ubuntu shell for normal build and validation commands.
- Example repository path in WSL: `/mnt/c/Users/scott/Documents/nexstar-protocol-converter-esp32-chatgpt/repository/nexstar-protocol-converter`.
- Portable Arduino CLI default: `$HOME/nexstar-portable-dev/arduino-cli`.
- FQBN: `esp32:esp32:esp32:PartitionScheme=huge_app`.

## Scripts

Run from the repository root:

```sh
./scripts/test.sh
./scripts/build.sh
./scripts/verify.sh
```

`scripts/test.sh` runs the browser static check. `scripts/build.sh` compiles the ESP32 firmware. `scripts/verify.sh` runs both scripts, then `git diff --check` and `git status --short`.

## Build overrides

`scripts/build.sh` accepts these environment overrides:

```sh
ARDUINO_CLI="$HOME/nexstar-portable-dev/arduino-cli" ./scripts/build.sh
BUILD_PATH="$HOME/nexstar-build/nexstar-protocol-converter" ./scripts/build.sh
OUTPUT_DIR="$PWD/dist" ./scripts/build.sh
JOBS=2 ./scripts/build.sh
```

Keep build output and cache outside the firmware source directory. Use a Linux-local `BUILD_PATH` for speed. Release-style generated artifacts go in `dist/`. Do not create or rely on `firmware/Nexstar_Protocol_Converter/build`.

## CI

GitHub Actions runs browser static checks and ESP32 compile checks for pushed work. Before starting a branch, confirm the relevant base or preceding protocol commit has green CI.

## Branch workflow

- `main` contains stable releases only.
- `develop` contains integrated development work.
- Feature and refactor work belongs on dedicated branches.
- Do not edit `main` or `develop` unless explicitly instructed.
- Keep commits narrowly scoped and avoid unrelated cleanup.

## Recovery commands

Wrong branch:

```sh
git status --short --branch
git switch refactor/modularize-firmware
```

Dirty tree:

```sh
git status --short
git diff --stat
```

Generated build folder inside firmware source:

```sh
git status --short --ignored firmware/Nexstar_Protocol_Converter/build
```

Failed compile log capture:

```sh
./scripts/build.sh 2>&1 | tee build.log
grep -i "error:" build.log
```
