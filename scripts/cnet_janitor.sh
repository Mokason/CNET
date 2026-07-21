#!/usr/bin/env bash
# CNET G1 library janitor — safe to run from systemd/cron.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

BASE="${CNET_JANITOR_BASE:-${CNET_BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}}"
export CNET_JANITOR_REPORT_DIR="${CNET_JANITOR_REPORT_DIR:-$REPO/artifacts/janitor}"
export CNET_JANITOR_LOW_REL="${CNET_JANITOR_LOW_REL:-0.55}"
export CNET_JANITOR_LOW_REL_MIN_EV="${CNET_JANITOR_LOW_REL_MIN_EV:-64}"
# Default: note low-rel into ledger so gap lane can rebuild later
export CNET_JANITOR_NOTE_LOW_REL="${CNET_JANITOR_NOTE_LOW_REL:-1}"
# Snapshots are opt-in (large CNB)
export CNET_JANITOR_SNAPSHOT="${CNET_JANITOR_SNAPSHOT:-0}"

if [[ ! -x "$REPO/bin/cnet_janitor" ]]; then
  make -s cnet_janitor_build
fi

# Don't fight a stop file or active exclusive ops
if [[ -f "${BASE}.stop" ]]; then
  echo "cnet_janitor.sh: skip — ${BASE}.stop present"
  exit 0
fi

mkdir -p logs "$CNET_JANITOR_REPORT_DIR"
./bin/cnet_janitor "$BASE" | tee -a logs/janitor_cron.log
grep -q JANITOR_OK logs/janitor_cron.log || true
# Keep last marker line easy to find
tail -n 5 logs/janitor_cron.log
