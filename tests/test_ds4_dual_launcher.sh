#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bash "$ROOT/tests/test_ember_dual_launcher.sh"
echo "DS4_DUAL_LAUNCHER_PASS"
