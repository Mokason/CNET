#!/usr/bin/env bash
# Personal AI ops tick — run on a timer to heal, process TODOs, and learn.
#
# Usage:
#   scripts/personal_ai_ops_tick.sh
#   scripts/personal_ai_ops_tick.sh --once
#
# Install timer:
#   scripts/personal_ai_ops_tick.sh install
#   scripts/personal_ai_ops_tick.sh uninstall
#   scripts/personal_ai_ops_tick.sh status
#
# Does:
#   1) rebuild gap_lane_run if missing
#   2) restart learner if dead (ops env)
#   3) optional jtc-seal if unit missing
#   4) process open TODOs from queue
#   5) optional Hermes ask on failures / unknown tools
#   6) write logs/personal_ai_ops/last_tick.json
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=personal_ai_common.sh
. "$REPO/scripts/personal_ai_common.sh"
cnet_load_personal_env
if [ -f "$REPO/config/personal-ai-ops.env" ]; then
  set -a
  # shellcheck source=/dev/null
  . "$REPO/config/personal-ai-ops.env" || true
  set +a
fi

BASE="$(cnet_default_base)"
OPS_DIR="$REPO/logs/personal_ai_ops"
STATE="$OPS_DIR/state.env"
TODO="${OPS_TODO_FILE:-$OPS_DIR/todo.jsonl}"
REPORT="$OPS_DIR/last_tick.json"
LOCK="$OPS_DIR/tick.lock"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
LANE_UNIT="cnet-personal-ai-lane.service"
OPS_SVC="cnet-personal-ai-ops.service"
OPS_TIMER="cnet-personal-ai-ops.timer"
TS=$(date -Iseconds)
mkdir -p "$OPS_DIR" "$OPS_DIR/lessons"

info() { echo "ops_tick: $*"; }
warn() { echo "ops_tick: WARN: $*" >&2; }

# Return the last unsigned key=value metric from a log.  Absence is a normal
# zero result: callers run under `set -euo pipefail`, so grep-style misses must
# never terminate the whole scheduled tick.
ops_last_metric() {
  local file="${1:-}" key="${2:-}"
  if [[ ! "$key" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || [ ! -f "$file" ]; then
    printf '0\n'
    return 0
  fi
  awk -v key="$key" '
    {
      rest = $0
      while (match(rest, key "=[0-9]+")) {
        value = substr(rest, RSTART + length(key) + 1, RLENGTH - length(key) - 1)
        rest = substr(rest, RSTART + RLENGTH)
      }
    }
    END { print value == "" ? 0 : value }
  ' "$file"
}

cmd="${1:-tick}"

# --- install / uninstall / status -----------------------------------------
install_timer() {
  mkdir -p "$UNIT_DIR"
  cat >"$UNIT_DIR/$OPS_SVC" <<EOF
[Unit]
Description=CNET Personal AI ops tick (heal, TODO, learn via Hermes)
After=default.target

[Service]
Type=oneshot
WorkingDirectory=$REPO
EnvironmentFile=-$REPO/config/personal-ai.env
EnvironmentFile=-$REPO/config/personal-ai-ops.env
Environment=CNET_BASE_PATH=$BASE
ExecStart=$REPO/scripts/personal_ai_ops_tick.sh tick
Nice=15

[Install]
WantedBy=default.target
EOF
  cat >"$UNIT_DIR/$OPS_TIMER" <<EOF
[Unit]
Description=CNET Personal AI ops timer (every 15 minutes)

[Timer]
OnBootSec=3min
OnUnitActiveSec=15min
AccuracySec=1min
Persistent=true
Unit=$OPS_SVC

[Install]
WantedBy=timers.target
EOF
  systemctl --user daemon-reload
  systemctl --user enable --now "$OPS_TIMER"
  info "installed $UNIT_DIR/$OPS_TIMER"
  systemctl --user list-timers "$OPS_TIMER" --no-pager || true
  echo "PERSONAL_AI_OPS_INSTALL_OK"
}

uninstall_timer() {
  systemctl --user disable --now "$OPS_TIMER" 2>/dev/null || true
  systemctl --user stop "$OPS_SVC" 2>/dev/null || true
  rm -f "$UNIT_DIR/$OPS_SVC" "$UNIT_DIR/$OPS_TIMER"
  systemctl --user daemon-reload
  info "uninstalled ops timer"
  echo "PERSONAL_AI_OPS_UNINSTALL_OK"
}

status_timer() {
  systemctl --user status "$OPS_TIMER" --no-pager 2>/dev/null || echo "timer: not installed"
  systemctl --user list-timers "$OPS_TIMER" --no-pager 2>/dev/null || true
  if [ -f "$REPORT" ]; then
    echo "--- last tick ---"
    cat "$REPORT"
  fi
}

case "$cmd" in
  --extract-metric)
    if [ "$#" -ne 3 ]; then
      echo "usage: $0 --extract-metric LOG KEY" >&2
      exit 2
    fi
    ops_last_metric "$2" "$3"
    exit 0
    ;;
  install) install_timer; exit 0 ;;
  uninstall) uninstall_timer; exit 0 ;;
  status) status_timer; exit 0 ;;
  tick|--once|"") ;;
  *)
    echo "usage: $0 tick|install|uninstall|status" >&2
    exit 2
    ;;
