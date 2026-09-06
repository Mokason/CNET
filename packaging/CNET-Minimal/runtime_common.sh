#!/usr/bin/env bash
# Shared by the three bounded native gates; installed alongside them.
cnet_fail() { echo "CNET_NATIVE_FAIL $*" >&2; exit 1; }

cnet_new_path() {
  local raw="$1" kind="$2" resolved lexical
  [[ -n $raw && $raw != *"'"* && $raw != *$'\n'* && $raw != *$'\r'* && $raw != *$'\t'* ]] || cnet_fail "unsafe_path kind=$kind"
  resolved=$(realpath -m -- "$raw") || cnet_fail "unsafe_path kind=$kind"
  lexical=$(realpath -ms -- "$raw") || cnet_fail "unsafe_path kind=$kind"
  [[ $resolved == "$lexical" ]] || cnet_fail "symlink_path kind=$kind path=$raw"
  [[ ! -e $resolved && ! -L $resolved ]] || cnet_fail "${kind}_exists path=$resolved"
  printf '%s\n' "$resolved"
}

cnet_offline() {
  export ROE_LIVE=0 ROE_LLM=0 ROE_LOOKUP=0 CNET_OPEN_CHAT=0 ROE_OPEN_CHAT=0
  export ROE_NO_THOUGHT=1 ROE_EVOLVE_TEACHER=0 ROE_EVOLVE_REVIEWER=1
  unset ROE_REVIEWER_BIN ROE_CHARTER_BIN CNET_HELD_MODEL_ENDPOINT ROE_FRONT_DOOR_BIAS
  export CNET_ROOT="$WORK" CNET_MINIMAL_ROOT="$ROOT" CNET_PACKS_ROOT="$PACKS"
  export CNET_FRONT_DOOR_BIN="$BIN/roe_front_door" CNET_BLOCKLIST="$ROOT/config/promote_blocklist.txt"
}

cnet_require_bins() {
  local name
  for name in roe_daily_packs_seed roe_front_door roe_domain_route roe_chain_think roe_evolve_tick roe_gold_put stream_ix_e2e_bench; do
    [[ -f $BIN/$name && -x $BIN/$name && ! -L $BIN/$name ]] || cnet_fail "missing_binary name=$name path=$BIN/$name"
  done
  for name in jq timeout sha256sum realpath; do
    command -v "$name" >/dev/null || cnet_fail "missing_command name=$name"
  done
}

cnet_prepare_runtime() {
  umask 077
  WORK="$(mktemp -d /tmp/cnet-runtime.XXXXXXXX)"
  BIN="$ROOT/bin"
  PACKS="$WORK/packs"
  if [[ -f $ROOT/packaging/CNET-Minimal/coverage_gold.tsv ]]; then
    BIN="$(realpath -m -- "${BIN_DIR:-$ROOT/bin}")"
    cnet_require_bins
    FIXTURES="$ROOT/packaging/CNET-Minimal/coverage_gold.tsv"
    BIN_DIR="$BIN" CNET_HARVEST_LOG="$WORK/harvest.log" bash "$ROOT/scripts/cert_coverage_harvest.sh" "$PACKS" >"$WORK/harvest.out" 2>&1 || cnet_fail "harvest_failed log=$WORK/harvest.out"
  else
    cnet_require_bins
    FIXTURES="$ROOT/config/coverage_gold.tsv"
    [[ -f $ROOT/data/roe_daily_packs/INDEX.json && -f $ROOT/data/roe_daily_packs/ROUTES.jsonl ]] || cnet_fail 'missing_packs'
    [[ -z $(find "$ROOT/data/roe_daily_packs" -type l -print -quit) ]] || cnet_fail 'symlink_packs'
    mkdir "$PACKS"
    cp -a -- "$ROOT/data/roe_daily_packs/." "$PACKS/"
  fi
  [[ -f $FIXTURES && -f $ROOT/config/domain_routes.tsv && -f $ROOT/config/promote_blocklist.txt ]] || cnet_fail 'missing_config'
  cnet_offline
  cd "$WORK"
  mkdir logs
}

cnet_marker() {
  local marker="$1" out
  shift
  out=$(timeout 30 "$@" 2>&1) || { printf '%s\n' "$out"; cnet_fail "command_failed marker=$marker"; }
  printf '%s\n' "$out"
  grep -q "^$marker" <<<"$out" || cnet_fail "missing_marker marker=$marker"
}

cnet_check_queries() {
  local q answer out route count=0
  while IFS=$'\t' read -r q answer; do
    [[ -n $q && $q != \#* ]] || continue
    out=$(timeout 30 "$BIN/roe_front_door" ask "$q" --root "$PACKS" 2>&1) || cnet_fail "query_failed query=$q"
    grep -q '^source=LOCAL .*verified=1 ' <<<"$out" || { printf '%s\n' "$out"; cnet_fail "expected_LOCAL query=$q"; }
    [[ -z $answer ]] || grep -Fxq "A: $answer" <<<"$out" || cnet_fail "wrong_gold_answer query=$q"
    route=$(timeout 30 "$BIN/roe_domain_route" --file "$ROOT/config/domain_routes.tsv" "$q") || cnet_fail "route_failed query=$q"
    grep -qx 'DISPATCH CERT' <<<"$route" || cnet_fail "expected_CERT query=$q"
    count=$((count + 1))
  done < <(awk '!/^#/ && NF' "$FIXTURES"; printf '%s\n' 'CNET proposes Unity disposes' 'who are you' 'format-truncation werror')
  [[ $count -eq 9 ]] || cnet_fail "query_count count=$count"
  for q in 'brand new teacher only query zz99' 'zz unknown mystic ooze 99' 'totally unknown zzqq mystic ooze' 'autonomous cycle probe novel fact beta-nine'; do
    out=$(timeout 30 "$BIN/roe_front_door" ask "$q" --root "$PACKS" 2>&1) || cnet_fail "ood_query_failed query=$q"
    if ! grep -q '^source=ASK_USER .*verified=0 miss=1 ' <<<"$out" || ! grep -q '^A: ABSTAIN:' <<<"$out"; then
      printf '%s\n' "$out"; cnet_fail "expected_ABSTAIN query=$q"
    fi
  done
  echo 'NATIVE_COVERAGE_OK local=9 exact_gold=6 domain_cert=9 ood_abstain=4 fixture_only=1'
}
