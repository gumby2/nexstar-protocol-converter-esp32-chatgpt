#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

"$ROOT/scripts/test.sh"
"$ROOT/scripts/build.sh"
git -C "$ROOT" diff --check
git -C "$ROOT" status --short

echo "Verification succeeded."