esac

# --- lock / throttle ------------------------------------------------------
if [ -f "$LOCK" ]; then
  age=$(( $(date +%s) - $(stat -c %Y "$LOCK" 2>/dev/null || echo 0) ))
  if [ "$age" -lt 120 ]; then
    info "skip: lock held (${age}s)"
    exit 0
  fi
fi
echo $$ >"$LOCK"
trap 'rm -f "$LOCK"' EXIT

MIN_IV="${OPS_MIN_INTERVAL_SEC:-300}"
if [ -f "$STATE" ]; then
  # shellcheck source=/dev/null
  . "$STATE" || true
  if [ -n "${LAST_TICK_UNIX:-}" ]; then
    now=$(date +%s)
    if [ $((now - LAST_TICK_UNIX)) -lt "$MIN_IV" ]; then
      info "skip: min interval ${MIN_IV}s"
      exit 0
    fi
  fi
fi

actions=()
issues=()
hermes_asked=0
todos_done=0
learner=0
serve=0
jtc=0
fixed=0

# --- 1) binary present ----------------------------------------------------
if [ ! -x "$REPO/bin/gap_lane_run" ]; then
  if [ "${OPS_REBUILD_GAP_LANE:-1}" = "1" ]; then
    info "rebuilding gap_lane_run"
    if make -C "$REPO" gap_lane_run_build -j"$(cnet_nproc)" >/dev/null 2>&1; then
      actions+=("rebuild_gap_lane_run")
      fixed=1
    else
      issues+=("gap_lane_run_build_failed")
    fi
  else
    issues+=("gap_lane_run_missing")
  fi
fi

# --- 2) learner alive -----------------------------------------------------
if cnet_learner_active; then
  learner=1
else
  learner=0
  issues+=("learner_inactive")
  if [ "${OPS_RESTART_LEARNER:-1}" = "1" ]; then
    if [ ! -x "$REPO/bin/gap_lane_run" ] && [ "${OPS_REBUILD_GAP_LANE:-1}" = "1" ]; then
      make -C "$REPO" gap_lane_run_build -j"$(cnet_nproc)" >/dev/null 2>&1 || true
    fi
    if systemctl --user start "$LANE_UNIT" 2>/dev/null; then
      sleep 2
      if cnet_learner_active; then
        actions+=("restart_learner")
        learner=1
        fixed=1
      else
        issues+=("learner_start_failed")
      fi
    else
      issues+=("learner_start_failed")
    fi
  fi
fi

# --- 3) serve (observe only unless OPS_RESTART_HERMES=1) ------------------
if cnet_serve_active; then
  serve=1
else
  serve=0
  issues+=("serve_mcp_inactive")
  if [ "${OPS_RESTART_HERMES:-0}" = "1" ]; then
    if systemctl --user restart hermes-gateway.service 2>/dev/null; then
      actions+=("restart_hermes_gateway")
      sleep 3
      cnet_serve_active && serve=1 && fixed=1
    fi
  fi
fi

# --- 4) JTC sealed --------------------------------------------------------
jtc=$(cnet_jtc_present "$BASE")
if [ "$jtc" != "1" ]; then
  issues+=("json_toolcall_missing")
  if [ "${OPS_AUTO_JTC_SEAL:-1}" = "1" ] && [ -f "$BASE" ]; then
    info "sealing json_toolcall (closed-set + lookup tools)"
    if STOP_LEARNER=1 bash "$REPO/scripts/json_toolcall_seal.sh" "$BASE" >/dev/null 2>&1; then
      actions+=("jtc_seal")
      jtc=$(cnet_jtc_present "$BASE")
      fixed=1
    else
      issues+=("jtc_seal_failed")
    fi
  fi
fi

# --- 5) metrics snapshot --------------------------------------------------
METRICS_FILE="$OPS_DIR/last_metrics.json"
echo '{}' >"$METRICS_FILE"
if [ -x "$REPO/scripts/personal_ai_metrics.sh" ]; then
  bash "$REPO/scripts/personal_ai_metrics.sh" "$BASE" >"$METRICS_FILE" 2>/dev/null || echo '{}' >"$METRICS_FILE"
