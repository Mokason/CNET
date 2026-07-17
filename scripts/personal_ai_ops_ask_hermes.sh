#!/usr/bin/env bash
# Ask Hermes (non-interactive) for help when the personal AI does not know
# something or ops needs a diagnosis. Stores the reply as a lesson artifact.
#
# Usage:
#   scripts/personal_ai_ops_ask_hermes.sh "question text"
#   echo "question" | scripts/personal_ai_ops_ask_hermes.sh
#
# Env: OPS_HERMES_MAX_TURNS OPS_HERMES_TIMEOUT_SEC OPS_STORE_LESSONS
#      REPO CNET_BASE_PATH (optional context)
set -euo pipefail
REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
# shellcheck source=personal_ai_common.sh
. "$REPO/scripts/personal_ai_common.sh" 2>/dev/null || true
REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"

if [ -f "$REPO/config/personal-ai-ops.env" ]; then
  set -a
  # shellcheck source=/dev/null
  . "$REPO/config/personal-ai-ops.env" || true
  set +a
fi

Q="${*:-}"
if [ -z "$Q" ] && [ ! -t 0 ]; then
  Q=$(cat)
fi
if [ -z "$Q" ]; then
  echo "usage: $0 \"question\"" >&2
  exit 2
fi

MAX_TURNS="${OPS_HERMES_MAX_TURNS:-4}"
TIMEOUT_SEC="${OPS_HERMES_TIMEOUT_SEC:-180}"
STORE="${OPS_STORE_LESSONS:-1}"
LESSON_DIR="$REPO/logs/personal_ai_ops/lessons"
mkdir -p "$LESSON_DIR" "$REPO/logs/personal_ai_ops"

BASE="${BASE_PATH:-${CNET_BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}}"
CTX="You are helping operate a local CNET personal AI.
Base: $BASE
Skills: gap_lane learner, SoulHost/Hermes MCP, closed-set json_toolcall_v0, residual GGUF.
Prefer concrete shell/MCP actions (scripts/personal_ai_auto.sh, jtc-seal, doctor).
Do not invent certified units that are not sealed. If knowledge is missing, say what to teach next.
Question: $Q"

if ! command -v hermes >/dev/null 2>&1; then
  echo "ops_ask_hermes: hermes CLI not found" >&2
  exit 1
fi

OUT="$REPO/logs/personal_ai_ops/hermes_last.txt"
TS=$(date -Iseconds)
set +e
timeout "$TIMEOUT_SEC" hermes chat -q "$CTX" -Q --max-turns "$MAX_TURNS" \
  >"$OUT" 2>"$REPO/logs/personal_ai_ops/hermes_last.err"
rc=$?
set -e

if [ ! -s "$OUT" ]; then
  echo "ops_ask_hermes: empty reply (rc=$rc)" >&2
  cat "$REPO/logs/personal_ai_ops/hermes_last.err" 2>/dev/null | tail -20 >&2 || true
  exit 1
fi

if [ "$STORE" = "1" ]; then
  LESSON="$LESSON_DIR/lesson_$(date +%Y%m%dT%H%M%S).md"
  {
    echo "# Ops lesson $TS"
    echo
    echo "## Question"
    echo "$Q"
    echo
    echo "## Hermes answer"
    cat "$OUT"
  } >"$LESSON"
  echo "ops_ask_hermes: stored $LESSON"
fi

# Surface answer
cat "$OUT"
echo
echo "OPS_ASK_HERMES_OK rc=$rc"
exit 0
