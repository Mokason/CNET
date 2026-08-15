# WEIGHT CONVERSION DOOR — TABLE CERTIFIES

Status: **SLICE 1 — ATTEST, TABLE, NATIVE STUDENT, UNBIND**

Weights nominate / label; the table certifies; the teacher leaves.

This is not tensor remap. This is not WordLM growth. This is not F11.
This is not open chat. This is not beat-8B. Transcoder / ACDC training
is **not** in this slice. Broader claims stay **WITHHELD**.

## Path (this slice)

1. Attest a local open weight file (SHA-256) via `cnet_weight_attest`
   → `cnet_oracle_identity_is_attested`.
2. Bind as an oracle with a lease (`acquire_oracle_bind`). Policy:
   `OraclePolicy.require_attested_to_teach` and `require_lease_to_teach`.
   Unattested cannot teach.
3. mmap the GGUF (`cnet_weight_mmap`). Do not materialize a full FP16
   workspace. The labeler reads one F32 LUT in the map.
4. On **one** typed enumerable domain, a linear-probe-or-forward labeler
   (`cnet_weight_labeler` as `PlanTeacherFn`) proposes rows.
5. `plan_table_build` with that labeler. Aborts stay aborts. Teacher
   sweep is not deployment (counters snapshotted inside plan_table).
6. Consolidate into a native-C BTN student (`btn_train_dynamic_spec`,
   same train=verify rule as consolidate). Not WordLM. Not a transformer
   clone. Not compete intent.
7. Full-domain spec-reproduction ≥ 0.95. Then unbind the teacher.
   Serve the student only (`cnet_weight_serve` never calls the labeler).

## First domain

- Name: `u8_inc16`
- Meaning: increment on 0..15 (wrap 15→0)
- Combo count: **16**
- Ports: `PORT_BINARY_MSB` width 4 / 4. No RAW fields.
- Generated in the harness. Does **not** read ASI-5 448,
  excluded_prompts.tsv, F11 journals, CHAT-1 row bodies, or compete TSVs.

## Fixture vs real GGUF

This slice uses an attested **fixture** weight file: a tiny synthetic
GGUF with one F32 tensor `u8_inc16.lut`. Metadata records
`cnet.conversion = fixture_not_bonsai`.

No host GGUF (Bonsai / gemma / …) was present on the box. This door
did **not** download a multi-GB model. The fixture is **not** Bonsai
conversion. A later slice may attest a real local GGUF and use it as
the oracle labeler on this same tiny domain only.

## What this is not

- Not a naive `W_q` / `W_k` / `W_v` / MLP tensor remap.
  `cnet_weight_tensor_remap_admit` always refuses.
- Not WordLM growth. WordLM is not linked and not trained.
- Not F11. The 448 held-out is not read. Floors unchanged.
- Not open chat. Not beat-8B. Not a general mind.
- Not skip-transcoder / ACDC / SAE training. Those stay later slices.
- Not auto-CERT. Compression / reconstruction / probe accuracy is not
  an admit signal. `CnetWeightConvertReport.certified` stays 0.
  A student may be CERT only through the existing certify doors after
  the table passes.
- Not an API extraction attack. Local file the operator has, only.
- Soft router over CERT is forbidden. Residual / teacher never speaks
  and never admits.

## Gate

```
make cnet_weight_convert   # CNET_WEIGHT_CONVERT_PASS
```

Native C11 `-Wall -Wextra -pedantic -Werror`. Seconds, not hours.
