#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/.build/esp32}"
OUT_DIR="${OUT_DIR:-$ROOT/dist}"
mkdir -p "$BUILD_DIR" "$OUT_DIR"
arduino-cli compile \
  --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" \
  --build-path "$BUILD_DIR" \
  --output-dir "$OUT_DIR" \
  --jobs "${JOBS:-2}" \
  --warnings default \
  "$ROOT/firmware/Nexstar_Protocol_Converter"
