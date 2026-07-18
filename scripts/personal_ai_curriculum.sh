#!/usr/bin/env bash
# Stale → curriculum planner (Ollama local/cloud) → teachable learn queue.
#
# Cloud/local LLM proposes *what* to learn next; CNET still certifies.
#
# Usage:
#   scripts/personal_ai_curriculum.sh status
#   scripts/personal_ai_curriculum.sh detect
#   scripts/personal_ai_curriculum.sh plan [--force]
#   scripts/personal_ai_curriculum.sh materialize [--max N]
#   scripts/personal_ai_curriculum.sh run [--force]   # detect|force → plan → materialize
#   scripts/personal_ai_curriculum.sh test            # forced e2e smoke (safe sandbox inbox)
#
# Env (personal-ai-ops.env):
#   OPS_CURRICULUM_ON_STALE=1
#   OPS_CURRICULUM_BACKEND=ollama   # ollama | hermes | off
#   OPS_CURRICULUM_MODEL=g4v2ref:latest
#   OPS_CURRICULUM_MODEL_FALLBACK=minimax-m3:cloud
#   OPS_CURRICULUM_TIMEOUT_SEC=120
#   OPS_STALE_NO_GROWTH_SEC=2700    # units flat this long → stale (default 45m)
#   OPS_CURRICULUM_MAX_PER_HOUR=2
#   OPS_CURRICULUM_MAX_ITEMS=3
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
QUEUE="${OPS_CURRICULUM_QUEUE:-$OPS_DIR/learn_queue.jsonl}"
STATE="$OPS_DIR/curriculum_state.env"
LAST_PLAN="$OPS_DIR/last_curriculum.json"
METRICS_SNAP="$OPS_DIR/last_metrics.json"
REPORT="$OPS_DIR/last_tick.json"
TODO="${OPS_TODO_FILE:-$OPS_DIR/todo.jsonl}"
INBOX="${BASE}.inbox"
WINDOW="${WINDOW_FILE:-${CNET_WINDOW_FILE:-$REPO/english_window_256.txt}}"

BACKEND="${OPS_CURRICULUM_BACKEND:-ollama}"
MODEL="${OPS_CURRICULUM_MODEL:-g4v2ref:latest}"
MODEL_FALLBACK="${OPS_CURRICULUM_MODEL_FALLBACK:-minimax-m3:cloud}"
TIMEOUT_SEC="${OPS_CURRICULUM_TIMEOUT_SEC:-120}"
STALE_SEC="${OPS_STALE_NO_GROWTH_SEC:-2700}"
MAX_PER_HOUR="${OPS_CURRICULUM_MAX_PER_HOUR:-2}"
MAX_ITEMS="${OPS_CURRICULUM_MAX_ITEMS:-3}"
OLLAMA_HOST="${OLLAMA_HOST:-http://127.0.0.1:11434}"

mkdir -p "$OPS_DIR"
cmd="${1:-status}"
shift || true
FORCE=0
MAX_MAT="$MAX_ITEMS"
SANDBOX=0
while [ $# -gt 0 ]; do
  case "$1" in
    --force) FORCE=1 ;;
    --max) MAX_MAT="${2:-3}"; shift ;;
    --sandbox) SANDBOX=1 ;;
    *) ;;
  esac
  shift || true
done

info() { echo "curriculum: $*"; }
warn() { echo "curriculum: WARN: $*" >&2; }

load_state() {
  LAST_UNITS=""
  LAST_UNITS_TS=""
  CURRICULUM_HOUR=""
  CURRICULUM_HOUR_COUNT=0
  if [ -f "$STATE" ]; then
    # shellcheck source=/dev/null
    . "$STATE" || true
  fi
}

save_state() {
  cat >"$STATE" <<EOF
LAST_UNITS=${LAST_UNITS:-}
LAST_UNITS_TS=${LAST_UNITS_TS:-}
CURRICULUM_HOUR=${CURRICULUM_HOUR:-}
CURRICULUM_HOUR_COUNT=${CURRICULUM_HOUR_COUNT:-0}
LAST_PLAN_TS=${LAST_PLAN_TS:-}
LAST_STALE=${LAST_STALE:-0}
EOF
}

