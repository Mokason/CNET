#!/bin/sh
# verify_logs.sh -- the log-scan gate for `make verify` / `make test`.
#
# Every test recipe runs the full chain even if one suite fails. This final
# positive-marker scan restores an honest nonzero exit for missing/crashed
# suites without false-positive greps on descriptive uses of "fail".

# FRESHNESS. Presence of a marker only ever proved that SOME process once wrote
# it. Nothing in the verify chain clears logs/ (the single `rm -rf logs` in the
# Makefile is inside `clean`), so a partial run, an interrupted run, or a run
# from last month left a full set of green logs behind and this gate reported
# "all suites reported success" over them -- while Makefile:1163 advertised that
# it "rejects missing or stale-success logs", which it did not.
#
# `make verify` now stamps a sentinel BEFORE its prerequisites run and passes it
# here as VERIFY_SINCE. Every log must be strictly newer than that stamp, so a
# carried-over log fails as loudly as a missing one.
#
# VERIFY_SINCE is intentionally OPTIONAL: this script is also run by hand. When
# it is unset the freshness column reads "unbound" rather than "ok", so a run
# without run-binding can never be mistaken for one with it.
set -u
LOGS=${LOGS:-logs}
SINCE=${VERIFY_SINCE:-}
fail=0
ok=0
stale=0

if [ -n "$SINCE" ] && [ ! -f "$SINCE" ]; then
    printf 'VERIFY LOG GATE: FAIL - sentinel %s does not exist; cannot prove freshness.\n' "$SINCE"
    exit 1
fi

# `find -newer` is the portable way to compare mtimes; MinGW's `test` has no
# -nt for files across filesystems and `stat` formats differ between GNU and
# BSD. Returns 0 when $1 is strictly newer than the sentinel.
newer_than_sentinel() {
    [ -z "$SINCE" ] && return 0
    [ -n "$(find "$1" -newer "$SINCE" 2>/dev/null)" ]
}

# Each row is "<logfile>|||<fixed success substring>".
# CORE = T1 only. Specialty CCE + PEFT live in T2 (see mk/verify_tiers.mk).
CORE='
authority.log|||CNET_AUTHORITY_PASS
cce_safetensors_test.log|||ALL SAFETENSORS TESTS PASSED
cce_autograd_test.log|||failed -> OK
cce_model_test.log|||failed -> OK
makefile_budget.log|||MAKEFILE_BUDGET_PASS
orphan_tools.log|||ORPHAN_TOOLS_PASS
layering_guard.log|||LAYERING_GUARD_PASS
clgemm_unit.log|||CLGEMM UNIT: PASS
cce_archive.log|||Archive Test: SUCCESS
cce_forest.log|||Forest + Router: SUCCESS
curl_guard.log|||CURL_GUARD_PASS
orphan_tests.log|||ORPHAN_TESTS_PASS
platform_sweep.log|||PLATFORM_SWEEP_PASS
residual_http_nocurl.log|||RESIDUAL_HTTP_NOCURL_PASS
cce_view.log|||failed -> OK
forest_view.log|||failed -> OK
cce_detect.log|||failed -> OK
contract_secure.log|||All contract security tests passed.
contract_unit.log|||All unit-file tests passed.
heal_mismatch.log|||HEAL_MISMATCH_PASS
mutate.log|||All mutation-sweep gates passed.
acquire.log|||ALL ACQUIRE TESTS PASSED
attribution.log|||ALL ATTRIBUTION TESTS PASSED
base.log|||ALL BASE TESTS PASSED
flagship.log|||ALL FLAGSHIP TESTS PASSED
decimal_demo.log|||All decimal acts passed.
circuit_demo.log|||All circuit demo parts passed.
leakcheck.log|||(clean)
cnet_vsa_gencap_bench.log|||CNET_VSA_GENCAP_BENCH_PASS
cnet_vsa_calibration_bench.log|||CNET_VSA_CALIBRATION_BENCH_PASS
cnet_vsa_lexicon_bench.log|||CNET_VSA_LEXICON_BENCH_PASS
cnet_vsa_answer_bench.log|||CNET_VSA_ANSWER_BENCH_PASS
cnet_vsa_q8_bench.log|||CNET_VSA_Q8_BENCH_PASS
cnet_vsa_stem_bench.log|||CNET_VSA_STEM_BENCH_PASS
cnet_vsa_delta_bench.log|||CNET_VSA_DELTA_BENCH_PASS
'

