#!/usr/bin/env bash
# CNET 24/7 auto-teach tick — Bonsai-8B as always-on teacher.
#
# Keeps the system learning with no human in the loop:
#   1) bonsai-server healthy
#   2) personal-ai-lane with HTTP teacher
#   3) feed bounded curriculum (curiosity + procedure queue)
#   4) PEFT / JTC cert tick (cnet_cert_learn_tick)
#   5) residual structure-mine persist (occasional)
#   6) write logs/autoteach/last_tick.json
#
# Safe bounds: no freeform sludge, no gateway restart, caps on curiosity.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
mkdir -p logs/autoteach

BASE="${CNET_BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}"
INBOX="${CNET_GAP_INBOX:-$BASE.inbox}"
LOG="logs/autoteach/last_tick.json"
RUNLOG="logs/autoteach/tick_$(date +%Y%m%d).log"
LOCK="logs/autoteach/tick.lock"
TS="$(date -Iseconds)"

exec 9>"$LOCK"
if ! flock -n 9; then
  echo "autoteach: another tick holds lock; exit"
  exit 0
fi

log() { echo "[$TS] $*" | tee -a "$RUNLOG"; }

# --- env for children ---
export CNET_BASE_PATH="$BASE"
export CNET_GAP_INBOX="$INBOX"
export CNET_FAULT_LOG="${CNET_FAULT_LOG:-$REPO/logs/cnet_faults.jsonl}"
export CNET_LORA_STORE_DIR="${CNET_LORA_STORE_DIR:-$REPO/logs/lora_store}"
export CNET_LORA_STORE_AUTOSAVE=1
export CNET_FAULT_MIRROR=1
export CNET_FAULT_DEDUPE="${CNET_FAULT_DEDUPE:-0}"
export CNET_RESIDUAL_HTTP="${CNET_RESIDUAL_HTTP:-http://127.0.0.1:8080}"
export CNET_TEACHER_HTTP="${CNET_TEACHER_HTTP:-http://127.0.0.1:8080}"
export CNET_RESIDUAL_WINDOW="${CNET_RESIDUAL_WINDOW:-$REPO/english_window_256_bonsai.txt}"
export CNET_WINDOW_FILE="${CNET_WINDOW_FILE:-$REPO/english_window_256_bonsai.txt}"
export CNET_STRUCTURE_EXPAND_N="${CNET_STRUCTURE_EXPAND_N:-32}"
export CNET_SOUL_RESIDUAL_HERMETIC=1
export CNET_SOUL_RESIDUAL_PREFER_HERMETIC=0
export CNET_CURIOSITY=1
export CNET_CURIOSITY_MAX_PER_HOUR="${CNET_CURIOSITY_MAX_PER_HOUR:-12}"
export CNET_CURIOSITY_MAX_PER_TICK="${CNET_CURIOSITY_MAX_PER_TICK:-2}"
export CNET_CURIOSITY_K=3
export CNET_CURIOSITY_YIELD_OPEN="${CNET_CURIOSITY_YIELD_OPEN:-32}"
export CNET_AUTO_LEARN_FREEFORM=0

bonsai_ok=0
lane_ok=0
cert_ok=0
mine_ok=0
proc_n=0
units_before=0
units_after=0
lora_n=0
faults_n=0

# --- 1) Bonsai server ---
if systemctl --user is-active --quiet bonsai-server.service; then
  bonsai_ok=1
else
  log "starting bonsai-server"
  systemctl --user start bonsai-server.service || true
  sleep 2
  systemctl --user is-active --quiet bonsai-server.service && bonsai_ok=1
fi
if ! curl -sf --max-time 5 "$CNET_RESIDUAL_HTTP/v1/models" >/dev/null; then
  log "WARN bonsai API not ready"
  bonsai_ok=0
else
  bonsai_ok=1
fi

# --- 2) Lane with HTTP teacher ---
if systemctl --user is-active --quiet cnet-personal-ai-lane.service; then
  lane_ok=1
else
  log "starting personal-ai-lane"
  systemctl --user start cnet-personal-ai-lane.service || true
  sleep 2
  systemctl --user is-active --quiet cnet-personal-ai-lane.service && lane_ok=1
fi

# Count units (best-effort via strings / size proxy)
if [[ -f "$BASE" ]]; then
  units_before=$(python3 - <<PY
import re,sys
b=open("$BASE","rb").read()
# rough: not exact count
print(b.count(b"acq_")+b.count(b"json_toolcall")+b.count(b"hyb_struct"))
PY
)
fi

