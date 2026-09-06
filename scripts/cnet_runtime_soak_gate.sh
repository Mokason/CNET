#!/usr/bin/env bash
# Two native gardener ticks and repeated fixed LOCAL/OOD checks, offline.
set -euo pipefail
ROOT="$(realpath -e -- "${CNET_MINIMAL_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}")"
if [[ -f $ROOT/scripts/cnet_runtime_common.sh ]]; then
  source "$ROOT/scripts/cnet_runtime_common.sh"
else
  source "$ROOT/packaging/CNET-Minimal/runtime_common.sh"
fi
cnet_prepare_runtime
LOG="${CNET_SOAK_LOG:-$WORK/soak.log}"
mkdir -p "$(dirname "$LOG")"
{
  echo "CNET_RUNTIME_SOAK_BEGIN root=$ROOT work=$WORK fixture_only=1"
  cnet_marker EVOLVE_BLOCKLIST_OK "$BIN/roe_evolve_tick" --selftest
  before=$(sha256sum "$PACKS/pack_personal/catalog.jsonl")
  ticks=$(jq -er '.ticks' "$PACKS/evolve_state.json")
  for i in 1 2; do
    cnet_check_queries
    cnet_marker ROE_EVOLVE_TICK_PASS "$BIN/roe_evolve_tick" --gold-only --reviewer
    jq -e '.dry_run == false and (.promoted|length) == 0 and any(.skipped[]; .reason|startswith("block"))' "$PACKS/EVOLVE_TICK.json" >/dev/null || cnet_fail "tick_report tick=$i"
    jq -e --argjson expected "$((ticks + i))" '.ticks == $expected' "$PACKS/evolve_state.json" >/dev/null || cnet_fail "tick_state tick=$i"
    [[ $before == "$(sha256sum "$PACKS/pack_personal/catalog.jsonl")" ]] || cnet_fail "personal_changed tick=$i"
  done
  jq -se 'length == 6 and all(.[]; (.answer|ascii_downcase|startswith("abstain")|not) and (.pattern|test("mystic|zz99")|not))' "$PACKS/pack_personal/catalog.jsonl" >/dev/null || cnet_fail 'personal_polluted'
  echo "CNET_RUNTIME_SOAK_GATE_PASS fixture_only=1 ticks=2 local=18 ood=8 live_calls=0 acquisition=WITHHELD work=$WORK"
} 2>&1 | tee "$LOG"