LONG='
supra_head_qat.log|||, 0 failed
supra_head_qat_corpus.log|||, 0 failed
transformer_qat_joint.log|||, 0 failed
wordlm_holdout.log|||, 0 failed
'

# T2 soak (verify-t2 / nightly / verify-long). Not scored on default verify.
T2='
cce_ssm.log|||failed -> OK
cce_st_llama.log|||failed -> OK
cce_specgraph.log|||failed -> OK
cce_wstore.log|||failed -> OK
cce_tiers.log|||failed -> OK
cce_similar.log|||failed -> OK
merge_family.log|||failed -> OK
hybrid_catalog.log|||failed -> OK
transformer_qat.log|||failed -> OK
qat_block.log|||ALL QAT BLOCK TESTS PASSED
mojo_bridge.log|||ALL MOJO BRIDGE TESTS PASSED
metric_honesty.log|||METRIC_HONESTY_PASS
moe_ckpt_test.log|||MOE_CKPT_PASS
distrust_loop.log|||DISTRUST_LOOP_PASS
distrust_loop.log|||AUTONOMY_TICK_PASS
autonomy_spine.log|||AUTONOMY_SPINE_PASS
cnet_vsa_router_bench.log|||CNET_VSA_ROUTER_BENCH_PASS
cnet_vsa_cli_bench.log|||CNET_VSA_CLI_BENCH_PASS
cnet_vsa_arena_bench.log|||CNET_VSA_ARENA_BENCH_PASS
cnet_vsa_encoder_sweep_bench.log|||CNET_VSA_ENCODER_SWEEP_BENCH_
vsa_routing_arena.log|||CNET_VSA_ROUTING_ARENA_PASS
'

# The compat tier (legacy quarantine): back-compat coverage that must stay
# green but no longer blocks every `make test`. Run via `make compat`.
COMPAT='
legacy_test.log|||ALL TESTS PASSED (single exe)
decimal_demo.log|||All decimal acts passed.
circuit_demo.log|||All circuit demo parts passed.
'

SPEC="$CORE"
if [ "${1:-}" = "long" ]; then
    # long = T1 CORE + T2 specialty + LONG supra + COMPAT
    SPEC="$CORE$T2$LONG$COMPAT"
elif [ "${1:-}" = "t2" ]; then
    SPEC="$T2"
elif [ "${1:-}" = "compat" ]; then
    SPEC="$COMPAT"
fi

printf '\n=== verify log gate (%s) ===\n' "$LOGS"

# A heredoc keeps the counters in this shell (a pipeline would fork the loop).
while IFS= read -r row; do
    [ -z "$row" ] && continue
    file=${row%%|||*}
    marker=${row#*|||}
    path="$LOGS/$file"
    if [ ! -f "$path" ]; then
        printf '  FAIL  %-26s  (log missing)\n' "$file"
        fail=$((fail + 1))
    elif ! grep -qF -- "$marker" "$path"; then
        printf '  FAIL  %-26s  (success marker not found: "%s")\n' "$file" "$marker"
        fail=$((fail + 1))
    elif ! newer_than_sentinel "$path"; then
        printf '  STALE %-26s  (marker present but log predates this run)\n' "$file"
        stale=$((stale + 1))
        fail=$((fail + 1))
    else
        printf '  ok    %-26s%s\n' "$file" \
            "$([ -n "$SINCE" ] || printf '  [freshness unbound]')"
        ok=$((ok + 1))
    fi
done <<EOF
$SPEC
EOF

if [ "$fail" -ne 0 ]; then
    printf '\nVERIFY LOG GATE: %d suite(s) FAILED (%d stale), %d ok -- see the logs above.\n' \
        "$fail" "$stale" "$ok"
    if [ "$stale" -ne 0 ]; then
        printf 'A STALE suite printed its success marker in an EARLIER run and was not\n'
        printf 're-run now. Treat it as unverified, not as passing.\n'
    fi
    exit 1
fi
if [ -n "$SINCE" ]; then
    printf '\nVERIFY LOG GATE: all %d suites reported success in THIS run (bound to %s).\n' \
        "$ok" "$SINCE"
else
    printf '\nVERIFY LOG GATE: all %d suites reported success, FRESHNESS UNBOUND\n' "$ok"
    printf '(no VERIFY_SINCE sentinel: these markers may have been written by an\n'
    printf 'earlier run. `make verify` binds them; a bare invocation does not.)\n'
fi
exit 0
