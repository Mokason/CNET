#!/usr/bin/env bash
# 12h progress report for the 24/7 auto-teach loop.
#
# Every metric here used to be a byte-marker proxy: units_proxy counted
# b"acq_"/b"json_toolcall"/b"hyb_struct" substrings in the base and read 355 on
# both sides of a window in which the real unit count moved 102 -> 120, so the
# report announced "zero progress" during an hour of genuine learning. It also
# counted raw fault_lines as activity when those lines were duplicate synthetic
# seed records.
#
# Now: authoritative unit count from cnb_audit, gap states from the ledger,
# DISTINCT fault pairs rather than lines, and closure totals from the lane's own
# hill-climb ledger. JSON via jq (was python3).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
BASE="$REPO/soul_gemma4v2_final.cnb"
GOV="$REPO/logs/autoteach"
mkdir -p "$GOV"

units_count() {
  if [[ -x "$REPO/bin/cnb_audit" && -f "$BASE" ]]; then
    "$REPO/bin/cnb_audit" "$BASE" --count 2>/dev/null \
      | sed -n 's/.*units=\([0-9]*\).*/\1/p' | head -1
  else
    echo 0
  fi
}

# Partition the ledger: every row lands in exactly one bucket.
# waiting_oracle is an ANNOTATION on a row, not a third state — same bug/fix
# as governor_miss_ingest.sh. bin/governor_autonomous.collect() is the reference
# (tests/test_metric_honesty.c pins the partition).
gap_states_json() {
  local open_n=0 closed_n=0 waiting=0 lineno=0
  local p="${BASE}.gaps.txt"
  if [[ -f "$p" ]]; then
    while IFS= read -r line || [[ -n "$line" ]]; do
      lineno=$((lineno + 1))
      [[ "$lineno" -lt 3 ]] && continue
      # shellcheck disable=SC2206
      parts=($line)
      if [[ "$line" == *waiting_oracle* || "$line" == *waiting_charter* ]]; then
        waiting=$((waiting + 1))
      elif [[ "${parts[1]:-}" == "1" ]]; then
        open_n=$((open_n + 1))
      elif [[ "${parts[1]:-}" == "2" ]]; then
        closed_n=$((closed_n + 1))
      fi
    done <"$p"
  fi
  jq -nc --argjson open "$open_n" --argjson closed "$closed_n" --argjson waiting "$waiting" \
    '{open:$open, closed:$closed, waiting:$waiting}'
}

