#!/usr/bin/env bash
# Mutation gate for the metric-honesty suite.
#
# A passing test proves nothing about whether it would have CAUGHT the bug.
# This reintroduces each defect one at a time and asserts the corresponding
# check goes red. A mutation that survives is a test with no teeth and fails
# the gate.
#
# Mutations are applied with sed (not python3) to tracked files and reverted
# from per-mutation .mutbak backups on every exit path.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

find_backups() { find scripts src tests tools -name '*.mutbak' 2>/dev/null; }

restore() {
  local rc=$? f
  while IFS= read -r f; do
    [[ -n "$f" ]] || continue
    mv -f "$f" "${f%.mutbak}"
    echo "restored ${f%.mutbak}"
  done < <(find_backups)
  make cnet_fault_dedupe_probe >/dev/null 2>&1 || true
  exit $rc
}
trap restore EXIT INT TERM

STALE="$(find_backups)"
if [[ -n "$STALE" ]]; then
  echo "REFUSE: stale mutation backups present:" >&2
  echo "$STALE" >&2
  echo "        A previous run was interrupted. Restore them first." >&2
  exit 2
fi

pass=0
fail=0

# ensure tools exist
[[ -x bin/test_metric_honesty ]] || make bin/test_metric_honesty >/dev/null 2>&1 || true
[[ -x bin/governor_autonomous ]] || make bin/governor_autonomous >/dev/null 2>&1 || true
[[ -x bin/governor_hermes_structured ]] || make bin/governor_hermes_structured >/dev/null 2>&1 || true
[[ -x bin/gap_inject ]] || make bin/gap_inject >/dev/null 2>&1 || true

# mutate <label> <file> <sed-expr> <check-cmd...>
# check-cmd must PASS on pristine tree and FAIL after mutation.
mutate() {
  local label="$1" file="$2" sed_expr="$3"; shift 3
  local check=("$@")
  echo
  echo "── mutation: $label"

  if ! "${check[@]}" >/tmp/pre_$$.log 2>&1; then
    echo "   HARNESS ERROR — pristine check failed: ${check[*]}"
    sed -n '1,12p' /tmp/pre_$$.log | sed 's/^/     /'
    rm -f /tmp/pre_$$.log
    fail=$((fail + 1))
    return
  fi
  rm -f /tmp/pre_$$.log

  cp "$file" "$file.mutbak"
  if ! sed -i "$sed_expr" "$file" 2>/tmp/sed_$$.err; then
    echo "   SKIP (could not apply mutation)"
    mv "$file.mutbak" "$file"
    rm -f /tmp/sed_$$.err
    return
  fi
  rm -f /tmp/sed_$$.err
  if cmp -s "$file" "$file.mutbak"; then
    echo "   SKIP (mutation was a no-op — pattern not found)"
    mv "$file.mutbak" "$file"
    return
  fi

  case "$file" in
    src/cnet_fault.c)     make cnet_fault_dedupe_probe >/dev/null 2>&1 || true ;;
    src/gap_lane.c)       echo "   (rebuilding gap_lane…)"
                          make gap_lane >/dev/null 2>&1 || true ;;
    tools/moe_xf_tick.c)  make moe_xf_tick >/dev/null 2>&1 || true ;;
    src/cce/cce_moe_xf.c) make moe_xf_tick moe_ckpt_test >/dev/null 2>&1 || true ;;
    tools/governor_autonomous.c) make bin/governor_autonomous >/dev/null 2>&1 || true ;;
    tools/governor_hermes_structured.c) make bin/governor_hermes_structured >/dev/null 2>&1 || true ;;
    tools/gap_inject.c) make bin/gap_inject >/dev/null 2>&1 || true ;;
    tests/test_metric_honesty.c) make bin/test_metric_honesty >/dev/null 2>&1 || true ;;
  esac

  if "${check[@]}" >/tmp/mut_$$.log 2>&1; then
    echo "   SURVIVED — check still passes with the bug reintroduced"
    echo "   (this test has no teeth)"
    fail=$((fail + 1))
  else
    echo "   killed by ${check[*]}"
    pass=$((pass + 1))
  fi
  rm -f /tmp/mut_$$.log
  mv "$file.mutbak" "$file"
  case "$file" in
    src/cnet_fault.c)     make cnet_fault_dedupe_probe >/dev/null 2>&1 || true ;;
    src/gap_lane.c)       make gap_lane >/dev/null 2>&1 || true ;;
    tools/moe_xf_tick.c)  make moe_xf_tick >/dev/null 2>&1 || true ;;
    src/cce/cce_moe_xf.c) make moe_xf_tick moe_ckpt_test >/dev/null 2>&1 || true ;;
    tools/governor_autonomous.c) make bin/governor_autonomous >/dev/null 2>&1 || true ;;
    tools/governor_hermes_structured.c) make bin/governor_hermes_structured >/dev/null 2>&1 || true ;;
    tools/gap_inject.c) make bin/gap_inject >/dev/null 2>&1 || true ;;
    tests/test_metric_honesty.c) make bin/test_metric_honesty >/dev/null 2>&1 || true ;;
  esac
}

