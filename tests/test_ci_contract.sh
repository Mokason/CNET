#!/usr/bin/env bash
# Enforce config/ci_contract.json against the Makefile (requires jq).
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
CONTRACT=config/ci_contract.json
MAKEFILE=Makefile
WORKFLOWS=.github/workflows

command -v jq >/dev/null 2>&1 || {
    printf 'FAIL: jq is required to read %s\n' "$CONTRACT" >&2
    exit 1
}

failures=0
fail_msg() {
    failures=$((failures + 1))
    printf 'FAIL: %s\n' "$1"
}

[[ -f "$CONTRACT" ]] || {
    printf 'FAIL: config/ci_contract.json is missing\n'
    exit 1
}

ci_line=$(grep -E '^ci_core:' "$MAKEFILE" | head -n1 || true)
[[ -n "$ci_line" ]] || {
    printf 'FAIL: make ci_core target missing\n'
    exit 1
}
# shellcheck disable=SC2206
prerequisites=(${ci_line#ci_core:})
prereq_has() {
    local want=$1 p
    for p in "${prerequisites[@]}"; do
        [[ "$p" == "$want" ]] && return 0
    done
    return 1
}

mapfile -t targets < <(grep -E '^[A-Za-z0-9_./-]+:' "$MAKEFILE" | sed 's/:.*//' | sort -u)
target_has() {
    local want=$1 t
    for t in "${targets[@]}"; do
        [[ "$t" == "$want" ]] && return 0
    done
    return 1
}

INFRASTRUCTURE='ci_config_gate warning_debt_strict release_warning_gate flagship_prefix_cache campaign_provenance_unit execution_tiers_doc_gate alt_paths_gate artifact_isa_gate runtime_artifact_hygiene ci_contract_gate'
infra_has() {
    printf '%s' " $INFRASTRUCTURE " | grep -Fq " $1 "
}

required_count=0
n_gates=$(jq '.gates | length' "$CONTRACT")
i=0
while [[ $i -lt $n_gates ]]; do
    name=$(jq -r ".gates[$i].target" "$CONTRACT")
    status=$(jq -r ".gates[$i].status" "$CONTRACT")
    proves=$(jq -r ".gates[$i].proves // empty" "$CONTRACT")
    reason=$(jq -r ".gates[$i].reason // empty" "$CONTRACT")

    target_has "$name" || fail_msg "contract names $name, which is not a Makefile target"
    [[ -n "$proves" ]] || fail_msg "$name: every gate must say what it proves"

    case "$status" in
        required)
            required_count=$((required_count + 1))
            prereq_has "$name" ||
                fail_msg "$name is required by the contract but is not a ci_core prerequisite"
            ;;
        blocked|withheld)
            [[ -n "$reason" ]] || fail_msg "$name: a $status gate must record why"
            if prereq_has "$name"; then
                fail_msg "$name is $status and must NOT be a ci_core prerequisite -- a gate that cannot run here cannot be part of a passing CI"
            fi
            ;;
        *)
            fail_msg "$name: unknown status '$status'"
            ;;
    esac
    i=$((i + 1))
done

undeclared=()
for p in "${prerequisites[@]}"; do
    [[ -z "$p" ]] && continue
    if jq -e --arg t "$p" '.gates[] | select(.target==$t and .status=="required")' \
            "$CONTRACT" >/dev/null 2>&1; then
        continue
    fi
    if infra_has "$p"; then
        continue
    fi
    undeclared+=("$p")
done
if [[ ${#undeclared[@]} -gt 0 ]]; then
    fail_msg "ci_core declares gates the contract does not mention: ${undeclared[*]}"
fi

hosted_status=$(jq -r '.hosted_workflow.status' "$CONTRACT")
case "$hosted_status" in
    withheld|blocked|required) ;;
    *) fail_msg "hosted_workflow status '$hosted_status' is not a recognised status" ;;
esac

workflow_present=0
if [[ -d "$WORKFLOWS" ]]; then
    shopt -s nullglob
    wf_entries=("$WORKFLOWS"/*)
    shopt -u nullglob
    [[ ${#wf_entries[@]} -gt 0 ]] && workflow_present=1
fi
if [[ $workflow_present -eq 0 ]]; then
    case "$hosted_status" in
        withheld|blocked) ;;
        *)
            fail_msg "no hosted workflow exists, so its status must be withheld or blocked, not '$hosted_status'"
            ;;
    esac
fi
[[ "$hosted_status" != "pass" ]] ||
    fail_msg "an absent hosted workflow is never a pass"

n_claims=$(jq '.withheld_claims // [] | length' "$CONTRACT")
ci=0
while [[ $ci -lt $n_claims ]]; do
    claim=$(jq -r ".withheld_claims[$ci].claim" "$CONTRACT")
    cstatus=$(jq -r ".withheld_claims[$ci].status" "$CONTRACT")
    creason=$(jq -r ".withheld_claims[$ci].reason // empty" "$CONTRACT")
    case "$cstatus" in
        withheld|blocked|failed) ;;
        *)
            fail_msg "claim '$claim' has status '$cstatus', which would read as established"
            ;;
    esac
    [[ -n "$creason" ]] || fail_msg "claim '$claim' must record why it is not established"
    ci=$((ci + 1))
done

if [[ $failures -gt 0 ]]; then
    printf 'CI_CONTRACT_FAIL failures=%d\n' "$failures"
    exit 1
fi

blocked=$(jq '[.gates[] | select(.status=="blocked")] | length' "$CONTRACT")
withheld_claims=$(jq '.withheld_claims // [] | length' "$CONTRACT")
printf 'CI_CONTRACT_PASS required=%d blocked=%d withheld_claims=%d hosted_workflow=%s\n' \
    "$required_count" "$blocked" "$withheld_claims" "$hosted_status"
