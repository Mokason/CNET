#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/scripts" "$TMP/logs" "$TMP/docs"
cp "$ROOT/scripts/gen_claims.sh" "$TMP/scripts/gen_claims.sh"

CLAIMS='unified_adapter|logs/unified_adapter.log|UNIFIED_ADAPTER_PASS|unified
unified_cce_adapter|logs/unified_cce_adapter.log|UNIFIED_CCE_ADAPTER_PASS|unified
unified_oracle_adapter|logs/unified_oracle_adapter.log|ORACLE_CONTRACT_ADAPTER_PASS|unified
specialist_unit|logs/specialist_unit.log|SPECIALIST_UNIT_PASS|unified
unified_specialist_het_plan|logs/unified_specialist.log|HET_PLAN_PASS|unified
oracle_v2|logs/oracle_v2_test.log|ORACLE_V2_PASS|unified
async_runtime|logs/unified_async.log|ASYNC_RUNTIME_PASS|unified
async_gpu_lanes|logs/unified_gpu.log|ASYNC_GPU_LANES_PASS|gpu
qgkp_envelope|logs/qgkp_envelope_test.log|QGKP_ENVELOPE_PASS|unified
model_runtime|logs/unified_models_runtime.log|MODEL_RUNTIME_PASS|unified
model_catalog|logs/unified_models_catalog.log|MODEL_CATALOG_PASS|unified
ds4_dual_launcher|logs/unified_ds4_launcher.log|DS4_DUAL_LAUNCHER_PASS|unified
soul_host|logs/soul_host_test.log|SOUL_HOST_UNIFIED_PASS|unified
dotnet_host|logs/unified_host.log|CNET_HOST_UNIFIED_PASS|unified'

seed_logs() {
  while IFS='|' read -r _id log marker _scope; do
    mkdir -p "$TMP/$(dirname "$log")"
    case "$marker" in
      QGKP_ENVELOPE_PASS|MODEL_RUNTIME_PASS|MODEL_CATALOG_PASS)
        printf '%s checks=10\n' "$marker" > "$TMP/$log"
        ;;
      *)
        printf '%s\n' "$marker" > "$TMP/$log"
        ;;
    esac
  done <<< "$CLAIMS"
}

fail() {
  printf 'CLAIMS_TEST_FAIL: %s\n' "$1" >&2
  exit 1
}

seed_logs
(
  cd "$TMP"
  bash scripts/gen_claims.sh --strict >/dev/null
) || fail "strict mode rejected complete evidence"
grep -Fq '# Verified Evidence (generated)' \
  "$TMP/docs/verified-today.generated.md" ||
  fail "unscoped inventory was mislabeled as current verification"
grep -Fq 'unscoped evidence inventory' \
  "$TMP/docs/verified-today.generated.md" ||
  fail "unscoped inventory omitted the provenance caveat"
grep -Fq 'Log mtime (UTC)' "$TMP/docs/verified-today.generated.md" ||
  fail "generated table mislabeled filesystem mtime as execution time"
grep -Fq '"log_mtime_utc":' "$TMP/logs/claims.jsonl" ||
  fail "ledger omitted the explicit log mtime field"
grep -Fq '"scanner_host":' "$TMP/logs/claims.jsonl" ||
  fail "ledger mislabeled scanner identity as gate-host attestation"

rm -f "$TMP/logs/unified_adapter.log"
if (
  cd "$TMP"
  bash scripts/gen_claims.sh --strict >/dev/null
); then
  fail "strict mode accepted missing required evidence"
fi

seed_logs
: > "$TMP/logs/unified.started"
touch -d '2030-01-01 UTC' "$TMP/logs/unified.started"
if (
  cd "$TMP"
  bash scripts/gen_claims.sh --strict --scope unified \
    --since logs/unified.started >/dev/null
); then
  fail "run-scoped strict mode accepted stale required evidence"
fi

# In unified scope, the GPU-only lane is deliberately not part of the CPU gate.
# Give every unified log the sentinel timestamp while leaving the GPU log old.
# Equal timestamps are accepted for coarse-resolution filesystems.
touch -d '2020-01-01 UTC' "$TMP/logs/unified_gpu.log"
while IFS='|' read -r _id log _marker scope; do
  [ "$scope" = unified ] && touch -r "$TMP/logs/unified.started" "$TMP/$log"
done <<< "$CLAIMS"
(
  cd "$TMP"
  bash scripts/gen_claims.sh --strict --scope unified \
    --since logs/unified.started >/dev/null
) || fail "unified scope incorrectly required the GPU-only lane"
grep -Fq '"verdict":"OUT_OF_SCOPE"' "$TMP/logs/claims.jsonl" ||
  fail "out-of-scope claim was not explicit in JSONL"
grep -Fq '**13/13 in-scope claims verified; 1 out of scope.**' \
  "$TMP/docs/verified-today.generated.md" ||
  fail "generated summary did not report scoped denominator"
grep -Fq '# Verified Today (generated)' \
  "$TMP/docs/verified-today.generated.md" ||
  fail "fresh run-scoped evidence was not labeled as current verification"

printf 'CLAIMS_STRICT_PASS\n'
