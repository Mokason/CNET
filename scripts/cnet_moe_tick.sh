#!/usr/bin/env bash
# CNET 24/7 learning tick — transformer-MoE block against a held-out entropy floor.
#
# Replaces the gap lane as the loop's learning substrate. The gap lane's unit is
# a one-hot -> one-hot table row certified by exact replay; a one-hot input space
# has no metric, so more rows never become generalisation. Here the artefact is a
# checkpoint and the predicate is:
#
#     certified := held-out CE below H1, the analytic order-1 plateau
#
# H1 cannot be reached by memorising the marginal — beating it requires the
# attention to use the previous token — and the source is sampled fresh forever,
# so there is no training set to overfit.
#
# Two checkpoints, deliberately:
#   live.ckpt  training always advances from here (SGD is noisy; ratcheting the
#              live weights on every worse tick would stop learning outright)
#   best.ckpt  replaced only when held-out CE improves — the certified artefact
# A divergence guard rolls live back to best if CE blows up or goes non-finite.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

DIR="${CNET_MOE_DIR:-$REPO/artifacts/moe}"
LIVE="$DIR/live.ckpt"
BEST="$DIR/best.ckpt"
STATE="$DIR/state.json"
EVID="$DIR/evidence.jsonl"
LOG="$DIR/tick_$(date +%Y%m%d).log"
LOCK="$DIR/tick.lock"
mkdir -p "$DIR"

exec 9>"$LOCK"
if ! flock -n 9; then echo "moe_tick: another tick holds the lock; exit"; exit 0; fi

STEPS="${MOE_STEPS:-400}"
EVAL_N="${MOE_EVAL_N:-64}"
LR="${MOE_LR:-0.003}"
DIVERGE_FACTOR="${MOE_DIVERGE_FACTOR:-1.25}"

if [[ ! -x bin/moe_xf_tick ]]; then
  echo "moe_tick: bin/moe_xf_tick missing (make moe_xf_tick)" >&2
  exit 1
fi

out=$(MOE_CKPT="$LIVE" MOE_CKPT_OUT="$LIVE.cand" MOE_STEPS="$STEPS" \
      MOE_EVAL_N="$EVAL_N" MOE_LR="$LR" \
      timeout "${MOE_TIMEOUT:-3000}" ./bin/moe_xf_tick 2>&1) || {
  echo "$out" | tail -5 >&2
  echo "moe_tick: trainer failed" >&2
  exit 1
}
echo "[$(date -Iseconds)] $out" >>"$LOG"

# Post-process MOE_TICK JSON line (jq + bash; was python3)
line=$(printf '%s\n' "$out" | grep '^MOE_TICK ' | tail -1 || true)
if [[ -z "$line" ]]; then
  echo "moe_tick: no MOE_TICK line" >&2
  exit 1
fi
t_json=${line#MOE_TICK }
CAND="${LIVE}.cand"
factor="${MOE_DIVERGE_FACTOR:-1.25}"

st='{}'
if [[ -f "$STATE" ]]; then
  st=$(jq -c . "$STATE" 2>/dev/null || echo '{}')
fi

ce=$(jq -r '.heldout_ce' <<<"$t_json")
h1=$(jq -r '.h1' <<<"$t_json")
h2=$(jq -r '.h2' <<<"$t_json")
eval_n=$(jq -r '.eval_n' <<<"$t_json")
step_to=$(jq -r '.step_to' <<<"$t_json")
train_ce=$(jq -r '.train_ce' <<<"$t_json")
uniform=$(jq -r '.uniform' <<<"$t_json")
device=$(jq -r '.device' <<<"$t_json")
secs=$(jq -r '.secs' <<<"$t_json")

prev_n=$(jq -r '.eval_n // empty' <<<"$st")
basis_changed=0
if [[ -n "$prev_n" && "$prev_n" != "$eval_n" ]]; then
  basis_changed=1
fi

best=""
if [[ "$basis_changed" -eq 0 ]]; then
  best=$(jq -r '.best_ce // empty' <<<"$st")
fi
if [[ "$basis_changed" -eq 1 ]]; then
  st=$(jq -c --arg m "eval_n ${prev_n} -> ${eval_n}; previous best_ce discarded" \
    '.basis_reset=$m | del(.best_ce)' <<<"$st")
  best=""
fi

action="advanced"
if ! jq -e '.heldout_ce | numbers | isfinite' <<<"$t_json" >/dev/null 2>&1; then
  action="diverged_rollback"
elif [[ -n "$best" ]] && awk -v c="$ce" -v b="$best" -v f="$factor" \
    'BEGIN{ exit (c > b*f) ? 0 : 1 }'; then
  action="diverged_rollback"
fi

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    openssl dgst -sha256 "$1" | awk '{print $NF}'
  fi
}

