#!/usr/bin/env bash
# Validate the local make ci contract (portable CI graph + ROCm lane).
# Hosted GitHub Actions CI is WITHHELD when .github/workflows/ci.yml is absent.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
fail() { printf 'CI_WORKFLOW_FAIL: %s\n' "$1" >&2; exit 1; }

[[ -f Makefile ]] || fail "Makefile missing"

ci_line=$(grep -E '^ci:' Makefile | head -n1 || true)
[[ -n "$ci_line" ]] || fail "make ci target missing"
# shellcheck disable=SC2086
ci_deps=${ci_line#ci:}
for gate in test dotnet_cce_tests cce_train_bench; do
    printf '%s' "$ci_deps" | grep -Eq "(^|[[:space:]])${gate}([[:space:]]|$)" ||
        fail "make ci missing full-suite gates: $gate"
done

rocm_line=$(grep -E '^ci_rocm:' Makefile | head -n1 || true)
[[ -n "$rocm_line" ]] || fail "make ci_rocm target missing (no GPU lane)"
rocm_deps=${rocm_line#ci_rocm:}
printf '%s' "$rocm_deps" | grep -Eq '(^|[[:space:]])ci([[:space:]]|$)' ||
    fail "ci_rocm must also run the portable ci gate"

# Recipe body until next blank line after the rule header.
rocm_recipe=$(awk '
  /^ci_rocm:/ {grab=1; next}
  grab && /^$/ {exit}
  grab {print}
' Makefile)
printf '%s' "$rocm_recipe" | grep -q 'hipgemm_res' ||
    fail "ci_rocm must run the hipgemm_res device gate"
printf '%s' "$rocm_recipe" | grep -q 'CNET_REQUIRE_ROCM=1' ||
    fail "ci_rocm must set CNET_REQUIRE_ROCM=1 so an absent device fails instead of silently skipping"
grep -Eq '^hipgemm_res:' Makefile || fail "hipgemm_res target missing"

workflow=".github/workflows/ci.yml"
if [[ -f "$workflow" ]]; then
    # Structural presence only; full YAML audit is optional without yq.
    printf 'CI_WORKFLOW_LOCAL_PASS status=local_gates_ok hosted_workflow=PRESENT\n'
else
    printf 'CI_WORKFLOW_LOCAL_PASS status=local_gates_ok hosted_workflow=WITHHELD\n'
    printf 'note: no executing hosted workflow. `make ci` / `make ci_rocm` are the '
    printf 'local authority; per-change enforcement elsewhere is NOT established\n'
fi