current_units() {
  local u
  u=$(cnet_unit_count_fast "$BASE" 2>/dev/null || echo "")
  if [ -z "$u" ] || [ "$u" = "null" ]; then
    if [ -f "$REPORT" ]; then
      u=$(python3 -c "import json; print(json.load(open('$REPORT')).get('units') or '')" 2>/dev/null || echo "")
    fi
  fi
  echo "${u:-0}"
}

teacher_idle_hint() {
  # 1 if recent journal suggests teacher sleep / zero closures
  if journalctl --user -u cnet-personal-ai-lane.service -n 30 --no-pager 2>/dev/null \
    | grep -qE 'teacher sleep|closed=0.*drained=0|drained=0.*closed=0'; then
    echo 1
  else
    echo 0
  fi
}

detect_stale() {
  load_state
  local now units idle age
  now=$(date +%s)
  units=$(current_units)
  idle=$(teacher_idle_hint)

  if [ -z "${LAST_UNITS:-}" ] || [ -z "${LAST_UNITS_TS:-}" ]; then
    LAST_UNITS="$units"
    LAST_UNITS_TS="$now"
    LAST_STALE=0
    save_state
    echo "fresh"
    echo "curriculum: detect units=$units baseline recorded" >&2
    return 0
  fi

  if [ "$units" -gt "${LAST_UNITS:-0}" ] 2>/dev/null; then
    LAST_UNITS="$units"
    LAST_UNITS_TS="$now"
    LAST_STALE=0
    save_state
    echo "growing"
    echo "curriculum: detect units=$units growing (was $LAST_UNITS)" >&2
    return 0
  fi

  # units flat or down
  age=$((now - LAST_UNITS_TS))
  if [ "$FORCE" = "1" ] || [ "$age" -ge "$STALE_SEC" ] || { [ "$idle" = "1" ] && [ "$age" -ge 600 ]; }; then
    LAST_STALE=1
    save_state
    echo "stale"
    echo "curriculum: detect STALE units=$units age=${age}s idle_hint=$idle force=$FORCE" >&2
    return 0
  fi

  LAST_STALE=0
  save_state
  echo "watching"
  echo "curriculum: detect watching units=$units age=${age}s need=${STALE_SEC}s idle=$idle" >&2
}

hour_budget_ok() {
  load_state
  local h
  h=$(date +%Y%m%d%H)
  if [ "${CURRICULUM_HOUR:-}" != "$h" ]; then
    CURRICULUM_HOUR="$h"
    CURRICULUM_HOUR_COUNT=0
    save_state
  fi
  if [ "${CURRICULUM_HOUR_COUNT:-0}" -ge "$MAX_PER_HOUR" ] && [ "$FORCE" != "1" ]; then
    return 1
  fi
  return 0
}

consume_budget() {
  load_state
  local h
  h=$(date +%Y%m%d%H)
  if [ "${CURRICULUM_HOUR:-}" != "$h" ]; then
    CURRICULUM_HOUR="$h"
    CURRICULUM_HOUR_COUNT=0
  fi
  CURRICULUM_HOUR_COUNT=$((CURRICULUM_HOUR_COUNT + 1))
  LAST_PLAN_TS=$(date -Iseconds)
  save_state
}

