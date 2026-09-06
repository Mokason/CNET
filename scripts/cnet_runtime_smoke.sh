#!/usr/bin/env bash
# Offline native fixture smoke; all writes are on a private pack copy.
set -euo pipefail
ROOT="$(realpath -e -- "${CNET_MINIMAL_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}")"
if [[ -f $ROOT/scripts/cnet_runtime_common.sh ]]; then
  source "$ROOT/scripts/cnet_runtime_common.sh"
else
  source "$ROOT/packaging/CNET-Minimal/runtime_common.sh"
fi
cnet_prepare_runtime
LOG="${CNET_SMOKE_LOG:-$WORK/smoke.log}"
mkdir -p "$(dirname "$LOG")"
{
  echo "CNET_RUNTIME_SMOKE_BEGIN root=$ROOT work=$WORK fixture_only=1"
  cnet_marker DOMAIN_ROUTE_PASS "$BIN/roe_domain_route" --test
  cnet_marker EVOLVE_BLOCKLIST_OK "$BIN/roe_evolve_tick" --selftest
  cnet_check_queries
  cnet_marker ROE_CHAIN_THINK_OK "$BIN/roe_chain_think" --no-act --root "$PACKS" --gov "$WORK/logs" 'who are you'
  [[ -s $WORK/logs/chain_last.txt ]] || cnet_fail 'chain_trace_missing'
  before=$(sha256sum "$PACKS/pack_personal/catalog.jsonl" "$PACKS/evolve_state.json")
  cnet_marker ROE_EVOLVE_TICK_PASS "$BIN/roe_evolve_tick" --dry-run --gold-only --reviewer
  jq -e '.dry_run == true and (.promoted|length) == 0 and any(.skipped[]; .reason|startswith("block"))' "$PACKS/EVOLVE_TICK.json" >/dev/null || cnet_fail 'dry_run_report'
  [[ $before == "$(sha256sum "$PACKS/pack_personal/catalog.jsonl" "$PACKS/evolve_state.json")" ]] || cnet_fail 'dry_run_changed_admissions'
  cnet_marker STREAM_IX_E2E_BENCH_PASS "$BIN/stream_ix_e2e_bench"
  echo "CNET_RUNTIME_SMOKE_PASS fixture_only=1 local=9 ood=4 native_dry_run=1 work=$WORK"
} 2>&1 | tee "$LOG"
