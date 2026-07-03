#!/bin/sh
# verify_logs.sh -- the log-scan gate for `make verify` / `make test`.
#
# WHY THIS EXISTS
#   Every test recipe in the verify chain runs its binary as
#       ./bin/name > logs/name.log 2>&1 || echo "test exited non-zero (see log)"
#   The trailing `|| echo` deliberately swallows the exit code so the WHOLE
#   chain keeps running even when one suite fails (run-everything behaviour).
#   The cost is that `make test` used to succeed even when a suite failed.
#   This script is the missing final gate: after every suite has run and
#   written its log, it re-reads each log and asserts that suite's terminal
#   SUCCESS marker is present. If any marker is missing (a failed suite prints
#   a different terminal line; a crashed/absent suite prints none), it exits
#   non-zero so `make test` fails.
#
# WHY POSITIVE MARKERS (not a failure-grep)
#   These logs legitimately contain the words "fail"/"failure"/"pass" inside
#   ordinary test descriptions (e.g. "ok  fails when only one source ...",
#   "ambiguous raw output recorded as a failure", "correct passes"). Grepping
#   for failure words would false-positive on passing runs. Instead each suite
#   emits a distinct terminal success line ONLY when it passed (verified in the
#   test sources: CCE suites print "N failed -> OK" only when fails==0; the big
#   suite prints "ALL TESTS PASSED (single exe)" only when total_failures==0;
#   etc.). Requiring that line is robust against both benign wording and crashes.
#
# USAGE
#   sh tests/verify_logs.sh          # scan the `verify` (= `make test`) chain
#   sh tests/verify_logs.sh long     # also scan the verify-long-only extras
#   LOGS=/path/to/logs sh tests/verify_logs.sh   # scan a different log dir
#
# Each row below is  "<logfile>|||<fixed success substring that MUST be present>"
# (matched with grep -F, so "->", "." and "()" are literal).

set -u
LOGS=${LOGS:-logs}
fail=0
ok=0

# --- verify (= make test) core chain -------------------------------------
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
supra_train.log|||failed -> OK
contract_secure.log|||All contract security tests passed.
contract_unit.log|||All unit-file tests passed.
mutate.log|||All mutation-sweep gates passed.
acquire.log|||ALL ACQUIRE TESTS PASSED
base.log|||ALL BASE TESTS PASSED
flagship.log|||ALL FLAGSHIP TESTS PASSED
decimal_demo.log|||All decimal acts passed.
circuit_demo.log|||All circuit demo parts passed.
legacy_test.log|||ALL TESTS PASSED (single exe)
leakcheck.log|||(clean)
'

# --- verify-long-only extras (the two supra QAT gates that also swallow) --
# (cce_train_bench and wordlm_bitnet already hard-fail: they run inline with
#  no `|| echo` swallow, so they need no log gate.)
LONG='
supra_head_qat.log|||, 0 failed
supra_head_qat_corpus.log|||, 0 failed
'

SPEC="$CORE"
if [ "${1:-}" = "long" ]; then
    SPEC="$CORE$LONG"
fi
# Strip any CRs so the parsed markers stay clean if this script (or the table)
# ever lands with CRLF line endings on Windows -- a trailing \r on a marker
# would otherwise never match. Log lines may still be CRLF; that is fine, the
# markers are mid-line substrings so grep -F matches regardless.
SPEC=$(printf '%s' "$SPEC" | tr -d '\r')

printf '\n=== verify log gate (%s) ===\n' "$LOGS"

# Feed the table via a heredoc (NOT a pipe) so the loop runs in THIS shell and
# the fail/ok counters survive.
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
