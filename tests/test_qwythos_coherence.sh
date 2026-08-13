#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
RESULTS="${1:-logs/qwythos_cnet_gpu1_results.jsonl}"
REPORT="${2:-logs/qwythos_cnet_gpu1_coherence_report.json}"

[[ -x bin/score_cnet_coherence ]] || make -s score_cnet_coherence
./bin/score_cnet_coherence "$RESULTS" --report "$REPORT"
jq -e '
  .samples == 3 and .fully_passed == 3 and .verdict == "COHERENT"
' "$REPORT" >/dev/null
echo "QWYTHOS_COHERENCE_PASS samples=3"