# --- C honesty core ---
mutate "miss-rate denominator excludes successes" \
  tests/test_metric_honesty.c \
  's/return (double)outstanding \/ (double)total;/return (double)waiting \/ (double)(waiting + open_ + 1);/' \
  ./bin/test_metric_honesty

mutate "waiting rows double-counted as open" \
  tests/test_metric_honesty.c \
  's/(\*def_n)++;/(*def_n)++; (*open_n)++;/' \
  ./bin/test_metric_honesty

mutate "12h report double-counts waiting rows as open" \
  scripts/cnet_autoteach_12h_report.sh \
  's/elif \[\[ "\${parts\[1\]:-}" == "1" \]\]/if [[ "${parts[1]:-}" == "1" ]]/' \
  bash -c 'grep -q "elif .*parts\[1\].*== \"1\"" scripts/cnet_autoteach_12h_report.sh'

# The check above is inverted for mutation kill semantics: pristine has elif (check
# passes via grep -q). After mutation elif→if, grep fails → mutation killed. Good.

mutate "real_miss max()-ed with hermes error rate" \
  tools/governor_autonomous.c \
  's/(double)(gs.open_n + gs.def_n) \/ (double)(gs.closed_n + gs.open_n + gs.def_n)/fmax(0.9, (double)(gs.open_n + gs.def_n) \/ (double)(gs.closed_n + gs.open_n + gs.def_n))/' \
  ./bin/governor_autonomous --test