if [[ "$action" == "diverged_rollback" ]]; then
  rm -f "$CAND"
  if [[ -f "$BEST" ]]; then
    cp -f "$BEST" "$LIVE"
  fi
else
  mv -f "$CAND" "$LIVE"
  if [[ -z "$best" ]] || awk -v c="$ce" -v b="$best" 'BEGIN{ exit (c < b) ? 0 : 1 }'; then
    cp -f "$LIVE" "$BEST"
    action="promoted"
  fi
fi

promoted=0
[[ "$action" == "promoted" ]] && promoted=1
below_h1=0
awk -v c="$ce" -v h="$h1" 'BEGIN{ exit (c < h) ? 0 : 1 }' && below_h1=1
certified=0
[[ "$promoted" -eq 1 && "$below_h1" -eq 1 ]] && certified=1

ts=$(date +%Y-%m-%dT%H:%M:%S%z)
gap=$(awk -v c="$ce" -v h="$h2" 'BEGIN{ print c-h }')

st=$(jq -c \
  --arg ts "$ts" \
  --argjson step "$step_to" \
  --argjson heldout_ce "$ce" \
  --argjson train_ce "$train_ce" \
  --argjson h1 "$h1" \
  --argjson h2 "$h2" \
  --argjson uniform "$uniform" \
  --argjson gap_to_floor "$gap" \
  --argjson below_h1 "$below_h1" \
  --arg action "$action" \
  --arg device "$device" \
  --argjson eval_n "$eval_n" \
  --argjson secs "$secs" \
  --argjson certified "$certified" \
  '.ts=$ts | .step=$step | .heldout_ce=$heldout_ce | .train_ce=$train_ce
   | .h1=$h1 | .h2=$h2 | .uniform=$uniform | .gap_to_floor=$gap_to_floor
   | .below_h1=($below_h1==1) | .action=$action | .device=$device
   | .eval_n=$eval_n | .secs=$secs | .certified=($certified==1)' <<<"$st")

# Describes THIS tick. Writing certified only on promotion left a stale True
# next to a held-out CE above H1 once a later tick merely advanced — and
# bin/governor_autonomous reads this field as a live signal (moe_certified).

if [[ "$promoted" -eq 1 ]]; then
  best_sha=$(sha256_file "$BEST")
  st=$(jq -c \
    --argjson ce "$ce" \
    --argjson step "$step_to" \
    --arg sha "$best_sha" \
    '.best_ce=$ce | .best_step=$step | .best_sha256=$sha' <<<"$st")
else
  if ! jq -e '.best_ce != null' <<<"$st" >/dev/null 2>&1; then
    if [[ -n "$best" ]]; then
      st=$(jq -c --argjson ce "$best" '.best_ce=$ce' <<<"$st")
    else
      st=$(jq -c --argjson ce "$ce" '.best_ce=$ce' <<<"$st")
    fi
  fi
fi

jq . <<<"$st" >"$STATE"

if [[ "$promoted" -eq 1 ]]; then
  jq -nc \
    --arg ts "$ts" \
    --argjson step "$step_to" \
    --argjson heldout_ce "$ce" \
    --argjson h1 "$h1" \
    --argjson h2 "$h2" \
    --argjson gap_to_floor "$gap" \
    --argjson certified "$certified" \
    --argjson eval_n "$eval_n" \
    --arg sha "$(jq -r '.best_sha256' <<<"$st")" \
    --arg device "$device" \
    '{
      version: 1, kind: "moe_xf_checkpoint", ts: $ts, step: $step,
      heldout_ce: $heldout_ce, h1: $h1, h2: $h2, gap_to_floor: $gap_to_floor,
      certified_below_h1: ($certified==1), eval_n: $eval_n,
      eval_seed: "0xE7A15EED", source: "markov2_order2_fresh_stream",
      artifact_sha256: $sha, device: $device
    }' >>"$EVID"
fi

best_ce=$(jq -r '.best_ce' <<<"$st")
echo "MOE_TICK_OK $(jq -nc \
  --arg action "$action" \
  --argjson step "$step_to" \
  --argjson heldout_ce "$ce" \
  --argjson best_ce "$best_ce" \
  --argjson h1 "$h1" \
  --argjson h2 "$h2" \
  --argjson gap_to_floor "$gap" \
  --argjson below_h1 "$below_h1" \
  --argjson certified "$certified" \
  '{
    action: $action, step: $step,
    heldout_ce: ($heldout_ce*1e6|round/1e6),
    best_ce: ($best_ce*1e6|round/1e6),
    h1: ($h1*1e6|round/1e6), h2: ($h2*1e6|round/1e6),
    gap_to_floor: ($gap_to_floor*1e6|round/1e6),
    below_h1: ($below_h1==1), certified: ($certified==1)
  }')"