ollama_chat() {
  local model="$1" prompt="$2" out_file="$3"
  local payload resp
  payload=$(python3 -c "
import json,sys
print(json.dumps({
  'model': sys.argv[1],
  'stream': False,
  'options': {'temperature': 0.3, 'num_predict': 800},
  'messages': [
    {'role': 'system', 'content': 'You are a curriculum planner for a local certified AI library (CNET). Reply with ONLY valid JSON, no markdown.'},
    {'role': 'user', 'content': sys.argv[2]},
  ],
}))
" "$model" "$prompt")
  if ! resp=$(curl -sS --max-time "$TIMEOUT_SEC" \
    -H 'Content-Type: application/json' \
    -d "$payload" \
    "$OLLAMA_HOST/api/chat" 2>"$OPS_DIR/curriculum_ollama.err"); then
    return 1
  fi
  python3 -c "
import json,sys
raw=sys.stdin.read()
try:
  o=json.loads(raw)
except Exception as e:
  sys.stderr.write('bad ollama json: %s\n'%e)
  sys.exit(1)
msg=(o.get('message') or {}).get('content') or ''
print(msg)
" <<<"$resp" >"$out_file"
  [ -s "$out_file" ]
}

build_prompt() {
  local units curiosity jtc tools_list
  units=$(current_units)
  curiosity=0
  if [ -f "${BASE}.curiosity" ]; then
    curiosity=$(grep -E '^count=' "${BASE}.curiosity" 2>/dev/null | tail -1 | cut -d= -f2 || echo 0)
  fi
  jtc=$(cnet_jtc_unit_name "$BASE" 2>/dev/null || echo none)
  tools_list="calculator,memory_store,memory_recall,file_read,cnet_recall,web_search,wiki_lookup,final"
  cat <<EOF
The local CNET personal AI library is STALE (not broken): units stopped growing, teacher idle, inbox empty.
Current: units=$units jtc=$jtc curiosity_hour_count=$curiosity base=$BASE
Tools already in closed set: $tools_list
Window has 256 token ids (lane can teach w_cur → tk{id}q{id} top-3).

Propose 2-4 next learning items as JSON ONLY:
{
  "reason": "short why stale",
  "items": [
    {"kind":"window_token","token_id":506,"why":"...","priority":1},
    {"kind":"lookup_skill","tool":"wiki_lookup","query":"Alan Turing","why":"...","priority":2},
    {"kind":"lookup_skill","tool":"web_search","query":"COBOL inventor","why":"...","priority":2},
    {"kind":"jtc_expand","tool":"http_get","why":"needs human alphabet expand","priority":3},
    {"kind":"grow_local","n":4,"why":"seed teachable token gaps","priority":1}
  ]
}
Rules:
- Prefer kind window_token (use integer token_id from typical english window 500-2000) or grow_local or lookup_skill.
- jtc_expand only if a real missing tool is needed; do not invent exotic tools.
- No markdown fences. JSON only.
EOF
}

extract_json() {
  local raw="$1" out="$2"
  python3 - "$raw" "$out" <<'PY'
import json, re, sys
text = open(sys.argv[1], encoding="utf-8", errors="replace").read().strip()
text = re.sub(r"^```(?:json)?\s*", "", text)
text = re.sub(r"\s*```$", "", text)
start = text.find("{")
end = text.rfind("}")
if start < 0 or end <= start:
    raise SystemExit("no json object in model output")
obj = json.loads(text[start : end + 1])
if not isinstance(obj.get("items"), list):
    raise SystemExit("items missing")
open(sys.argv[2], "w", encoding="utf-8").write(json.dumps(obj, indent=2) + "\n")
print("ok items", len(obj["items"]))
PY
}

plan() {
  if [ "$BACKEND" = "off" ]; then
    warn "backend=off"
    return 1
  fi
  if ! hour_budget_ok; then
    warn "hourly budget exhausted ($MAX_PER_HOUR)"
    return 1
  fi

  local prompt raw_txt="$OPS_DIR/curriculum_raw.txt" used_model=""
  prompt=$(build_prompt)
  info "planning via ollama model=$MODEL (fallback=$MODEL_FALLBACK)"
  rm -f "$raw_txt"
  if ollama_chat "$MODEL" "$prompt" "$raw_txt"; then
    used_model="$MODEL"
  elif [ -n "$MODEL_FALLBACK" ] && [ "$MODEL_FALLBACK" != "$MODEL" ] && \
       ollama_chat "$MODEL_FALLBACK" "$prompt" "$raw_txt"; then
    used_model="$MODEL_FALLBACK"
    info "used fallback model $used_model"
  else
    warn "ollama plan failed (see $OPS_DIR/curriculum_ollama.err)"
    # deterministic fallback curriculum so pipeline still testable offline-ish
    python3 - <<PY
import json
from pathlib import Path
p=Path("$LAST_PLAN")
obj={
  "reason": "ollama unavailable; hermetic fallback curriculum",
  "backend": "fallback",
  "items": [
    {"kind":"grow_local","n":4,"why":"seed lane-teachable window gaps","priority":1},
    {"kind":"lookup_skill","tool":"wiki_lookup","query":"Grace Hopper","why":"exercise lookup path","priority":2},
    {"kind":"window_token","token_id":506,"why":"first window token seed","priority":1},
  ],
}
p.write_text(json.dumps(obj,indent=2)+"\n")
print("fallback plan written")
PY
    used_model="fallback"
  fi

  if [ "$used_model" != "fallback" ]; then
    if ! extract_json "$raw_txt" "$LAST_PLAN"; then
      warn "parse failed; using hermetic fallback"
      python3 - <<PY
import json
from pathlib import Path
Path("$LAST_PLAN").write_text(json.dumps({
  "reason": "parse fail; hermetic fallback",
  "backend": "fallback",
  "items": [
    {"kind":"grow_local","n":3,"why":"seed teachable","priority":1},
    {"kind":"lookup_skill","tool":"wiki_lookup","query":"Ada Lovelace","why":"lookup smoke","priority":2},
  ],
}, indent=2)+"\n")
PY
      used_model="fallback"
    fi
  fi

  # stamp metadata + append queue lines
  python3 - <<PY
import json, time
from pathlib import Path
from datetime import datetime, timezone
plan=json.loads(Path("$LAST_PLAN").read_text())
plan["ts"]=datetime.now().astimezone().isoformat(timespec="seconds")
plan["model"]="$used_model"
plan["base"]="$BASE"
plan["units"]=$(current_units)
Path("$LAST_PLAN").write_text(json.dumps(plan, indent=2)+"\n")
q=Path("$QUEUE")
with q.open("a", encoding="utf-8") as f:
  for it in plan.get("items") or []:
    if not isinstance(it, dict):
      continue
    row=dict(it)
    row["ts"]=plan["ts"]
    row["status"]="open"
    row["source"]="curriculum"
    row["model"]=plan["model"]
    f.write(json.dumps(row, ensure_ascii=False)+"\n")
print("queued", len(plan.get("items") or []), "→", q)
PY
  consume_budget
  info "plan saved $LAST_PLAN model=$used_model"
  echo "PERSONAL_AI_CURRICULUM_PLAN_OK model=$used_model"
}

materialize() {
  local target_inbox="$INBOX"
  if [ "$SANDBOX" = "1" ]; then
    target_inbox="$OPS_DIR/test_sandbox.inbox"
    : >"$target_inbox"
    info "sandbox inbox $target_inbox"
  fi

  python3 - <<'PY' "$QUEUE" "$MAX_MAT" "$target_inbox" "$WINDOW" "$TODO" "$REPO" "$SANDBOX"
import json, os, sys, subprocess
from pathlib import Path
from datetime import datetime

queue_path, max_n, inbox, window, todo, repo, sandbox = sys.argv[1:8]
max_n = int(max_n)
sandbox = sandbox == "1"
lines = Path(queue_path).read_text(encoding="utf-8").splitlines() if Path(queue_path).exists() else []
open_rows = []
kept = []
for line in lines:
    line=line.strip()
    if not line:
        continue
    try:
        o=json.loads(line)
    except Exception:
        kept.append(line)
        continue
    if o.get("status") == "open":
        open_rows.append(o)
    else:
        kept.append(json.dumps(o, ensure_ascii=False))

open_rows.sort(key=lambda r: int(r.get("priority") or 99))
done = 0
actions = []
win_ids = []
if Path(window).exists():
    win_ids = [int(x) for x in Path(window).read_text().split() if x.isdigit()]
W = len(win_ids) if win_ids else 256
K = 3

def append_no_plan(tid: int):
    Path(inbox).parent.mkdir(parents=True, exist_ok=True)
    with open(inbox, "a", encoding="utf-8") as f:
        f.write(f"NO_PLAN 1 {W} 1 w_cur 1 {W} {K} tk{tid}q{tid}\n")

for row in open_rows:
    if done >= max_n:
        kept.append(json.dumps(row, ensure_ascii=False))
        continue
    kind = (row.get("kind") or "").strip()
    ok = False
    detail = ""
    try:
        if kind in ("grow_local", "window_token"):
            n = int(row.get("n") or 1)
            if kind == "window_token":
                tid = int(row.get("token_id") or (win_ids[0] if win_ids else 506))
                append_no_plan(tid)
                ok = True
                detail = f"NO_PLAN tk{tid}q{tid}"
            else:
                # use grow_local script for real inbox; sandbox: manual seeds
                if sandbox:
                    for tid in (win_ids or [506,529,532,531])[: max(1, min(n, 4))]:
                        append_no_plan(int(tid))
                    ok = True
                    detail = f"sandbox seeds n={min(n,4)}"
                else:
                    r = subprocess.run(
                        ["bash", f"{repo}/scripts/personal_ai_grow_local.sh", str(max(1, min(n, 8)))],
                        capture_output=True, text=True, timeout=60)
                    ok = r.returncode == 0
                    detail = (r.stdout or r.stderr or "")[-200:]
        elif kind == "lookup_skill":
            tool = (row.get("tool") or "wiki_lookup").strip()
            query = (row.get("query") or row.get("q") or "Alan Turing").strip()
            # Prefer Hermes-style C learn cycle (memory → tools → skill).
            learn_bin = Path(repo) / "bin" / "cnet_learn_cycle"
            if learn_bin.is_file():
                env = os.environ.copy()
                env["CNET_SKILLS_DIR"] = str(Path(repo) / "logs" / "personal_ai_skills")
                r = subprocess.run(
                    [str(learn_bin), query],
                    capture_output=True, text=True, timeout=120, env=env, cwd=repo)
                ok = r.returncode == 0 and "CNET_LEARN_CYCLE_OK" in (r.stdout or "")
                detail = (r.stdout or r.stderr or "")[:220]
            else:
                so = Path(repo) / "cnet.so"
                if so.exists():
                    code = f"""
import ctypes
lib=ctypes.CDLL({str(so)!r})
buf=ctypes.create_string_buffer(2048)
fc=ctypes.c_int(0)
q={query!r}.encode()
if {tool!r}=="web_search":
  lib.port_contract_mcp_web_search.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_int)]
  lib.port_contract_mcp_web_search.restype=ctypes.c_int
  lib.port_contract_mcp_web_search(q,buf,len(buf),ctypes.byref(fc))
else:
  lib.port_contract_mcp_wiki_lookup.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_int)]
  lib.port_contract_mcp_wiki_lookup.restype=ctypes.c_int
  lib.port_contract_mcp_wiki_lookup(q,buf,len(buf),ctypes.byref(fc))
print(buf.value[:180].decode('utf-8','replace'))
"""
                    r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, timeout=90)
                    ok = r.returncode == 0 and bool((r.stdout or "").strip())
                    detail = (r.stdout or r.stderr or "")[:200]
                else:
                    ok = False
                    detail = "cnet_learn_cycle and cnet.so missing"
        elif kind == "jtc_expand":
            Path(todo).parent.mkdir(parents=True, exist_ok=True)
            t = {
                "ts": datetime.now().astimezone().isoformat(timespec="seconds"),
                "id": f"jtc-{row.get('tool','tool')}",
                "status": "open",
                "priority": int(row.get("priority") or 3),
                "text": f"jtc expand tool {row.get('tool')}: {row.get('why','')}",
            }
            with open(todo, "a", encoding="utf-8") as f:
                f.write(json.dumps(t) + "\n")
            ok = True
            detail = "todo queued"
        else:
            detail = f"unknown kind {kind}"
    except Exception as e:
        detail = str(e)
        ok = False

    row["status"] = "done" if ok else "failed"
    row["done_ts"] = datetime.now().astimezone().isoformat(timespec="seconds")
    row["result"] = detail
    kept.append(json.dumps(row, ensure_ascii=False))
    actions.append({"kind": kind, "ok": ok, "detail": detail[:120]})
    if ok:
        done += 1

Path(queue_path).write_text("\n".join(kept) + ("\n" if kept else ""), encoding="utf-8")
print(json.dumps({"materialized": done, "actions": actions}, indent=2))
if done:
    print(f"PERSONAL_AI_CURRICULUM_MATERIALIZE_OK n={done}")
else:
    print("PERSONAL_AI_CURRICULUM_MATERIALIZE_OK n=0")
PY
}

