#!/usr/bin/env bash
# Mutation gate for the metric-honesty suite.
#
# A passing test proves nothing about whether it would have CAUGHT the bug.
# This reintroduces each 2026-07-25 defect one at a time and asserts the
# corresponding test goes red. A mutation that survives is a test with no teeth
# and fails the gate.
#
# Mutations are applied in place to tracked files and reverted with
# `git checkout --` on every exit path, including interrupts.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Restore ONLY from the per-mutation backup this script made.
#
# An earlier version restored with `git checkout -- <files>` on exit, which
# discards uncommitted work: it silently reverted an in-flight fix to
# governor_miss_ingest.sh that had not been committed yet. The .mutbak copy is
# the exact pre-mutation content, so it is both sufficient and safe.
# find, not a fixed glob list: src/*.mutbak would silently miss
# src/cce/cce_moe_xf.c.mutbak and leave that source mutated on disk.
find_backups() { find scripts src tests tools -name '*.mutbak' 2>/dev/null; }

restore() {
  local rc=$? f
  while IFS= read -r f; do
    [[ -n "$f" ]] || continue
    mv -f "$f" "${f%.mutbak}"
    echo "restored ${f%.mutbak}"
  done < <(find_backups)
  # Rebuild the probe from pristine source so a later run is not poisoned.
  make cnet_fault_dedupe_probe >/dev/null 2>&1 || true
  exit $rc
}
trap restore EXIT INT TERM

# Any leftover backup means a previous run died mid-mutation.
STALE="$(find_backups)"
if [[ -n "$STALE" ]]; then
  echo "REFUSE: stale mutation backups present:" >&2
  echo "$STALE" >&2
  echo "        A previous run was interrupted. Restore them first." >&2
  exit 2
fi

pass=0
fail=0

# mutate <label> <file> <python-rewrite> <test-selector...>
mutate() {
  local label="$1" file="$2" rewrite="$3"; shift 3
  local sel=("$@")
  echo
  echo "── mutation: $label"

  # The gate must not lie either. A selector that does not exist (a renamed
  # test, a typo) makes unittest exit non-zero, which would read as a kill.
  # Require the selector to exist AND pass on the pristine tree first.
  if ! python3 tests/test_metric_honesty.py "${sel[@]}" >/tmp/pre_$$.log 2>&1; then
    echo "   HARNESS ERROR — ${sel[*]} does not pass on the unmutated tree"
    sed -n '1,12p' /tmp/pre_$$.log | sed 's/^/     /'
    rm -f /tmp/pre_$$.log
    fail=$((fail + 1))
    return
  fi
  rm -f /tmp/pre_$$.log

  cp "$file" "$file.mutbak"
  if ! python3 - "$file" <<PY
import sys, pathlib
p = pathlib.Path(sys.argv[1])
s = p.read_text()
$rewrite
p.write_text(s)
PY
  then
    echo "   SKIP (could not apply mutation)"
    mv "$file.mutbak" "$file"
    return
  fi
  if cmp -s "$file" "$file.mutbak"; then
    echo "   SKIP (mutation was a no-op — pattern not found)"
    mv "$file.mutbak" "$file"
    return
  fi

  # C mutations only take effect once the dependent binary is rebuilt.
  case "$file" in
    src/cnet_fault.c)     make cnet_fault_dedupe_probe >/dev/null 2>&1 || true ;;
    src/gap_lane.c)       echo "   (rebuilding gap_lane…)"
                          make gap_lane >/dev/null 2>&1 || true ;;
    tools/moe_xf_tick.c)  make moe_xf_tick >/dev/null 2>&1 || true ;;
    src/cce/cce_moe_xf.c) make moe_xf_tick moe_ckpt_test >/dev/null 2>&1 || true ;;
  esac

  if python3 tests/test_metric_honesty.py "${sel[@]}" >/tmp/mut_$$.log 2>&1; then
    echo "   SURVIVED — ${sel[*]} still passes with the bug reintroduced"
    echo "   (this test has no teeth)"
    fail=$((fail + 1))
  else
    echo "   killed by ${sel[*]}"
    pass=$((pass + 1))
  fi
  rm -f /tmp/mut_$$.log
  mv "$file.mutbak" "$file"
  case "$file" in
    src/cnet_fault.c)     make cnet_fault_dedupe_probe >/dev/null 2>&1 || true ;;
    src/gap_lane.c)       make gap_lane >/dev/null 2>&1 || true ;;
    tools/moe_xf_tick.c)  make moe_xf_tick >/dev/null 2>&1 || true ;;
    src/cce/cce_moe_xf.c) make moe_xf_tick moe_ckpt_test >/dev/null 2>&1 || true ;;
  esac
}

