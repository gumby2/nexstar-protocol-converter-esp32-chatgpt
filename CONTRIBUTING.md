# Contributing

Thanks for helping improve the NexStar Protocol Converter. Keep changes narrow, behavior-preserving, and easy to validate.

## Branch Model

- `main` contains stable, tested releases only.
- `develop` contains integrated development work.
- `feature/*` branches are for new features.
- `refactor/*` branches are for structure-only or behavior-preserving refactors.

Do not commit directly to `main` unless explicitly instructed.

## Create a Branch

```bash
git checkout develop
git pull
git checkout -b feature/short-description
```

Use `refactor/short-description` for refactoring work.

## Required Tests

Before opening a pull request or committing requested work, run:

```bash
./scripts/test.sh
./scripts/build.sh
git diff --check
```

For the full local check, run:

```bash
./scripts/verify.sh
```

## Compile Locally

The primary local compile target is the classic ESP32 Dev Module with Arduino ESP32 core 3.3.10 and the Huge APP partition scheme.

```bash
arduino-cli core install esp32:esp32@3.3.10
./scripts/build.sh
```

The build script uses:

```text
esp32:esp32:esp32:PartitionScheme=huge_app
```

Generated build directories must not be created inside the source sketch. Do not create or commit `firmware/Nexstar_Protocol_Converter/build`.

Environment overrides:

- `ARDUINO_CLI` sets the Arduino CLI executable path.
- `BUILD_PATH` sets the reusable build cache path.
- `OUTPUT_DIR` sets the compiled artifact output directory.
- `JOBS` sets the compile job count.

## Commit Messages

Use concise imperative commit messages that describe the change:

```text
Add browser static checks
Extract logging subsystem
Fix LX200 coordinate response
```

Keep commits narrowly scoped. Avoid mixing formatting, cleanup, refactors, and behavior changes in the same commit.

## Pull Requests

Pull requests should include:

- A short summary of what changed.
- The validation commands that were run.
- Notes about any behavior or compatibility impact.
- Screenshots or logs only when they clarify user-visible behavior or build failures.

Do not include generated build output, cache directories, binaries, private credentials, or unrelated cleanup.

## Stable Releases

Stable releases are merged to `main` only after validation on the development branch. Versioned release filenames use:

```text
Nexstar_Protocol_Converter_vX.YY.ino
```

The repository working sketch remains:

```text
firmware/Nexstar_Protocol_Converter/Nexstar_Protocol_Converter.ino
```
