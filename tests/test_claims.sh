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
specialist_health|logs/specialist_health.log|SPECIALIST_HEALTH_PASS|unified
gap_lane|logs/gap_lane.log|GAP_LANE_PASS|unified
dispatch_story|logs/dispatch_story.log|DISPATCH_STORY_PASS|unified
unified_specialist_het_plan|logs/unified_specialist.log|HET_PLAN_PASS|unified
oracle_v2|logs/oracle_v2_test.log|ORACLE_V2_PASS|unified
async_runtime|logs/unified_async.log|ASYNC_RUNTIME_PASS|unified
async_gpu_lanes|logs/unified_gpu.log|ASYNC_GPU_LANES_PASS|gpu
qgkp_envelope|logs/qgkp_envelope_test.log|QGKP_ENVELOPE_PASS|unified
model_runtime|logs/unified_models_runtime.log|MODEL_RUNTIME_PASS|unified
model_catalog|logs/unified_models_catalog.log|MODEL_CATALOG_PASS|unified
ds4_dual_launcher|logs/unified_ds4_launcher.log|DS4_DUAL_LAUNCHER_PASS|unified
soul_host|logs/soul_host_test.log|SOUL_HOST_UNIFIED_PASS|unified
specialist_reopen|logs/soul_reopen_test.log|SPECIALIST_REOPEN_PASS|unified
admission_bypass|logs/admission_bypass_audit.log|ADMISSION_BYPASS_AUDIT_PASS|unified
build_hygiene|logs/build_hygiene_test.log|BUILD_HYGIENE_PASS|unified
alt_paths|logs/alt_paths_gate.log|ALT_PATHS_GATE_PASS|unified
managed_restore|logs/dotnet_restore.log|DOTNET_RESTORE_PASS|unified
dotnet_host|logs/unified_host.log|CNET_HOST_UNIFIED_PASS|unified'
CLAIMS="$CLAIMS
real_moe_e2e|logs/moe_e2e.log|REAL_MOE_E2E_PASS|model
real_proj_qat_gemma_e2e|logs/proj_qat_gemma_e2e.log|REAL_PROJ_QAT_GEMMA_E2E_PASS|model
distrust_loop|logs/distrust_loop.log|DISTRUST_LOOP_PASS|autonomy
autonomy_tick|logs/distrust_loop.log|AUTONOMY_TICK_PASS|autonomy
autonomy_spine|logs/autonomy_spine.log|AUTONOMY_SPINE_PASS|autonomy
seal_trust|logs/seal_trust.log|SEAL_TRUST_PASS|autonomy"

seed_logs() {
  while IFS='|' read -r _id log marker _scope; do
    [ -z "$_id" ] && continue
    mkdir -p "$TMP/$(dirname "$log")"
    case "$marker" in
      QGKP_ENVELOPE_PASS|MODEL_RUNTIME_PASS|MODEL_CATALOG_PASS)
        printf '%s checks=10\n' "$marker" >> "$TMP/$log"
        ;;
      DISTRUST_LOOP_PASS|AUTONOMY_TICK_PASS)
        # same gate log carries both markers
        if [ ! -f "$TMP/$log" ] || ! grep -q "$marker" "$TMP/$log" 2>/dev/null; then
          printf '%s\n' "$marker" >> "$TMP/$log"
        fi
        ;;
      *)
        # first writer wins unless empty
        if [ ! -s "$TMP/$log" ]; then
          printf '%s\n' "$marker" > "$TMP/$log"
        elif ! grep -q "$marker" "$TMP/$log" 2>/dev/null; then
          printf '%s\n' "$marker" >> "$TMP/$log"
        fi
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

# A skip marker can never satisfy real-model evidence, even when the process
# producing it exited successfully.
printf 'REAL_MOE_E2E_SKIPPED reason=no_checkpoint\n' > "$TMP/logs/moe_e2e.log"
if (
  cd "$TMP"
  bash scripts/gen_claims.sh --strict --scope model >/dev/null
); then
  fail "model scope accepted skipped real-model evidence"
fi
grep -Fq '"claim":"real_moe_e2e","scope":"model"' "$TMP/logs/claims.jsonl" &&
  grep -Fq '"verdict":"SKIPPED"' "$TMP/logs/claims.jsonl" ||
  fail "skipped model evidence was not classified explicitly"

# An explicit failure marker outranks an exact pass marker in the same log.
seed_logs
printf 'FAIL: terminal model failure\nREAL_MOE_E2E_PASS\n' > "$TMP/logs/moe_e2e.log"
if (
  cd "$TMP"
  bash scripts/gen_claims.sh --strict --scope model >/dev/null
); then
  fail "model scope accepted contradictory FAIL+PASS evidence"
fi
grep -Fq '"claim":"real_moe_e2e","scope":"model"' "$TMP/logs/claims.jsonl" &&
  grep -Fq '"verdict":"FAIL"' "$TMP/logs/claims.jsonl" ||
  fail "explicit failure did not outrank the pass marker"

# Missing and stale model evidence are fatal in strict model scope.
seed_logs
rm -f "$TMP/logs/moe_e2e.log"
if (cd "$TMP" && bash scripts/gen_claims.sh --strict --scope model >/dev/null); then
  fail "model scope accepted missing real-model evidence"
fi
seed_logs
: > "$TMP/logs/model.started"
touch -d '2030-01-01 UTC' "$TMP/logs/model.started"
if (cd "$TMP" && bash scripts/gen_claims.sh --strict --scope model --since logs/model.started >/dev/null); then
  fail "model scope accepted stale real-model evidence"
fi

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
printf 'REAL_MOE_E2E_SKIPPED reason=no_checkpoint\n' > "$TMP/logs/moe_e2e.log"
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
# Denominator: every unified-scope claim in scope; non-unified marked out of scope.
# Do not hardcode N/N — claim table grows (autonomy, etc.).
grep -E '\*\*[0-9]+/[0-9]+ in-scope claims verified; [0-9]+ out of scope\.\*\*' \
  "$TMP/docs/verified-today.generated.md" >/dev/null ||
  fail "generated summary did not report scoped denominator"
# unified in-scope count must equal number of unified rows in CLAIMS table
u_expect=$(printf '%s\n' "$CLAIMS" | awk -F'|' '$4=="unified"{c++} END{print c+0}')
u_got=$(grep -Eo '\*\*[0-9]+/[0-9]+ in-scope' "$TMP/docs/verified-today.generated.md" | head -1 | grep -Eo '[0-9]+/[0-9]+' | cut -d/ -f1)
[ "$u_got" = "$u_expect" ] || fail "unified in-scope count $u_got != table $u_expect"
grep -Fq '# Verified Today (generated)' \
  "$TMP/docs/verified-today.generated.md" ||
  fail "fresh run-scoped evidence was not labeled as current verification"
awk '/"claim":"real_moe_e2e"/ && /"verdict":"OUT_OF_SCOPE"/ { found=1 } END { exit !found }' \
  "$TMP/logs/claims.jsonl" ||
  fail "unified ledger did not visibly report model evidence out of scope"

printf 'CLAIMS_STRICT_PASS\n'
