#!/usr/bin/env bash
# Behavioral authority test for the single CNET release gate.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
failures=0
fail() {
    failures=$((failures + 1))
    printf 'RELEASE_INTEGRITY_AUTHORITY_FAIL: %s\n' "$1" >&2
}

policy=$(cat docs/RELEASE_POLICY.md)
version=$(tr -d '[:space:]' <VERSION)
header=$(cat include/cnet_version.h)
runner_path=tests/run_release_integrity.sh
runner=
[[ -f "$runner_path" ]] && runner=$(cat "$runner_path")

body=$(awk '
  /^release_integrity:/ {grab=1; next}
  grab && /^[^[:space:]#]/ && !/^$/ {exit}
  grab {print}
' Makefile)
[[ -n "$body" ]] || fail "Makefile has no executable release_integrity recipe"
printf '%s' "$body" | grep -Fq 'bash tests/run_release_integrity.sh' ||
    fail "release_integrity does not delegate to the fail-closed runner"
[[ -n "$runner" ]] || fail "tests/run_release_integrity.sh is missing"
grep -Eq '^mcp_protocol_survival:[[:space:]]*' Makefile ||
    fail "Makefile references but does not define mcp_protocol_survival"

required_in_order=(
    release_integrity_authority
    license_metadata_test
    real_model_control_plane_test
    phase123_benchmark_test
    phase5_integration_test
    gguf_integrity
    model_runtime_integrity
    specialist_authority
    persistence_integrity
    mcp_protocol_survival
    release_package
    'PORTABLE=1 ci_core'
    'SKIP_RELEASE_PACKAGE=1 priority_acceptance'
    'git diff --check'
    'git diff --cached --check'
)
last=-1
for required in "${required_in_order[@]}"; do
    # bash string index of substring
    rest=${runner#*"$required"}
    if [[ "$rest" == "$runner" ]]; then
        fail "release runner omits $required"
        continue
    fi
    pos=$(( ${#runner} - ${#rest} - ${#required} ))
    if [[ $pos -le $last ]]; then
        fail "release runner runs $required out of order"
    else
        last=$pos
    fi
done

# Clean-tree status checks: exactly two, bracketing the run.
needle='git status --porcelain --untracked-files=no'
status_positions=()
tmp=$runner
offset=0
while [[ "$tmp" == *"$needle"* ]]; do
    prefix=${tmp%%"$needle"*}
    pos=$((offset + ${#prefix}))
    status_positions+=("$pos")
    tmp=${tmp#"$prefix$needle"}
    offset=$((pos + ${#needle}))
done
[[ ${#status_positions[@]} -eq 2 ]] ||
    fail "release runner must check a clean tracked tree at start and end"

auth_pos=${runner%%release_integrity_authority*}
auth_pos=${#auth_pos}
prio_pos=${runner%%priority_acceptance*}
prio_pos=${#prio_pos}
if [[ ${#status_positions[@]} -eq 2 ]]; then
    if [[ "${status_positions[0]}" -gt "$auth_pos" ]]; then
        fail "release runner clean-tree preflight runs too late"
    fi
    if [[ "${status_positions[1]}" -lt "$prio_pos" ]]; then
        fail "release runner clean-tree closure runs too early"
    fi
fi

printf '%s' "$runner" | grep -Fq 'logs/verified-today.release.md' ||
    fail "release runner does not preserve and clean generated evidence"
printf '%s' "$runner" | grep -Fq 'git restore -- docs/verified-today.generated.md' ||
    fail "release runner does not preserve and clean generated evidence"
printf '%s' "$runner" | grep -Fq 'set -Eeuo pipefail' ||
    fail "release runner is not strict Bash"
printf '%s' "$runner" | grep -Fq 'CNET_RELEASE_INTEGRITY_PASS' ||
    fail "release runner emits no terminal PASS marker"
if printf '%s' "$runner" | grep -Fq '|| echo'; then
    fail "release runner swallows failure with || echo"
fi
printf '%s' "$policy" | grep -Fq 'make release_integrity' ||
    fail "release policy does not name make release_integrity"
printf '%s' "$header" | grep -Fq "#define CNET_VERSION_STRING \"$version\"" ||
    fail "VERSION and cnet_version.h disagree"

# Behavioral failure injection against a disposable repo + fake make.
if [[ -n "$runner" ]]; then
    TD=$(mktemp -d)
    trap 'rm -rf "$TD"' EXIT
    mkdir -p "$TD/tests" "$TD/logs"
    cp "$runner_path" "$TD/tests/run_release_integrity.sh"
    chmod 755 "$TD/tests/run_release_integrity.sh"
    cat >"$TD/fake-make" <<'EOF'
#!/usr/bin/env bash
set -u
printf '%s\n' "$*" >> "$CNET_RELEASE_CALL_LOG"
fail=${CNET_RELEASE_FAIL_TARGET:-}
if [[ -n "$fail" && " $* " == *" $fail "* ]]; then
  printf '%s\n' INJECTED_RELEASE_FAILURE >&2
  exit 17
fi
EOF
    chmod 755 "$TD/fake-make"
    (
        cd "$TD"
        git init -q
        git add tests/run_release_integrity.sh
        git -c user.name='CNET Gate Test' -c user.email='gate-test@invalid' \
            commit -qm fixture
    )
    call_log="$TD/calls.log"
    export CNET_RELEASE_MAKE="$TD/fake-make"
    export CNET_RELEASE_CALL_LOG="$call_log"
    export CNET_RELEASE_FAIL_TARGET=phase123_benchmark_test
    printf 'STALE CNET_RELEASE_INTEGRITY_PASS\n' >"$TD/logs/release_integrity.log"

    set +e
    failed_err=$(
        cd "$TD"
        bash tests/run_release_integrity.sh 2>&1 >/dev/null
    )
    failed_rc=$?
    set -e

    calls=$(cat "$call_log" 2>/dev/null || true)
    [[ $failed_rc -eq 17 ]] ||
        fail "injected make failure returned $failed_rc, expected 17"
    printf '%s' "$calls" | grep -Fq 'phase123_benchmark_test' ||
        fail "failure injection never reached its target"
    if printf '%s' "$calls" | grep -Fq 'phase5_integration_test'; then
        fail "release runner continued after injected failure"
    fi
    failed_log="$TD/logs/release_integrity.log"
    [[ -f "$failed_log" ]] || fail "release runner did not atomically publish failure log"
    if [[ -f "$failed_log" ]]; then
        failure_text=$(cat "$failed_log")
        printf '%s' "$failure_text" | grep -Fq 'INJECTED_RELEASE_FAILURE' ||
            fail "release runner preserved stale log after injected failure"
        if printf '%s' "$failure_text" | grep -Fq 'CNET_RELEASE_INTEGRITY_PASS'; then
            fail "release runner wrote PASS after injected failure"
        fi
    fi
    printf '%s' "$failed_err" | grep -Fq 'INJECTED_RELEASE_FAILURE' ||
        fail "release runner did not replay failure log to original stderr"

    rm -f "$call_log"
    unset CNET_RELEASE_FAIL_TARGET
    set +e
    (
        cd "$TD"
        bash tests/run_release_integrity.sh >/dev/null 2>&1
    )
    passed_rc=$?
    set -e
    success_calls=$(cat "$call_log" 2>/dev/null || true)
    success_log=$(cat "$TD/logs/release_integrity.log" 2>/dev/null || true)
    [[ $passed_rc -eq 0 ]] || fail "injected success path returned $passed_rc"
    printf '%s' "$success_calls" | grep -Fq 'priority_acceptance' ||
        fail "injected success path did not reach final make target"
    printf '%s' "$success_log" | tail -n1 | grep -Fq 'CNET_RELEASE_INTEGRITY_PASS' ||
        fail "injected success path lacks terminal PASS marker"
fi

[[ $failures -eq 0 ]] || exit 1
printf 'RELEASE_INTEGRITY_AUTHORITY_PASS\n'