# --- 3) Curriculum feed ---
# Procedure skills (structured, not tk sludge)
if [[ -x scripts/procedure_chunk_seal.sh ]]; then
  out=$(bash scripts/procedure_chunk_seal.sh 2>&1 | tail -1) || true
  log "$out"
  proc_n=$(echo "$out" | grep -oE 'count=[0-9]+' | cut -d= -f2 || echo 0)
fi
# Research queue occasionally (every 3rd hour bucket)
if [[ $(( $(date +%H) % 3 )) -eq 0 ]] && [[ -x scripts/queue_research_skills.sh ]]; then
  bash scripts/queue_research_skills.sh >>"$RUNLOG" 2>&1 || true
fi

# Bounded teachable NO_PLAN lines for window tokens (HTTP teacher can close these)
# Format matches gap_inbox_note_no_plan / curiosity: NO_PLAN family w ... goal
if [[ -f "$CNET_WINDOW_FILE" ]] && [[ "$bonsai_ok" -eq 1 ]]; then
  python3 - <<'PY' >>"$RUNLOG" 2>&1 || true
import os, random, time
from pathlib import Path
win = Path(os.environ["CNET_WINDOW_FILE"])
inbox = Path(os.environ.get("CNET_GAP_INBOX", "soul_gemma4v2_final.cnb.inbox"))
ids = [int(x) for x in win.read_text().split() if x.strip().isdigit()]
if not ids:
    raise SystemExit(0)
k = int(os.environ.get("CNET_CURIOSITY_K", "3"))
n = int(os.environ.get("CNET_AUTOTEACH_INJECT", "4"))
W = len(ids)
# sample uncovered-ish tokens (random slice; lane dedupes)
random.seed(int(time.time()) // 3600)  # stable within hour
picks = random.sample(ids, min(n, len(ids)))
lines = []
for tid in picks:
    # ONEHOT family=0, width=W, count=1 tag w_cur -> goal tk{N}q{N} k fields
    lines.append(
        f"NO_PLAN 0 {W} 1 w_cur 0 {W} {k} tk{tid}q{tid}\n"
    )
inbox.parent.mkdir(parents=True, exist_ok=True)
with inbox.open("a") as f:
    f.writelines(lines)
print(f"AUTOTEACH_INJECT n={len(lines)} inbox={inbox}")
PY
fi

# --- 4) PEFT / JTC cert tick (bounded) ---
if [[ -x bin/cnet_cert_learn_tick ]] && [[ "$bonsai_ok" -eq 1 ]]; then
  if timeout 600 env CNET_FAULT_DEDUPE=0 ./bin/cnet_cert_learn_tick >>"$RUNLOG" 2>&1; then
    cert_ok=1
  else
    log "cert_learn_tick failed or timed out"
  fi
fi

# --- 5) Structure-mine persist every other hour ---
if [[ $(( $(date +%H) % 2 )) -eq 0 ]] && [[ -x bin/struct_mine_persist ]] && [[ "$bonsai_ok" -eq 1 ]]; then
  if timeout 900 ./bin/struct_mine_persist >>"$RUNLOG" 2>&1; then
    mine_ok=1
  else
    log "struct_mine_persist failed or timed out"
  fi
fi

# --- metrics ---
if [[ -f "$CNET_FAULT_LOG" ]]; then
  faults_n=$(wc -l <"$CNET_FAULT_LOG" | tr -d ' ')
fi
if [[ -d "$CNET_LORA_STORE_DIR" ]]; then
  lora_n=$(ls -1 "$CNET_LORA_STORE_DIR" 2>/dev/null | wc -l | tr -d ' ')
fi
if [[ -f "$BASE" ]]; then
  units_after=$(python3 - <<PY
b=open("$BASE","rb").read()
print(b.count(b"acq_")+b.count(b"json_toolcall")+b.count(b"hyb_struct"))
PY
)
fi

python3 - <<PY
import json, os
from pathlib import Path
rep = {
  "ts": os.environ.get("TS", "$TS"),
  "bonsai_ok": $bonsai_ok,
  "lane_ok": $lane_ok,
  "cert_ok": $cert_ok,
  "mine_ok": $mine_ok,
  "procedure_seeds": int("$proc_n" or 0),
  "fault_lines": int("$faults_n" or 0),
  "lora_files": int("$lora_n" or 0),
  "units_proxy_before": int("$units_before" or 0),
  "units_proxy_after": int("$units_after" or 0),
  "residual_http": os.environ.get("CNET_RESIDUAL_HTTP", ""),
  "base": os.environ.get("CNET_BASE_PATH", ""),
}
Path("$LOG").write_text(json.dumps(rep, indent=2) + "\n")
print("AUTOTEACH_TICK_OK", json.dumps(rep))
PY

exit 0
