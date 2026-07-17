#!/usr/bin/env bash
# Seal json_toolcall_v0 into a personal CNB and verify SoulHost serve.
#
# Usage:
#   scripts/json_toolcall_seal.sh [base.cnb] [--force]
#
# Env:
#   STOP_LEARNER=1   (default) stop/start cnet-personal-ai-lane around seal
#   STOP_LEARNER=0   skip stop (unsafe if learner is writing the same CNB)
set -euo pipefail
# shellcheck source=personal_ai_common.sh
. "$(cd "$(dirname "$0")" && pwd)/personal_ai_common.sh"
cnet_load_personal_env

BASE="$(cnet_default_base)"
FORCE=()
STOP_LEARNER="${STOP_LEARNER:-1}"
LANE_UNIT="cnet-personal-ai-lane.service"
was_active=0

for a in "$@"; do
  case "$a" in
    --force) FORCE=(--force) ;;
    -*) echo "usage: $0 [base.cnb] [--force]" >&2; exit 2 ;;
    *) BASE="$a" ;;
  esac
done

info() { echo "jtc_seal: $*"; }
die() { echo "jtc_seal: $*" >&2; exit 1; }

info "base=$BASE"
make -C "$REPO" json_toolcall_seal_cli -j"$(cnet_nproc)"
[ -x "$REPO/bin/json_toolcall_seal" ] || die "bin/json_toolcall_seal missing"

if [ "$STOP_LEARNER" = "1" ] && command -v systemctl >/dev/null 2>&1; then
  if systemctl --user is-active --quiet "$LANE_UNIT" 2>/dev/null; then
    was_active=1
    info "stopping learner (avoid CNB write race)"
    systemctl --user stop "$LANE_UNIT"
    # Wait for process to release file
    for _ in $(seq 1 30); do
      pgrep -f "gap_lane_run.*$(basename "$BASE")" >/dev/null 2>&1 || break
      sleep 0.5
    done
  fi
fi

set +e
"$REPO/bin/json_toolcall_seal" "$BASE" "${FORCE[@]}"
rc=$?
set -e

if [ "$was_active" = "1" ]; then
  info "restarting learner"
  if [ ! -x "$REPO/bin/gap_lane_run" ]; then
    info "rebuilding gap_lane_run (systemd ConditionFileIsExecutable)"
    make -C "$REPO" gap_lane_run_build -j"$(cnet_nproc)" || true
  fi
  systemctl --user start "$LANE_UNIT" || info "WARN: learner restart failed"
fi

[ "$rc" -eq 0 ] || die "seal failed rc=$rc"
info "done — .NET: JsonToolCall.Classify(new SoulHost(\"$BASE\"), json)"
info "MCP: cnet_classify_toolcall / cnet_json_toolcall_status"
if pgrep -x CnetMcpServer >/dev/null 2>&1; then
  info "RECYCLE: Hermes MCP children must restart to load the new unit (serve is otherwise stale)"
  info "  e.g. systemctl --user restart hermes-gateway.service"
fi
echo "JSON_TOOLCALL_SEAL_SH_OK"