faults_json() {
  local p="$REPO/logs/cnet_faults.jsonl"
  if [[ ! -f "$p" ]]; then
    echo '{"lines":0,"distinct":0,"units":0}'
    return
  fi
  jq -s '
    length as $n |
    (map(.unit) | unique | length) as $u |
    (map([.unit, (.in//[]), (.tgt//[])]) | unique | length) as $d |
    {lines:$n, distinct:$d, units:$u}
  ' "$p" 2>/dev/null || echo '{"lines":0,"distinct":0,"units":0}'
}

lane_totals_json() {
  local since="$1"
  local p="${BASE}.hill_climb.jsonl"
  if [[ ! -f "$p" ]]; then
    echo '{"examined":0,"closed":0,"deferred":0,"no_oracle":0,"ticks":0}'
    return
  fi
  jq -s --argjson since "$since" '
    map(select((.ts//0) >= $since)) as $r |
    {
      examined: ($r|map(.examined//0)|add//0),
      closed: ($r|map(.closed//0)|add//0),
      deferred: ($r|map(.deferred//0)|add//0),
      no_oracle: ($r|map(.no_oracle//0)|add//0),
      ticks: ($r|length)
    }
  ' "$p" 2>/dev/null || echo '{"examined":0,"closed":0,"deferred":0,"no_oracle":0,"ticks":0}'
}

moe_json() {
  local p="$REPO/artifacts/moe/state.json"
  if [[ ! -f "$p" ]]; then echo '{}'; return; fi
  jq -c '{step,heldout_ce,best_ce,h1,h2,gap_to_floor,below_h1,certified,eval_n,device}' "$p" 2>/dev/null || echo '{}'
}

svc() { systemctl --user is-active "$1" 2>/dev/null || echo inactive; }

u=$(units_count); u=${u:-0}
gaps=$(gap_states_json)
f=$(faults_json)
moe=$(moe_json)
lora_n=0
[[ -d "$REPO/logs/lora_store" ]] && lora_n=$(find "$REPO/logs/lora_store" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l | tr -d ' ')
base_size=0
[[ -f "$BASE" ]] && base_size=$(wc -c <"$BASE" | tr -d ' ')
ts=$(date -Iseconds)
t_unix=$(date +%s)

now=$(jq -nc \
  --argjson moe "$moe" \
  --arg ts "$ts" \
  --argjson t_unix "$t_unix" \
  --argjson units "$u" \
  --argjson gaps "$gaps" \
  --argjson fault_lines "$(jq '.lines' <<<"$f")" \
  --argjson fault_distinct "$(jq '.distinct' <<<"$f")" \
  --argjson lora_files "$lora_n" \
  --argjson base_size "$base_size" \
  --arg bonsai "$(svc bonsai-server)" \
  --arg lane "$(svc cnet-personal-ai-lane)" \
  --arg autoteach_timer "$(svc cnet-autoteach.timer)" \
  --arg governor_timer "$(svc cnet-governor.timer)" \
  '{
    moe: $moe, ts: $ts, t_unix: $t_unix, units: $units, gaps: $gaps,
    fault_lines: $fault_lines, fault_distinct: $fault_distinct,
    lora_files: $lora_files, base_size: $base_size,
    services: {bonsai:$bonsai, lane:$lane, autoteach_timer:$autoteach_timer, governor_timer:$governor_timer},
    schema: 3
  }')

bp="$GOV/baseline_12h.json"
b0='{}'
if [[ -f "$bp" ]]; then
  b0=$(jq -c . "$bp" 2>/dev/null || echo '{}')
fi
if [[ "$(jq -r '.schema // 0' <<<"$b0")" != "3" ]]; then
  # schema 1 used byte-marker proxies; schema 2 double-counted waiting rows
  # as open. Re-baseline rather than print a fabricated delta.
  b0=$(jq -c --argjson now "$now" \
    '$now + {horizon_h:12, due_unix: ($now.t_unix + 12*3600), rebaselined_from_proxy_schema: true}')
  jq . <<<"$b0" >"$bp"
fi

elapsed_h=$(jq -nr --argjson now "$now" --argjson b0 "$b0" \
  '(($now.t_unix - $b0.t_unix)/3600 * 100 | floor) / 100')
lane=$(lane_totals_json "$(jq -r '.t_unix' <<<"$b0")")

delta=$(jq -nc --argjson now "$now" --argjson b0 "$b0" '{
  units: ($now.units - $b0.units),
  gaps_closed: ($now.gaps.closed - $b0.gaps.closed),
  gaps_open: ($now.gaps.open - $b0.gaps.open),
  gaps_waiting: ($now.gaps.waiting - $b0.gaps.waiting),
  fault_lines: ($now.fault_lines - $b0.fault_lines),
  fault_distinct: ($now.fault_distinct - $b0.fault_distinct),
  lora_files: ($now.lora_files - $b0.lora_files),
  base_size: ($now.base_size - $b0.base_size)
}')
units_per_h=$(jq -nr --argjson d "$delta" --argjson e "$elapsed_h" \
  'if $e > 0 then (($d.units / $e)*100|floor)/100 else 0 end')

rep=$(jq -nc \
  --argjson baseline "$b0" \
  --argjson now "$now" \
  --argjson delta "$delta" \
  --argjson elapsed_h "$elapsed_h" \
  --argjson units_per_h "$units_per_h" \
  --argjson lane "$lane" \
  '{baseline:$baseline, now:$now, delta:$delta, elapsed_h:$elapsed_h, units_per_h:$units_per_h, lane_since_baseline:$lane}')
jq . <<<"$rep" >"$GOV/report_12h.json"

close_rate=$(jq -nr --argjson lane "$lane" \
  'if $lane.examined > 0 then $lane.closed / $lane.examined else 0 end')

m0=$(jq -c '.moe // {}' <<<"$b0")
m1=$(jq -c '.moe // {}' <<<"$now")
if [[ "$(jq 'length' <<<"$m1")" -gt 0 ]]; then
  d() { jq -nr --argjson m0 "$m0" --argjson m1 "$m1" --arg k "$1" '
    ($m0[$k]) as $a | ($m1[$k]) as $b |
    if $a != null then "\($a) → \($b)" else "\($b)" end'; }
  moe_md=$(cat <<EOF
## Learning substrate: transformer-MoE vs analytic entropy floor

The metric that cannot be gamed by memorisation — the markov2 stream is sampled
fresh every sequence and H2 is computed from the source, so CE falls only on
genuine generalisation. Certified means held-out CE below H1, the order-1
plateau, which requires the attention to use the previous token.

- step: $(d step)
- held-out CE: $(d heldout_ce)   (uniform ~4.159)
- floors: H1 $(jq -r '.h1' <<<"$m1") (order-1 plateau) → H2 $(jq -r '.h2' <<<"$m1") (true floor)
- gap to floor: $(d gap_to_floor)
- below H1 (certified): $(jq -r '.below_h1' <<<"$m1") / $(jq -r '.certified' <<<"$m1")
- eval_n $(jq -r '.eval_n' <<<"$m1") on $(jq -r '.device' <<<"$m1")
EOF
)
else
  moe_md="## Learning substrate

- no MoE state yet (run \`make moe_tick\`)
"
fi

cat >"$GOV/report_12h.md" <<EOF
# CNET auto-teach 12h report

- when: $(jq -r '.ts' <<<"$now")
- elapsed_h: $elapsed_h

$moe_md
## Lookup coverage (gap lane — secondary)
- units: $(jq -r '.units' <<<"$b0") → $(jq -r '.units' <<<"$now") ($(jq -r '.units' <<<"$delta" | awk '{printf "%+d",$1}'))  [${units_per_h}/h]
- gaps closed: $(jq -r '.gaps.closed' <<<"$b0") → $(jq -r '.gaps.closed' <<<"$now") ($(jq -r '.gaps_closed' <<<"$delta" | awk '{printf "%+d",$1}'))
- gaps open: $(jq -r '.gaps.open' <<<"$b0") → $(jq -r '.gaps.open' <<<"$now") ($(jq -r '.gaps_open' <<<"$delta" | awk '{printf "%+d",$1}'))
- gaps waiting oracle: $(jq -r '.gaps.waiting' <<<"$b0") → $(jq -r '.gaps.waiting' <<<"$now") ($(jq -r '.gaps_waiting' <<<"$delta" | awk '{printf "%+d",$1}'))

## Lane activity since baseline
- ticks $(jq -r '.ticks' <<<"$lane"), examined $(jq -r '.examined' <<<"$lane"), closed $(jq -r '.closed' <<<"$lane"), deferred $(jq -r '.deferred' <<<"$lane"), no_oracle $(jq -r '.no_oracle' <<<"$lane")
- close rate: $(awk -v r="$close_rate" 'BEGIN{printf "%.0f%%", r*100}')

## Fault bus
- lines: $(jq -r '.fault_lines' <<<"$b0") → $(jq -r '.fault_lines' <<<"$now") ($(jq -r '.fault_lines' <<<"$delta" | awk '{printf "%+d",$1}'))
- DISTINCT (unit,in,tgt): $(jq -r '.fault_distinct' <<<"$b0") → $(jq -r '.fault_distinct' <<<"$now") ($(jq -r '.fault_distinct' <<<"$delta" | awk '{printf "%+d",$1}'))
- lora adapters: $(jq -r '.lora_files' <<<"$b0") → $(jq -r '.lora_files' <<<"$now") ($(jq -r '.lora_files' <<<"$delta" | awk '{printf "%+d",$1}'))

Line growth without distinct growth means duplicate records, not new signal.

## Services now
- bonsai: $(jq -r '.services.bonsai' <<<"$now")
- lane: $(jq -r '.services.lane' <<<"$now")
- autoteach_timer: $(jq -r '.services.autoteach_timer' <<<"$now")
- governor_timer: $(jq -r '.services.governor_timer' <<<"$now")
EOF

echo "WROTE $GOV/report_12h.json"
jq -nc --argjson elapsed_h "$elapsed_h" --argjson units_per_h "$units_per_h" \
  --argjson delta "$delta" --argjson lane "$lane" \
  '{elapsed_h:$elapsed_h, units_per_h:$units_per_h, delta:$delta, lane:$lane}'
