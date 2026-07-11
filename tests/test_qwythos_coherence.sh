#!/usr/bin/env bash
set -euo pipefail

RESULTS="${1:-logs/qwythos_cnet_gpu1_results.jsonl}"
REPORT="${2:-logs/qwythos_cnet_gpu1_coherence_report.json}"

python3 tools/score_cnet_coherence.py "$RESULTS" --report "$REPORT"
python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); assert r["samples"] == 3, r; assert r["fully_passed"] == 3, r; assert r["verdict"] == "COHERENT", r; print("QWYTHOS_COHERENCE_PASS samples=3")' "$REPORT"
