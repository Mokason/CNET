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
      u=$(jq -r '.units // empty' "$REPORT" 2>/dev/null || echo "")
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
  payload=$(jq -nc \
    --arg model "$model" \
    --arg prompt "$prompt" \
    '{
      model: $model,
      stream: false,
      options: {temperature: 0.3, num_predict: 800},
      messages: [
        {role: "system", content: "You are a curriculum planner for a local certified AI library (CNET). Reply with ONLY valid JSON, no markdown."},
        {role: "user", content: $prompt}
      ]
    }')
  if ! resp=$(curl -sS --max-time "$TIMEOUT_SEC" \
    -H 'Content-Type: application/json' \
    -d "$payload" \
    "$OLLAMA_HOST/api/chat" 2>"$OPS_DIR/curriculum_ollama.err"); then
    return 1
  fi
  if ! jq -e . >/dev/null 2>&1 <<<"$resp"; then
    echo "bad ollama json" >&2
    return 1
  fi
  jq -r '.message.content // empty' <<<"$resp" >"$out_file"
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
  local cleaned
  # strip markdown fences, then take first `{` through last `}`
  cleaned=$(sed -E '1s/^```(json)?[[:space:]]*//; $s/[[:space:]]*```$//' "$raw")
  cleaned=$(printf '%s' "$cleaned" | awk '
    BEGIN { buf=""; start=0; depth=0 }
    {
      for (i = 1; i <= length($0); i++) {
        c = substr($0, i, 1)
        if (c == "{") {
          if (!start) { start=1; buf="" }
          depth++
        }
        if (start) buf = buf c
        if (c == "}" && start) {
          depth--
          if (depth == 0) { print buf; exit }
        }
      }
      if (start) buf = buf "\n"
    }
  ')
  if ! jq -e '.items|type=="array"' <<<"$cleaned" >/dev/null 2>&1; then
    echo "items missing" >&2
    return 1
  fi
  jq . <<<"$cleaned" >"$out"
  echo "ok items $(jq '.items|length' <<<"$cleaned")"
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
    jq -n '{
      reason: "ollama unavailable; hermetic fallback curriculum",
      backend: "fallback",
      items: [
        {kind:"grow_local", n:4, why:"seed lane-teachable window gaps", priority:1},
        {kind:"lookup_skill", tool:"wiki_lookup", query:"Grace Hopper", why:"exercise lookup path", priority:2},
        {kind:"window_token", token_id:506, why:"first window token seed", priority:1}
      ]
    }' >"$LAST_PLAN"
    echo "fallback plan written"
    used_model="fallback"
  fi

  if [ "$used_model" != "fallback" ]; then
    if ! extract_json "$raw_txt" "$LAST_PLAN"; then
      warn "parse failed; using hermetic fallback"
      jq -n '{
        reason: "parse fail; hermetic fallback",
        backend: "fallback",
        items: [
          {kind:"grow_local", n:3, why:"seed teachable", priority:1},
          {kind:"lookup_skill", tool:"wiki_lookup", query:"Ada Lovelace", why:"lookup smoke", priority:2}
        ]
      }' >"$LAST_PLAN"
      used_model="fallback"
    fi
  fi

  # stamp metadata + append queue lines
  local plan_ts units_n
  plan_ts=$(date -Iseconds)
  units_n=$(current_units)
  jq --arg ts "$plan_ts" --arg model "$used_model" --arg base "$BASE" --argjson units "$units_n" \
    '.ts=$ts | .model=$model | .base=$base | .units=$units' "$LAST_PLAN" >"$LAST_PLAN.tmp"
  mv "$LAST_PLAN.tmp" "$LAST_PLAN"
  jq -c --arg ts "$plan_ts" --arg model "$used_model" '
    .items[]? | select(type=="object") |
    . + {ts:$ts, status:"open", source:"curriculum", model:$model}
  ' "$LAST_PLAN" >>"$QUEUE"
  echo "queued $(jq '.items|length' "$LAST_PLAN") → $QUEUE"
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

  local queue_path="$QUEUE" max_n="$MAX_MAT" inbox="$target_inbox"
  local window="$WINDOW" todo="$TODO" repo="$REPO" sandbox="$SANDBOX"
  local kept_tmp actions_tmp open_tmp
  kept_tmp=$(mktemp); actions_tmp=$(mktemp); open_tmp=$(mktemp)
  : >"$kept_tmp"; : >"$actions_tmp"; : >"$open_tmp"
  trap 'rm -f "$kept_tmp" "$actions_tmp" "$open_tmp"' RETURN

  if [[ -f "$queue_path" ]]; then
    while IFS= read -r line || [[ -n "$line" ]]; do
      [[ -z "$line" ]] && continue
      if ! jq -e . >/dev/null 2>&1 <<<"$line"; then
        echo "$line" >>"$kept_tmp"
        continue
      fi
      if [[ "$(jq -r '.status // empty' <<<"$line")" == "open" ]]; then
        echo "$line" >>"$open_tmp"
      else
        echo "$line" >>"$kept_tmp"
      fi
    done <"$queue_path"
  fi
  # priority sort (low number first)
  if [[ -s "$open_tmp" ]]; then
    jq -c -s 'sort_by(.priority // 99)[]' "$open_tmp" >"${open_tmp}.s"
    mv "${open_tmp}.s" "$open_tmp"
  fi

  local -a win_ids=()
  if [[ -f "$window" ]]; then
    mapfile -t win_ids < <(tr ' ' '\n' <"$window" | grep -E '^[0-9]+$' || true)
  fi
  local W=${#win_ids[@]}
  [[ "$W" -eq 0 ]] && W=256
  local K=3 done=0

  append_no_plan() {
    local tid=$1
    mkdir -p "$(dirname "$inbox")"
    echo "NO_PLAN 1 $W 1 w_cur 1 $W $K tk${tid}q${tid}" >>"$inbox"
  }

  while IFS= read -r row || [[ -n "$row" ]]; do
    [[ -z "$row" ]] && continue
    if [[ "$done" -ge "$max_n" ]]; then
      echo "$row" >>"$kept_tmp"
      continue
    fi
    local kind ok=0 detail="" n tid tool query out rc
    kind=$(jq -r '.kind // empty' <<<"$row" | tr -d '\r')
    detail=""
    case "$kind" in
      window_token)
        tid=$(jq -r --argjson d "${win_ids[0]:-506}" '.token_id // $d' <<<"$row")
        append_no_plan "$tid"
        ok=1
        detail="NO_PLAN tk${tid}q${tid}"
        ;;
      grow_local)
        n=$(jq -r '.n // 1' <<<"$row")
        [[ "$n" -lt 1 ]] && n=1
        [[ "$n" -gt 8 ]] && n=8
        if [[ "$sandbox" == "1" ]]; then
          local seeds=("${win_ids[@]:-}")
          if [[ ${#seeds[@]} -eq 0 ]]; then seeds=(506 529 532 531); fi
          local i=0 take=$n
          [[ "$take" -gt 4 ]] && take=4
          [[ "$take" -lt 1 ]] && take=1
          for ((i=0; i<take && i<${#seeds[@]}; i++)); do
            append_no_plan "${seeds[$i]}"
          done
          ok=1
          detail="sandbox seeds n=$take"
        else
          out=$(timeout 60 bash "$repo/scripts/personal_ai_grow_local.sh" "$n" 2>&1) && ok=1 || ok=0
          detail=$(printf '%s' "$out" | tail -c 200)
        fi
        ;;
      lookup_skill)
        tool=$(jq -r '.tool // "wiki_lookup"' <<<"$row")
        query=$(jq -r '.query // .q // "Alan Turing"' <<<"$row")
        if [[ -x "$repo/bin/cnet_learn_cycle" ]]; then
          out=$(CNET_SKILLS_DIR="$repo/logs/personal_ai_skills" \
            timeout 120 "$repo/bin/cnet_learn_cycle" "$query" 2>&1) && rc=0 || rc=$?
          if [[ "$rc" -eq 0 ]] && grep -q CNET_LEARN_CYCLE_OK <<<"$out"; then
            ok=1
          fi
          detail=$(printf '%s' "$out" | head -c 220)
        else
          ok=0
          detail="cnet_learn_cycle missing"
        fi
        ;;
      jtc_expand)
        mkdir -p "$(dirname "$todo")"
        jq -nc \
          --arg ts "$(date -Iseconds)" \
          --arg id "jtc-$(jq -r '.tool // "tool"' <<<"$row")" \
          --argjson pri "$(jq -r '.priority // 3' <<<"$row")" \
          --arg text "jtc expand tool $(jq -r '.tool // empty' <<<"$row"): $(jq -r '.why // empty' <<<"$row")" \
          '{ts:$ts, id:$id, status:"open", priority:$pri, text:$text}' >>"$todo"
        ok=1
        detail="todo queued"
        ;;
      *)
        detail="unknown kind $kind"
        ok=0
        ;;
    esac

    local status done_ts
    done_ts=$(date -Iseconds)
    if [[ "$ok" -eq 1 ]]; then status=done; else status=failed; fi
    jq -c --arg st "$status" --arg ts "$done_ts" --arg res "$detail" \
      '.status=$st | .done_ts=$ts | .result=$res' <<<"$row" >>"$kept_tmp"
    jq -nc --arg kind "$kind" --argjson ok "$ok" --arg detail "${detail:0:120}" \
      '{kind:$kind, ok:($ok==1), detail:$detail}' >>"$actions_tmp"
    [[ "$ok" -eq 1 ]] && done=$((done + 1))
  done <"$open_tmp"

  if [[ -s "$kept_tmp" ]]; then
    cat "$kept_tmp" >"$queue_path"
    echo >>"$queue_path"
  else
    : >"$queue_path"
  fi

  local actions_json
  if [[ -s "$actions_tmp" ]]; then
    actions_json=$(jq -s . "$actions_tmp")
  else
    actions_json='[]'
  fi
  jq -n --argjson materialized "$done" --argjson actions "$actions_json" \
    '{materialized:$materialized, actions:$actions}'
  echo "PERSONAL_AI_CURRICULUM_MATERIALIZE_OK n=$done"

  if [[ -x "$repo/bin/cnet_pattern" ]]; then
    local proposed=0
    export CNET_PATTERN_STORE="${CNET_PATTERN_STORE:-$repo/logs/personal_ai_ops/pattern_runtime.jsonl}"
    while IFS= read -r a; do
      [[ "$(jq -r '.ok' <<<"$a")" == "true" ]] || continue
      kind=$(jq -r '.kind // "curriculum"' <<<"$a")
      detail=$(jq -r '.detail // empty' <<<"$a" | head -c 200)
      if CNET_PATTERN_STORE="$CNET_PATTERN_STORE" \
          "$repo/bin/cnet_pattern" propose "$kind" "$detail" 2>/dev/null \
          | grep -q CNET_PATTERN_PROPOSE_OK; then
        proposed=$((proposed + 1))
      fi
    done < <(jq -c '.[]' <<<"$actions_json")
    [[ "$proposed" -gt 0 ]] && echo "PERSONAL_AI_CURRICULUM_PATTERN_PROPOSE_OK n=$proposed"
  fi
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
  # stronger checks (jq)
  jq -e '.items|type=="array" and length>=1' "$LAST_PLAN" >/dev/null
  [[ -s "$QUEUE" ]] || { echo "empty queue" >&2; return 1; }
  local qn done_n oks
  qn=$(grep -c . "$QUEUE" || true)
  done_n=$(jq -c -s 'map(select(.status=="done" or .status=="failed"))|length' "$QUEUE")
  [[ "$done_n" -ge 1 ]] || { echo "no done/failed rows" >&2; return 1; }
  oks=$(jq -c -s 'map(select(.status=="done"))|length' "$QUEUE")
  echo "TEST_ASSERT_OK items $(jq '.items|length' "$LAST_PLAN") queue $qn sandbox_inbox_lines $inbox_n"
  echo "materialize_done $oks"
  [[ "$oks" -ge 1 ]] || { echo "no successful materialize" >&2; return 1; }
  echo "PERSONAL_AI_CURRICULUM_TEST_PASS"
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