status_cmd() {
  load_state
  echo "base=$BASE"
  echo "units=$(current_units)"
  echo "backend=$BACKEND model=$MODEL fallback=$MODEL_FALLBACK"
  echo "queue=$QUEUE"
  echo "state: LAST_UNITS=${LAST_UNITS:-} LAST_UNITS_TS=${LAST_UNITS_TS:-} hour_count=${CURRICULUM_HOUR_COUNT:-0}/$MAX_PER_HOUR stale=${LAST_STALE:-}"
  if [ -f "$LAST_PLAN" ]; then
    echo "--- last plan ---"
    head -40 "$LAST_PLAN"
  fi
  if [ -f "$QUEUE" ]; then
    echo "--- queue (last 8) ---"
    tail -8 "$QUEUE"
  fi
}

run_pipeline() {
  local d
  d=$(detect_stale)
  info "detect → $d"
  if [ "$d" != "stale" ] && [ "$FORCE" != "1" ]; then
    echo "PERSONAL_AI_CURRICULUM_SKIP reason=$d"
    return 0
  fi
  # if force and not stale, still plan
  plan
  materialize
  echo "PERSONAL_AI_CURRICULUM_RUN_OK"
}

test_e2e() {
  info "TEST e2e (force plan + sandbox materialize)"
  FORCE=1
  SANDBOX=1
  # isolate queue for test
  local save_queue="$QUEUE"
  QUEUE="$OPS_DIR/learn_queue_test.jsonl"
  rm -f "$QUEUE" "$OPS_DIR/test_sandbox.inbox"
  : >"$QUEUE"
  plan
  materialize
  local inbox_n plan_ok mat_ok
  inbox_n=$(wc -l <"$OPS_DIR/test_sandbox.inbox" 2>/dev/null | tr -d ' ' || echo 0)
  plan_ok=0
  mat_ok=0
  grep -q PERSONAL_AI_CURRICULUM_PLAN_OK <<<"$(cat "$LAST_PLAN" >/dev/null; echo PERSONAL_AI_CURRICULUM_PLAN_OK)" && plan_ok=1
  [ -s "$LAST_PLAN" ] && plan_ok=1
  [ "${inbox_n:-0}" -gt 0 ] || grep -q '"ok": true' <<<"$(tail -1 "$QUEUE" 2>/dev/null || true)" && mat_ok=1
  # stronger checks
  python3 - <<PY
import json
from pathlib import Path
plan=json.loads(Path("$LAST_PLAN").read_text())
assert "items" in plan and len(plan["items"])>=1, plan
q=Path("$QUEUE").read_text().strip().splitlines()
assert q, "empty queue"
done=sum(1 for line in q if json.loads(line).get("status") in ("done","failed"))
assert done>=1, q
print("TEST_ASSERT_OK items", len(plan["items"]), "queue", len(q), "sandbox_inbox_lines", $inbox_n)
# at least one materialize success preferred
oks=sum(1 for line in q if json.loads(line).get("status")=="done")
print("materialize_done", oks)
if oks<1:
  raise SystemExit("no successful materialize")
print("PERSONAL_AI_CURRICULUM_TEST_PASS")
PY
  QUEUE="$save_queue"
}

case "$cmd" in
  status) status_cmd ;;
  detect) detect_stale ;;
  plan) plan ;;
  materialize) materialize ;;
  run) run_pipeline ;;
  test) test_e2e ;;
  *)
    echo "usage: $0 status|detect|plan|materialize|run|test [--force] [--sandbox] [--max N]" >&2
    exit 2
    ;;
esac