mutate "miss-rate denominator excludes successes" \
  scripts/governor_miss_ingest.sh \
  's = s.replace("denom = max(1, outstanding + closed_n)", "denom = max(1, waiting + open_n + 1)")
s = s.replace("real_miss_rate = min(1.0, outstanding / denom)", "real_miss_rate = min(1.0, waiting / denom)")' \
  TestMissRateDenominator

mutate "waiting rows double-counted as open" \
  scripts/governor_miss_ingest.sh \
  's = s.replace("        elif state==\"1\":", "        if state==\"1\":")' \
  TestMissRateDenominator.test_waiting_rows_are_not_double_counted TestParsersAgree

mutate "real_miss max()-ed with the Hermes error rate" \
  scripts/governor_autonomous.py \
  's = s.replace("real_miss = float(miss.get(\"real_miss_rate\") or 0)", "real_miss = max(float(miss.get(\"real_miss_rate\") or 0), float(hermes.get(\"hermes_err_rate\") or 0))")' \
  TestMissRateProvenance

mutate "Hermes classifier matches the substring \"error\"" \
  scripts/governor_hermes_structured.py \
  'import re
s = re.sub(r"def classify\(text: str, tool_name: str \| None\) -> str:",
           "def classify(text: str, tool_name: str | None) -> str:\n    if tool_name and \"error\" in text.lower():\n        return \"fail\"", s, count=1)' \
  TestHermesClassifier

mutate "injector RNG seeded from a time bucket" \
  scripts/gap_inject.py \
  's = s.replace("    picks = random.sample(fresh, min(n, len(fresh)))",
                "    import time as _t\n    random.seed(int(_t.time()) // 3600)\n    picks = random.sample(fresh, min(n, len(fresh)))")' \
  TestGapInject.test_successive_calls_are_not_identical

mutate "injector ignores already-sealed ids" \
  scripts/gap_inject.py \
  's = s.replace("    fresh = [i for i in ids if i not in skip]", "    fresh = list(ids)")' \
  TestGapInject.test_never_proposes_a_sealed_token \
  TestGapInject.test_exhaustion_is_reported_not_faked

mutate "fault dedupe is in-process only" \
  src/cnet_fault.c \
  's = s.replace("    fault_dedup_seed_from_log(fl);\n", "")' \
  TestFaultBusIdempotence.test_dedupe_survives_a_new_process

mutate "eval probe writes to the shared fault bus" \
  scripts/governor_eval_probe.sh \
  's = s.replace("EVAL_FAULTS=\"$DIR/eval_faults.jsonl\"", "EVAL_FAULTS=\"$CNET_FAULT_LOG\"")' \
  TestEvalProbeHonesty.test_probe_does_not_grow_the_shared_fault_bus

mutate "seal rejects reported as a running total" \
  scripts/governor_eval_probe.sh \
  's = s.replace("\"seal_reject_new\": _i(\"$reject\"),", "\"seal_reject_hits\": _i(\"$reject\"),")' \
  TestEvalProbeHonesty.test_seal_rejects_are_a_delta_not_a_running_total

mutate "research tags blindly truncated" \
  scripts/queue_research_skills.sh \
  's = s.replace("    goal=\"${full:0:26}_$(printf \x27%04x\x27 $(( h & 0xffff )))\"", "    goal=\"${full:0:31}\"")' \
  TestTagCollisionResistance.test_long_names_sharing_a_prefix_stay_distinct

mutate "pin prune ignores KEEP.txt" \
  scripts/cnet_pin_prune.sh \
  's = s.replace("  if is_protected \"$f\"; then", "  if false; then")' \
  TestPinProtection.test_protected_pin_survives_forced_prune

