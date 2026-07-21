# Unit evidence bundles

**Status**: first slice — typed bundle + sidecar store + gap_lane / SoulHost wire  
**Gate**: `make evidence_bundle` → `EVIDENCE_BUNDLE_PASS`

## Why

A sealed CNU1 (weights + contract) is the claim. Audit, rollback, and
teacher-change detection need an explicit **evidence bundle** per learned unit.

## Contents

| Field | Source |
|---|---|
| `contract_digest` | `contract_content_digest` |
| `dataset_hash` | FNV over exemplar input\|\|output tables |
| `artifact_digest` | FNV of sealed CNU1 blob |
| `artifact_sha256` | SHA-256 of same blob |
| `behavior_digest` | `contract_btn_digest` / base unit ref |
| `toolchain_digest` | teacher/toolchain stamp (0 = unattested) |
| `runtime_libs_digest` | `cnet_runtime_libs_digest()` |
| reliability samples | successes/failures + Laplace |
| `counterfactual_stability` | optional [0,1]; `<0` unknown |
| `rollback_unit` + `rollback_artifact_digest` | prior version to restore |

## Persistence

JSONL sidecar: default `<base>.evidence.jsonl`, or `CNET_EVIDENCE_STORE`.
Replace-by-unit-name. Report-only — never grants admission.

## Wiring

- `gap_lane` provenance reconcile → `cnet_evidence_record` after seal link
- SoulHost structure mine seal → record when store path known
- `soul_unit_evidence(host, name, out, cap)` → JSON report for hosts/MCP

## API

```c
cnet_evidence_bundle_from_base / from_parts / verify / is_complete
cnet_evidence_store_put / get / load / path_for_base
cnet_evidence_record(base, unit, store, opts)
soul_unit_evidence(...)
```
