#!/usr/bin/env bash
# Weekly CNET governance brief from janitor LATEST + lane status.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
OUT="${1:-artifacts/janitor/WEEKLY_BRIEF.md}"
mkdir -p "$(dirname "$OUT")"
{
  echo "# CNET weekly governance brief"
  echo
  echo "- Generated: $(date -Iseconds)"
  echo "- Host: $(hostname)"
  echo
  echo "## Services"
  echo "- personal-ai-lane: $(systemctl --user is-active cnet-personal-ai-lane.service 2>/dev/null || echo n/a)"
  echo "- janitor timer: $(systemctl --user is-enabled cnet-janitor.timer 2>/dev/null || echo n/a)"
  echo
  echo "## Base"
  if [[ -f soul_gemma4v2_final.cnb ]]; then
    ls -lh soul_gemma4v2_final.cnb | awk '{print "- size:", $5, "mtime:", $6, $7, $8}'
  fi
  echo
  echo "## Latest janitor"
  if [[ -f artifacts/janitor/LATEST.md ]]; then
    sed -n '1,80p' artifacts/janitor/LATEST.md
  else
    echo "_(no LATEST.md — run janitor)_"
  fi
  echo
  echo "## Pins"
  if [[ -d artifacts/janitor/pins ]]; then
    ls -lh artifacts/janitor/pins/pin_*.cnb 2>/dev/null | tail -5 || echo "_(none)_"
    [[ -f artifacts/janitor/pins/CURRENT ]] && echo "CURRENT: $(cat artifacts/janitor/pins/CURRENT)"
  fi
  echo
  echo "## Charter"
  echo "- enforce: ${CNET_CHARTER_ENFORCE:-0}"
  echo "- path: ${CNET_CHARTER_PATH:-config/actuator_charter.txt}"
} > "$OUT"
echo "BRIEF_OK $OUT"
# Optional Discord home via hermes is left to cron deliver
