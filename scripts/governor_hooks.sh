#!/usr/bin/env bash
# Pre/post hooks invoked by native governor (and quality tests).
set -euo pipefail
ROOT="${CNET_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
export CNET_ROOT="$ROOT"
export CNET_GOVERNOR_DIR="${CNET_GOVERNOR_DIR:-$ROOT/logs/governor}"
export CNET_BASE_PATH="${CNET_BASE_PATH:-$ROOT/soul_gemma4v2_final.cnb}"
export CNET_FAULT_LOG="${CNET_FAULT_LOG:-$ROOT/logs/cnet_faults.jsonl}"
mkdir -p "$CNET_GOVERNOR_DIR"
chmod +x "$ROOT/scripts/governor_"*.sh 2>/dev/null || true

cmd="${1:-pre}"
case "$cmd" in
  pre)
    bash "$ROOT/scripts/governor_resource_snap.sh" || true
    bash "$ROOT/scripts/governor_miss_ingest.sh" || true
    ;;
  eval)
    bash "$ROOT/scripts/governor_eval_probe.sh" || true
    ;;
  web)
    bash "$ROOT/scripts/governor_safe_web.sh"
    ;;
  research)
    if [[ -x "$ROOT/scripts/queue_research_skills.sh" ]]; then
      bash "$ROOT/scripts/queue_research_skills.sh" || true
    else
      bash "$ROOT/scripts/procedure_chunk_seal.sh" || true
    fi
    ;;
  *)
    echo "usage: $0 pre|eval|web|research" >&2
    exit 2
    ;;
esac