fi

# --- 6) TODO queue --------------------------------------------------------
: >>"$TODO"
max_todo="${OPS_TODO_MAX:-2}"
if [ -f "$TODO" ] && [ -s "$TODO" ]; then
  tmp_todo=$(mktemp)
  while IFS= read -r line; do
    [ -z "$line" ] && continue
    status=$(echo "$line" | python3 -c "import sys,json; print(json.loads(sys.stdin.read()).get('status',''))" 2>/dev/null || echo "")
    if [ "$status" != "open" ]; then
      echo "$line" >>"$tmp_todo"
      continue
    fi
    if [ "$todos_done" -ge "$max_todo" ]; then
      echo "$line" >>"$tmp_todo"
      continue
    fi
    text=$(echo "$line" | python3 -c "import sys,json; print(json.loads(sys.stdin.read()).get('text',''))" 2>/dev/null || echo "")
    tid=$(echo "$line" | python3 -c "import sys,json; print(json.loads(sys.stdin.read()).get('id',''))" 2>/dev/null || echo "")
    info "TODO open: $text"
    # Heuristic actions
    done_one=0
    case "$text" in
      *jtc*|*json_tool*|*toolcall*)
        if STOP_LEARNER=0 bash "$REPO/scripts/json_toolcall_seal.sh" "$BASE" >/dev/null 2>&1; then
          done_one=1
          actions+=("todo_jtc_seal")
        fi
        ;;
      *learner*|*gap_lane*)
        systemctl --user start "$LANE_UNIT" 2>/dev/null && done_one=1 && actions+=("todo_start_learner")
        ;;
      *observe*|*metrics*)
        bash "$REPO/scripts/personal_ai_observe.sh" "$BASE" >/dev/null 2>&1 && done_one=1 && actions+=("todo_observe")
        ;;
      *)
        if [ "${OPS_ASK_HERMES:-1}" = "1" ]; then
          if bash "$REPO/scripts/personal_ai_ops_ask_hermes.sh" \
            "TODO for personal AI ops: $text. Propose one concrete command under $REPO." \
            >/dev/null 2>&1; then
            hermes_asked=1
            actions+=("todo_hermes")
            # leave open unless Hermes said DONE — mark done after ask so queue advances
            done_one=1
          fi
        fi
        ;;
    esac
    if [ "$done_one" = "1" ]; then
      echo "$line" | python3 -c "import sys,json; o=json.loads(sys.stdin.read()); o['status']='done'; o['done_ts']='$TS'; print(json.dumps(o))" >>"$tmp_todo"
      todos_done=$((todos_done + 1))
      fixed=1
    else
      echo "$line" >>"$tmp_todo"
    fi
  done <"$TODO"
  mv "$tmp_todo" "$TODO"
fi

# --- 7) escalate remaining issues via Hermes ------------------------------
if [ "${#issues[@]}" -gt 0 ] && [ "${OPS_ASK_HERMES:-1}" = "1" ]; then
  # Only ask if we still have issues after fixes
  still=()
  for i in "${issues[@]}"; do
    case "$i" in
      learner_inactive)
        cnet_learner_active || still+=("$i")
        ;;
      json_toolcall_missing)
        [ "$(cnet_jtc_present "$BASE")" = "1" ] || still+=("$i")
        ;;
      *) still+=("$i") ;;
    esac
  done
  if [ "${#still[@]}" -gt 0 ]; then
    q="Personal AI ops found issues after heal attempts: ${still[*]}. Base=$BASE learner=$(cnet_learner_active && echo 1 || echo 0). Suggest exact fix commands."
    if bash "$REPO/scripts/personal_ai_ops_ask_hermes.sh" "$q" >/dev/null 2>&1; then
      hermes_asked=1
      actions+=("hermes_diagnose")
    else
      warn "Hermes ask failed (offline?)"
    fi
  fi
fi

# --- 8) unknown-tool demand (inbox jtc lines growing without unit? ) ------
if [ "${OPS_NOTE_JTC_GAP_ON_UNKNOWN:-1}" = "1" ] && [ "$jtc" = "1" ]; then
  # If many NO_PLAN but not jtc, leave them to token teacher.
  # If jtc_gaps present and hermes lessons mention new tools, operator expands alphabet.
  :
fi

