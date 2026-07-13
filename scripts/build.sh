#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

ARDUINO_CLI="${ARDUINO_CLI:-$HOME/nexstar-portable-dev/arduino-cli}"
BUILD_PATH="${BUILD_PATH:-$HOME/nexstar-build/nexstar-protocol-converter}"
OUTPUT_DIR="${OUTPUT_DIR:-$ROOT/dist}"
JOBS="${JOBS:-2}"
SKETCH_DIR="$ROOT/firmware/Nexstar_Protocol_Converter"
FQBN="esp32:esp32:esp32:PartitionScheme=huge_app"

if [ ! -x "$ARDUINO_CLI" ]; then
  echo "error: Arduino CLI is not executable: $ARDUINO_CLI" >&2
  echo "Set ARDUINO_CLI=/path/to/arduino-cli to override the default." >&2
  exit 1
fi

mkdir -p "$BUILD_PATH" "$OUTPUT_DIR"

find "$OUTPUT_DIR" -maxdepth 1 -type f \
  \( -name '*.bin' -o -name '*.elf' -o -name '*.hex' -o -name '*.map' -o -name '*.eep' \) \
  -exec rm -f {} +

echo "Compiling ESP32 firmware..."
"$ARDUINO_CLI" compile \
  --fqbn "$FQBN" \
  --build-path "$BUILD_PATH" \
  --output-dir "$OUTPUT_DIR" \
  --jobs "$JOBS" \
  --warnings default \
  "$SKETCH_DIR"

echo "Build succeeded."
echo "Output directory: $OUTPUT_DIR"