mutate "cadence gate back on the wall-clock hour" \
  scripts/cnet_autoteach_tick.sh \
  's = s.replace("if due research_queue 3 &&", "if [[ $(( $(date +%H) % 3 )) -eq 0 ]] &&")' \
  TestCadenceGates.test_no_modulo_hour_gates

mutate "lane stops persisting reliability stats" \
  src/gap_lane.c \
  's = s.replace("        (void)cnb_put_stats(&L->base, e->name, e->btn);", "        (void)e;")' \
  TestReliabilityIsPersisted.test_lane_persists_and_restores

mutate "lane stops restoring reliability stats on load" \
  src/gap_lane.c \
  's = s.replace("if (e->btn && e->name) (void)cnb_apply_stats(&L->base, e->name, e->btn);", "if (e->btn && e->name) { (void)e; }")' \
  TestReliabilityIsPersisted.test_lane_persists_and_restores

mutate "units metric back to the byte-marker proxy" \
  scripts/governor_autonomous.py \
  'import re
s = re.sub(r"    audit = ROOT / \"bin/cnb_audit\".*?            pass\n", "", s, flags=re.DOTALL, count=1)' \
  TestUnitCountIsAuthoritative.test_no_byte_marker_proxy_on_the_primary_path

mutate "held-out set redrawn each tick (CE deltas become noise)" \
  tools/moe_xf_tick.c \
  's = s.replace("    g_eval_rng = 0xE7A15EEDu;", "    g_eval_rng = 0xE7A15EEDu ^ (unsigned)step0;")' \
  TestMoeFloorMetric.test_heldout_set_is_fixed_across_ticks

mutate "training draws from the held-out stream" \
  tools/moe_xf_tick.c \
  's = s.replace("    g_train_rng = 0x1234u + (unsigned)step0 * 2654435761u;", "    g_train_rng = 0xE7A15EEDu;")' \
  TestMoeFloorMetric.test_training_stream_does_not_replay_after_resume

mutate "resume ignored — every tick restarts from scratch" \
  tools/moe_xf_tick.c \
  's = s.replace("    m = cce_moe_xf_load(ckpt, &step0);", "    m = NULL;")' \
  TestMoeFloorMetric.test_resume_is_cumulative

mutate "gap_to_floor measured against H1 instead of H2" \
  tools/moe_xf_tick.c \
  's = s.replace("ce_eval - h2, ce_eval < h1 ? 1 : 0", "ce_eval - h1, ce_eval < h1 ? 1 : 0")' \
  TestMoeFloorMetric.test_gap_to_floor_is_ce_minus_h2

mutate "checkpoint drops the Adam moments" \
  src/cce/cce_moe_xf.c \
  's = s.replace("        rc |= xf_w(f, p->m, bytes, &h);   /* Adam first moment  */\n        rc |= xf_w(f, p->v, bytes, &h);   /* Adam second moment */\n", "        { float *z = (float *)calloc((size_t)p->n, sizeof(float)); rc |= xf_w(f, z, bytes, &h); rc |= xf_w(f, z, bytes, &h); free(z); }\n")' \
  TestMoeFloorMetric.test_checkpoint_contract_holds

mutate "ratchet promotes regardless of improvement" \
  scripts/cnet_moe_tick.sh \
  's = s.replace("    if best is None or ce < best:", "    if True:")' \
  TestMoeFloorMetric.test_promotion_requires_improvement

mutate "eval_n change silently keeps the old baseline" \
  scripts/cnet_moe_tick.sh \
  's = s.replace("basis_changed = prev_n is not None and int(prev_n) != eval_n", "basis_changed = False")' \
  TestMoeFloorMetric.test_changing_eval_n_resets_the_ratchet

echo
echo "════════════════════════════════════════"
echo "mutations killed: $pass    survived: $fail"
if (( fail == 0 && pass > 0 )); then
  echo "MUTATION_GATE_PASS killed=$pass"
else
  echo "MUTATION_GATE_FAIL killed=$pass survived=$fail" >&2
  exit 1
fi
