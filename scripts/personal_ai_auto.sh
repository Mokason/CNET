#!/usr/bin/env bash
# Make Personal AI automatic: one command to prepare, install, start, status.
#
# Automatic loop (no babysitter):
#   [Serve]  Hermes MCP / SoulHost  — local units first; misses → GAP_INBOX
#            + periodic health tick
#   [Learn]  cnet-personal-ai-lane  — inbox → teach from local teacher GGUF
#            → seal CNB; teacher sleeps when idle
#
# Usage:
#   scripts/personal_ai_auto.sh prepare   # build binaries, check paths
#   scripts/personal_ai_auto.sh install   # install user systemd units
#   scripts/personal_ai_auto.sh start     # enable --now learner (+ optional serve)
#   scripts/personal_ai_auto.sh stop
#   scripts/personal_ai_auto.sh status
#   scripts/personal_ai_auto.sh doctor    # print what is wired / missing
#
# Env overrides:
#   BASE_PATH=.../soul.cnb
#   TEACHER=.../model.gguf          # empty = maintenance-only lane (no teach)
#   SERVE=1                         # also run deploy_hermes_mcp.sh on start
#   TICK_SECONDS=60                 # MCP health tick
set -euo pipefail

# shellcheck source=personal_ai_common.sh
. "$(cd "$(dirname "$0")" && pwd)/personal_ai_common.sh"
cnet_load_personal_env
BASE="$(cnet_default_base)"
TEACHER="${TEACHER:-${CNET_PERSONAL_TEACHER:-/home/marble/AI/Models/gemma4-v2-Q4_K_M.gguf}}"
# Optional Tier C residual GGUF (open-ended serve); empty = hermetic/soft only
RESIDUAL="${RESIDUAL:-${CNET_RESIDUAL_GGUF:-}}"
RESIDUAL_WINDOW="${RESIDUAL_WINDOW:-${CNET_RESIDUAL_WINDOW:-$REPO/english_window_256.txt}}"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
LANE_UNIT="cnet-personal-ai-lane.service"
TARGET_UNIT="cnet-personal-ai.target"
SERVE="${SERVE:-0}"
TICK="${TICK_SECONDS:-60}"

cmd="${1:-}"

die() { echo "personal_ai_auto: $*" >&2; exit 1; }
info() { echo "personal_ai_auto: $*"; }

prepare() {
  info "build learner + personal_ai gate + placement CLI + ops tools"
  make -C "$REPO" gap_lane_run_build personal_ai cnet_plan_cli cnb_audit serve_proof_cli \
    -j"$(cnet_nproc)"
  [ -x "$REPO/bin/gap_lane_run" ] || die "gap_lane_run missing"
  [ -f "$BASE" ] || info "WARN: base not found yet: $BASE (serve/learn need it)"
  if [ -n "$TEACHER" ] && [ ! -f "$TEACHER" ]; then
    info "WARN: teacher not found: $TEACHER (lane will run maintenance-only)"
  fi
  if [ -n "$RESIDUAL" ]; then
    if [ -f "$RESIDUAL" ]; then
      info "Tier C residual GGUF: $RESIDUAL"
      [ -f "$RESIDUAL_WINDOW" ] || info "WARN: residual window missing: $RESIDUAL_WINDOW (synth window)"
    else
      info "WARN: CNET_RESIDUAL_GGUF not found: $RESIDUAL"
    fi
  else
    info "Tier C residual: unset (bind later via CNET_RESIDUAL_GGUF)"
  fi
  if [ -x "$REPO/bin/cnet_plan" ]; then
    info "placement doctor:"
    CNET_BASE_PATH="$BASE" CNET_PERSONAL_TEACHER="$TEACHER" \
      CNET_RESIDUAL_GGUF="${RESIDUAL:-}" \
      "$REPO/bin/cnet_plan" doctor || info "WARN: doctor NEEDS_ATTENTION (see dual_safe)"
  fi
  # Ensure inbox exists so serve can append before lane starts
  if [ -f "$BASE" ]; then
    : >>"${BASE}.inbox"
    units=$(cnet_unit_count_fast "$BASE")
    [ -n "$units" ] && info "sealed units=$units"
  fi
  info "PERSONAL_AI_AUTO_PREPARE_OK"
}