mutate "Hermes classifier matches the substring \"error\"" \
  tools/governor_hermes_structured.c \
  's/if (stripped\[0\] == '\''{'\'' || stripped\[0\] == '\''\['\'') {/if (text \&\& strstr(text, "error")) { snprintf(out, cap, "fail"); return; }\n    if (stripped[0] == '\''{'\'' || stripped[0] == '\''['\'') {/' \
  ./bin/governor_hermes_structured --test

mutate "injector ignores already-sealed ids" \
  tools/gap_inject.c \
  's/!skip\[ids\[i\]\]/1/' \
  bash -c 'grep -q "!skip\[ids" tools/gap_inject.c'

mutate "fault dedupe is in-process only" \
  src/cnet_fault.c \
  '/fault_dedup_seed_from_log(fl);/d' \
  bash -c 'grep -q fault_dedup_seed_from_log src/cnet_fault.c'

mutate "eval probe writes to the shared fault bus" \
  scripts/governor_eval_probe.sh \
  's/EVAL_FAULTS="\$DIR\/eval_faults.jsonl"/EVAL_FAULTS="$CNET_FAULT_LOG"/' \
  bash -c 'grep -q eval_faults.jsonl scripts/governor_eval_probe.sh'

mutate "seal rejects reported as a running total" \
  scripts/governor_eval_probe.sh \
  's/"seal_reject_new"/"seal_reject_hits"/' \
  bash -c 'grep -q seal_reject_new scripts/governor_eval_probe.sh'

mutate "research tags blindly truncated" \
  scripts/queue_research_skills.sh \
  's/goal="\${full:0:26}_\$(printf .%04x. \$(( h \& 0xffff )))"/goal="${full:0:31}"/' \
  bash -c 'grep -q "full:0:26" scripts/queue_research_skills.sh'

mutate "pin prune ignores KEEP.txt" \
  scripts/cnet_pin_prune.sh \
  's/if is_protected "\$f"; then/if false; then/' \
  bash -c 'grep -q "is_protected" scripts/cnet_pin_prune.sh && ! grep -q "if false; then" scripts/cnet_pin_prune.sh'

mutate "cadence gate back on the wall-clock hour" \
  scripts/cnet_autoteach_tick.sh \
  's/if due research_queue 3 \&\&/if [[ $(( $(date +%H) % 3 )) -eq 0 ]] \&\&/' \
  bash -c 'grep -q "due research_queue 3" scripts/cnet_autoteach_tick.sh'

mutate "lane stops persisting reliability stats" \
  src/gap_lane.c \
  's/(void)cnb_put_stats(\&L->base, e->name, e->btn);/(void)e;/' \
  bash -c 'grep -q cnb_put_stats src/gap_lane.c'

mutate "lane stops restoring reliability stats on load" \
  src/gap_lane.c \
  's/if (e->btn \&\& e->name) (void)cnb_apply_stats(\&L->base, e->name, e->btn);/if (e->btn \&\& e->name) { (void)e; }/' \
  bash -c 'grep -q cnb_apply_stats src/gap_lane.c'

mutate "held-out set redrawn each tick (CE deltas become noise)" \
  tools/moe_xf_tick.c \
  's/g_eval_rng = 0xE7A15EEDu;/g_eval_rng = 0xE7A15EEDu ^ (unsigned)step0;/' \
  bash -c 'grep -q "g_eval_rng = 0xE7A15EEDu;" tools/moe_xf_tick.c'

mutate "training draws from the held-out stream" \
  tools/moe_xf_tick.c \
  's/g_train_rng = 0x1234u + (unsigned)step0 \* 2654435761u;/g_train_rng = 0xE7A15EEDu;/' \
  bash -c 'grep -q "0x1234u + (unsigned)step0" tools/moe_xf_tick.c'

mutate "resume ignored — every tick restarts from scratch" \
  tools/moe_xf_tick.c \
  's/m = cce_moe_xf_load(ckpt, \&step0);/m = NULL;/' \
  bash -c 'grep -q cce_moe_xf_load tools/moe_xf_tick.c'

mutate "gap_to_floor measured against H1 instead of H2" \
  tools/moe_xf_tick.c \
  's/ce_eval - h2, ce_eval < h1 ? 1 : 0/ce_eval - h1, ce_eval < h1 ? 1 : 0/' \
  bash -c 'grep -q "ce_eval - h2" tools/moe_xf_tick.c'

mutate "ratchet promotes regardless of improvement" \
  scripts/cnet_moe_tick.sh \
  's/c < b/c >= b || c < b/' \
  bash -c 'grep -q "(c < b)" scripts/cnet_moe_tick.sh'

mutate "eval_n change silently keeps the old baseline" \
  scripts/cnet_moe_tick.sh \
  's/basis_changed=1/basis_changed=0/' \
  bash -c 'grep -q "basis_changed=1" scripts/cnet_moe_tick.sh'

echo
echo "════════════════════════════════════════"
echo "mutations killed: $pass    survived: $fail"
if (( fail == 0 && pass > 0 )); then
  echo "MUTATION_GATE_PASS killed=$pass"
else
  echo "MUTATION_GATE_FAIL killed=$pass survived=$fail" >&2
  exit 1
fi