# --- 9) stale curriculum (Ollama planner → teachable queue) ---------------
if [ "${OPS_CURRICULUM_ON_STALE:-0}" = "1" ] && [ -x "$REPO/scripts/personal_ai_curriculum.sh" ]; then
  if bash "$REPO/scripts/personal_ai_curriculum.sh" run >/dev/null 2>>"$OPS_DIR/curriculum.log"; then
    if grep -q 'PERSONAL_AI_CURRICULUM_RUN_OK' "$OPS_DIR/curriculum.log" 2>/dev/null || \
       [ -f "$OPS_DIR/last_curriculum.json" ]; then
      # only record action when a plan was produced this hour recently
      if [ -f "$OPS_DIR/last_curriculum.json" ]; then
        age_plan=$(( $(date +%s) - $(stat -c %Y "$OPS_DIR/last_curriculum.json" 2>/dev/null || echo 0) ))
        if [ "$age_plan" -lt 120 ]; then
          actions+=("curriculum_plan")
          fixed=1
        fi
      fi
    fi
  fi
fi

# --- 10) pattern runtime: promote fluid→frozen + optional CNB import ------
if [ "${OPS_PATTERN_PROMOTE:-1}" = "1" ]; then
  if [ ! -x "$REPO/bin/cnet_pattern" ]; then
    make -C "$REPO" pattern_cli -j"$(cnet_nproc)" >/dev/null 2>&1 || true
  fi
  if [ -x "$REPO/bin/cnet_pattern" ]; then
    export CNET_PATTERN_STORE="${CNET_PATTERN_STORE:-$OPS_DIR/pattern_runtime.jsonl}"
    export CNET_BASE_PATH="${CNET_BASE_PATH:-$BASE}"
    if CNET_BASE_PATH="$BASE" CNET_PATTERN_STORE="$CNET_PATTERN_STORE" \
      "$REPO/bin/cnet_pattern" promote >>"$OPS_DIR/pattern.log" 2>&1; then
      if grep -q 'CNET_PATTERN_PROMOTE_OK' "$OPS_DIR/pattern.log" 2>/dev/null; then
        prom=$(ops_last_metric "$OPS_DIR/pattern.log" promoted)
        if [ -n "${prom:-}" ] && [ "$prom" -gt 0 ] 2>/dev/null; then
          actions+=("pattern_promote")
          fixed=1
        fi
      fi
    fi
    if [ "${OPS_PATTERN_IMPORT_UNITS:-0}" = "1" ]; then
      maxu="${OPS_PATTERN_IMPORT_MAX:-32}"
      if CNET_BASE_PATH="$BASE" CNET_PATTERN_STORE="$CNET_PATTERN_STORE" \
        "$REPO/bin/cnet_pattern" import-units "$maxu" >>"$OPS_DIR/pattern.log" 2>&1; then
        actions+=("pattern_import_units")
      fi
    fi
    CNET_PATTERN_STORE="$CNET_PATTERN_STORE" "$REPO/bin/cnet_pattern" status \
      >"$OPS_DIR/pattern_status.txt" 2>/dev/null || true
  fi
fi

# --- report ---------------------------------------------------------------
units=$(cnet_unit_count_fast "$BASE")
units=${units:-null}
actions_json=$(printf '%s\n' "${actions[@]+"${actions[@]}"}" | python3 -c 'import sys,json; print(json.dumps([l.strip() for l in sys.stdin if l.strip()]))' 2>/dev/null || echo '[]')
issues_json=$(printf '%s\n' "${issues[@]+"${issues[@]}"}" | python3 -c 'import sys,json; print(json.dumps([l.strip() for l in sys.stdin if l.strip()]))' 2>/dev/null || echo '[]')

python3 - <<PY
import json
from pathlib import Path
try:
    metrics = json.loads(Path("$METRICS_FILE").read_text())
except Exception:
    metrics = {}
try:
    actions = json.loads('''$actions_json''')
except Exception:
    actions = []
try:
    issues = json.loads('''$issues_json''')
except Exception:
    issues = []
units_raw = "$units"
units = None if units_raw in ("", "null") else int(units_raw)
rep = {
  "ts": "$TS",
  "base": "$BASE",
  "learner_active": $learner,
  "serve_mcp": $serve,
  "json_toolcall": int("$jtc" or 0),
  "units": units,
  "actions": actions,
  "issues": issues,
  "todos_done": $todos_done,
  "hermes_asked": $hermes_asked,
  "fixed": $fixed,
  "metrics": metrics,
}
Path("$REPORT").write_text(json.dumps(rep, indent=2) + "\n")
print(json.dumps(rep, indent=2))
PY

echo "LAST_TICK_UNIX=$(date +%s)" >"$STATE"
echo "LAST_TICK_TS=$TS" >>"$STATE"

if [ "$fixed" = "1" ] || [ "$todos_done" -gt 0 ]; then
  echo "PERSONAL_AI_OPS_TICK_OK fixed=1"
else
  echo "PERSONAL_AI_OPS_TICK_OK fixed=0"
fi