install_units() {
  prepare
  mkdir -p "$UNIT_DIR"
  # Materialize unit with resolved paths from this host.
  local src_lane="$REPO/config/cnet-personal-ai-lane.service"
  local src_target="$REPO/config/cnet-personal-ai.target"
  [ -f "$src_lane" ] || die "missing $src_lane"
  sed \
    -e "s|/home/marble/AI/CNET|$REPO|g" \
    -e "s|Environment=CNET_BASE_PATH=.*|Environment=CNET_BASE_PATH=$BASE|g" \
    -e "s|Environment=CNET_GAP_INBOX=.*|Environment=CNET_GAP_INBOX=${BASE}.inbox|g" \
    -e "s|Environment=CNET_PERSONAL_TEACHER=.*|Environment=CNET_PERSONAL_TEACHER=$TEACHER|g" \
    "$src_lane" >"$UNIT_DIR/$LANE_UNIT"
  # Residual env primarily from personal-ai.env (EnvironmentFile). Optionally pin
  # in [Service] when RESIDUAL is set for this install (must be before [Install]).
  if [ -n "$RESIDUAL" ] && ! grep -q '^Environment=CNET_RESIDUAL_GGUF=' "$UNIT_DIR/$LANE_UNIT"; then
    awk -v r="$RESIDUAL" -v w="$RESIDUAL_WINDOW" '
      /^\[Install\]/ && !done {
        print "# Tier C residual (personal_ai path; gap_lane_run ignores)"
        print "Environment=CNET_RESIDUAL_GGUF=" r
        print "Environment=CNET_RESIDUAL_WINDOW=" w
        print ""
        done=1
      }
      { print }
    ' "$UNIT_DIR/$LANE_UNIT" >"$UNIT_DIR/$LANE_UNIT.tmp" && \
      mv "$UNIT_DIR/$LANE_UNIT.tmp" "$UNIT_DIR/$LANE_UNIT"
  fi
  sed "s|/home/marble/AI/CNET|$REPO|g" "$src_target" >"$UNIT_DIR/$TARGET_UNIT"
  systemctl --user daemon-reload
  info "installed $UNIT_DIR/$LANE_UNIT"
  if [ -n "$RESIDUAL" ]; then
    info "Tier C residual env pinned in [Service] for personal_ai consumers"
  fi
  info "PERSONAL_AI_AUTO_INSTALL_OK"
}

start() {
  install_units
  systemctl --user enable --now "$LANE_UNIT"
  if [ "$SERVE" = "1" ]; then
    if [ -x "$REPO/scripts/deploy_hermes_mcp.sh" ] && [ -f "$BASE" ]; then
      info "deploying serve side (Hermes MCP) with inbox + health tick"
      BASE_PATH="$BASE" TICK_SECONDS="$TICK" \
        bash "$REPO/scripts/deploy_hermes_mcp.sh" || \
        info "WARN: hermes deploy failed (learner still running)"
    else
      info "WARN: SERVE=1 but hermes deploy or base missing — learner only"
    fi
  fi
  info "PERSONAL_AI_AUTO_START_OK"
  status
}

stop() {
  systemctl --user stop "$LANE_UNIT" 2>/dev/null || true
  systemctl --user disable "$LANE_UNIT" 2>/dev/null || true
  info "PERSONAL_AI_AUTO_STOP_OK"
}

status() {
  # Residual already resolved via cnet_load_personal_env + env overrides (no re-grep).
  local res_show="$RESIDUAL"
  local win_show="$RESIDUAL_WINDOW"
  echo "=== Personal AI automation status ==="
  echo "repo:    $REPO"
  echo "base:    $BASE$(cnet_path_mark "$BASE")"
  echo "inbox:   ${BASE}.inbox$(cnet_path_mark "${BASE}.inbox" ' [ok]' ' [absent]')"
  echo "teacher: $TEACHER$(cnet_path_mark "$TEACHER" ' [ok]' ' [missing→maintenance]')"
  if [ -n "$res_show" ]; then
    echo "residual C: $res_show$(cnet_path_mark "$res_show")"
    echo "res_window: ${win_show:-$RESIDUAL_WINDOW}"
  else
    echo "residual C: (unset — hermetic/soft only until CNET_RESIDUAL_GGUF)"
  fi
  echo "binary:  $REPO/bin/gap_lane_run$(cnet_x_mark "$REPO/bin/gap_lane_run")"
  systemctl --user status "$LANE_UNIT" --no-pager 2>/dev/null | head -15 || \
    echo "lane unit: not installed/running"
  if cnet_serve_active; then
    echo "serve (CnetMcpServer): running"
  else
    echo "serve (CnetMcpServer): not running (enable SERVE=1 on start or deploy_hermes_mcp.sh)"
  fi
  units=$(cnet_unit_count_fast "$BASE")
  [ -n "$units" ] && echo "units:   $units (sealed CNB)"
  echo "=== loop ==="
  echo "  serve miss → ${BASE}.inbox"
  echo "  lane tick  → teach → seal ${BASE}"
  echo "  MCP recycle / reopen → new units available locally"
}

