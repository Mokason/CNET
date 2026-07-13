#!/bin/sh
# verify_logs.sh -- the log-scan gate for `make verify` / `make test`.
#
# Every test recipe runs the full chain even if one suite fails. This final
# positive-marker scan restores an honest nonzero exit for missing/crashed
# suites without false-positive greps on descriptive uses of "fail".

set -u
LOGS=${LOGS:-logs}
fail=0
ok=0

# Each row is "<logfile>|||<fixed success substring>".
CORE='
cce_safetensors_test.log|||ALL SAFETENSORS TESTS PASSED
cce_autograd_test.log|||failed -> OK
cce_model_test.log|||failed -> OK
cce_view.log|||failed -> OK
forest_view.log|||failed -> OK
cce_detect.log|||failed -> OK
cce_ssm.log|||failed -> OK
cce_st_llama.log|||failed -> OK
cce_specgraph.log|||failed -> OK
cce_wstore.log|||failed -> OK
cce_tiers.log|||failed -> OK
cce_similar.log|||failed -> OK
merge_family.log|||failed -> OK
hybrid_catalog.log|||failed -> OK
supra_train.log|||failed -> OK
contract_secure.log|||All contract security tests passed.
contract_unit.log|||All unit-file tests passed.
mutate.log|||All mutation-sweep gates passed.
acquire.log|||ALL ACQUIRE TESTS PASSED
base.log|||ALL BASE TESTS PASSED
flagship.log|||ALL FLAGSHIP TESTS PASSED
decimal_demo.log|||All decimal acts passed.
circuit_demo.log|||All circuit demo parts passed.
leakcheck.log|||(clean)
'

LONG='
supra_head_qat.log|||, 0 failed
supra_head_qat_corpus.log|||, 0 failed
supra_joint_qat.log|||, 0 failed
wordlm_holdout.log|||, 0 failed
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
    SPEC="$CORE$LONG$COMPAT"
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
    elif grep -qF -- "$marker" "$path"; then
        printf '  ok    %-26s\n' "$file"
        ok=$((ok + 1))
    else
        printf '  FAIL  %-26s  (success marker not found: "%s")\n' "$file" "$marker"
        fail=$((fail + 1))
    fi
done <<EOF
$SPEC
EOF

if [ "$fail" -ne 0 ]; then
    printf '\nVERIFY LOG GATE: %d suite(s) FAILED, %d ok -- see the logs above.\n' "$fail" "$ok"
    exit 1
fi
printf '\nVERIFY LOG GATE: all %d suites reported success.\n' "$ok"
exit 0