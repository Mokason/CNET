#!/usr/bin/env bash
# Fixed native harvest gate: new private packs only, external fixture gold.
set -euo pipefail
umask 077
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/packaging/CNET-Minimal/runtime_common.sh"
BIN="$(realpath -m -- "${BIN_DIR:-$ROOT/bin}")"
cnet_require_bins
WORK="$(mktemp -d /tmp/cnet-harvest.XXXXXXXX)"
PACKS=$(cnet_new_path "${1:-$WORK/packs}" output)
LOG="${CNET_HARVEST_LOG:-$ROOT/logs/cert_coverage_harvest.log}"
FIXTURES="$ROOT/packaging/CNET-Minimal/coverage_gold.tsv"
mkdir -p "$(dirname "$LOG")" "$(dirname "$PACKS")"
mkdir "$PACKS"
cnet_offline
cd "$WORK"
{
  echo "CERT_COVERAGE_HARVEST_BEGIN packs=$PACKS fixture_only=1"
  cnet_marker ROE_DAILY_PACKS_SEED_OK "$BIN/roe_daily_packs_seed" --root "$PACKS"
  cnet_marker DOMAIN_ROUTE_PASS "$BIN/roe_domain_route" --test
  cnet_marker EVOLVE_BLOCKLIST_OK "$BIN/roe_evolve_tick" --selftest
  while IFS=$'\t' read -r q answer; do
    [[ -n $q && $q != \#* ]] || continue
    "$BIN/roe_gold_put" "$q" "$answer"
    # Demand carries no answer: the only labels are the reviewed gold above.
    jq -nc --arg q "$q" '{query:$q,source:"fixture_demand"}' >>"$PACKS/miss_log.jsonl"
  done < "$FIXTURES"
  cnet_marker ROE_EVOLVE_TICK_PASS "$BIN/roe_evolve_tick" --gold-only --reviewer --max-promotes 6
  jq -e '.dry_run == false and (.promoted|length) == 6 and all(.promoted[]; .reason == "gold_file")' "$PACKS/EVOLVE_TICK.json" >/dev/null || cnet_fail 'fixture_admission_count'
  cnet_check_queries
  echo "CERT_COVERAGE_HARVEST_PASS fail=0 packs=$PACKS fixture_only=1"
} 2>&1 | tee "$LOG"