doctor() {
  status
  echo "=== doctor ==="
  local ok=1
  [ -x "$REPO/bin/gap_lane_run" ] || { echo "FIX: make gap_lane_run_build"; ok=0; }
  [ -f "$BASE" ] || { echo "FIX: set BASE_PATH to a sealed .cnb"; ok=0; }
  [ -f "$UNIT_DIR/$LANE_UNIT" ] || { echo "FIX: scripts/personal_ai_auto.sh install"; ok=0; }
  [ -x "$REPO/bin/cnb_audit" ] || echo "NOTE: make cnb_audit (unit counts in metrics/loop)"
  [ -x "$REPO/bin/serve_proof" ] || echo "NOTE: make serve_proof_cli (Tier A sample proof)"
  systemctl --user is-active --quiet "$LANE_UNIT" 2>/dev/null || \
    echo "NOTE: lane inactive — scripts/personal_ai_auto.sh start"
  # Light live serve sample when base + CLI present (proves sealed units run).
  if [ -f "$BASE" ] && [ -x "$REPO/bin/serve_proof" ]; then
    echo "--- serve-proof sample (max 4) ---"
    local sp_log="$REPO/logs/serve_proof_doctor.log"
    mkdir -p "$REPO/logs"
    if "$REPO/bin/serve_proof" "$BASE" --max 4 >"$sp_log" 2>&1 && \
        grep -q "SERVE_PROOF_PASS" "$sp_log"; then
      grep -E 'units_loaded=|sample:|SERVE_PROOF_' "$sp_log" || true
      echo "serve-proof: OK"
    else
      echo "NOTE: serve-proof sample failed — try: scripts/personal_ai_auto.sh serve-proof live"
      tail -5 "$sp_log" 2>/dev/null || true
    fi
  fi
  if [ "$ok" = 1 ]; then
    echo "PERSONAL_AI_AUTO_DOCTOR_OK"
  else
    echo "PERSONAL_AI_AUTO_DOCTOR_NEEDS_FIX"
    return 1
  fi
}

# Dispatch table (aliases map → canonical action). Avoids long if/elif chains.
case "$cmd" in
  hillclimb|eg) cmd=hillclimb ;;
  loop|report) cmd=loop ;;
  serve-proof|serve_proof) cmd=serve-proof ;;
  jtc-seal|json-toolcall-seal|json_toolcall_seal) cmd=jtc-seal ;;
esac

case "$cmd" in
  prepare) prepare ;;
  install) install_units ;;
  start) start ;;
  stop) stop ;;
  status) status ;;
  doctor) doctor ;;
  grow) bash "$REPO/scripts/personal_ai_campaign_nudge.sh" "${2:-16}" ;;
  observe) bash "$REPO/scripts/personal_ai_observe.sh" "$BASE" ;;
  metrics) bash "$REPO/scripts/personal_ai_metrics.sh" "$BASE" ;;
  hillclimb) bash "$REPO/scripts/personal_ai_hill_climb_report.sh" "$BASE" "${2:-7}" ;;
  loop) bash "$REPO/scripts/personal_ai_loop_report.sh" "$BASE" ;;
  serve-proof) bash "$REPO/scripts/personal_ai_serve_proof.sh" "${2:-hermetic}" ;;
  jtc-seal)
    # Seal closed-set json_toolcall_v0 into personal CNB (stops learner if needed).
    bash "$REPO/scripts/json_toolcall_seal.sh" "$BASE" ${2:+"$2"}
    ;;
  *)
    cat <<EOF
usage: $0 prepare|install|start|stop|status|doctor|grow|observe|metrics|hillclimb|loop|serve-proof|jtc-seal

Automatic Personal AI:
  1. prepare  — build learner binary + check base/teacher + placement doctor
  2. install  — user systemd unit for the learner
  3. start    — enable --now learner
                SERVE=1 also deploys Hermes MCP (local serve + inbox + health)
  grow        — seed inbox + campaign nudge (library growth)
  observe     — A: snapshot metrics JSON under logs/
  metrics     — JSON metrics to stdout
  hillclimb   — local EG report (days optional, default 7)
  loop        — human-readable loop report (inbox/ledger/learner/serve)
  serve-proof — post-seal Tier A proof (hermetic|live|all)
  jtc-seal    — seal json_toolcall_v0 into personal CNB (+ SoulHost verify)

Env: BASE_PATH / CNET_BASE_PATH  TEACHER  SERVE=0|1  TICK_SECONDS
     RESIDUAL / CNET_RESIDUAL_GGUF  (Tier C; also config/personal-ai.env)
     RESIDUAL_WINDOW / CNET_RESIDUAL_WINDOW
     STOP_LEARNER=1 (default for jtc-seal)
EOF
    exit 2
    ;;
esac
