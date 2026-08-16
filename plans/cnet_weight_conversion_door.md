# WEIGHT CONVERSION DOOR — TABLE CERTIFIES

Status: **SLICE 2 — HOST GGUF LABELER, STILL TABLE CERTIFIES**

Weights nominate / label; the table certifies; the teacher leaves.

This is not tensor remap. This is not WordLM growth. This is not F11.
This is not open chat. This is not beat-8B. Transcoder / ACDC training
is **not** in this slice. Broader claims stay **WITHHELD**.

## HOST GGUF LABELER — still TABLE CERTIFIES

The leased labeler is a GGUF, but it is **behind the door**, not a mouth.

- **Reader is `cce_gguf`.** Open with `cce_gguf_load`. Locate the named
  tensor with `cce_gguf_find_tensor`. Read it with `cce_gguf_tensor_bytes`
  (mmap path) or `cce_gguf_load_f32` (typed decode). The private
  fixture-only `parse_lut` (1 MiB cap, name must be `u8_inc16.lut`,
  always `fixture=1`) is gone. A real host GGUF is no longer refused
  by those toy caps; it is still refused as a *labeler* unless it
  carries a typed `u8_inc16.lut` (or equivalent) that the silent path
  can read.
- **Residual is not the labeler.** `residual_gguf_oracle` /
  `residual_gguf_label_batch` speak next-tokens. Convert does not
  call them. `teacher_calls` and `residual_speak` stay 0. The labeler
  is `cnet_weight_labeler` (`PlanTeacherFn`): typed u8 in → typed u8
  out, abort ≠ guess, by reading one named LUT tensor.
- **Fixture-or-host: fixture.** No host GGUF (Bonsai / gemma / …) was
  present on the box. Nothing multi-GB was downloaded. The payload is
  still the tiny `u8_inc16` increment table written as a GGUF that
  the *real* `cce_gguf_load` accepts. Reader is cce_gguf; payload is
  still u8_inc16, **not Bonsai**. A later slice may attest a real
  local host GGUF. That file can be attested; the increment table
  still has to come from a tensor/LUT the GGUF carries. Next-token
  residual will not certify this domain.
- **Not Bonsai conversion. Not open chat. Not F11.**

## Path (this slice)

1. Attest a local open weight file (SHA-256) via `cnet_weight_attest`
   → `cnet_oracle_identity_is_attested`.
2. Bind as an oracle with a lease (`acquire_oracle_bind`). Policy:
   `OraclePolicy.require_attested_to_teach` and `require_lease_to_teach`.
   Unattested cannot teach.
3. Open the GGUF with `cce_gguf_load` (`cnet_weight_mmap`). Do not
   materialize a full FP16 workspace. The labeler reads one named
   tensor (`u8_inc16.lut`) through `cce_gguf_load_f32`.
4. On **one** typed enumerable domain, a silent tensor labeler
   (`cnet_weight_labeler` as `PlanTeacherFn`) proposes rows.
5. `plan_table_build` with that labeler. Aborts stay aborts. Teacher
   sweep is not deployment (counters snapshotted inside plan_table).
6. Consolidate into a native-C BTN student (`btn_train_dynamic_spec`,
   same train=verify rule as consolidate). Not WordLM. Not a transformer
   clone. Not compete intent.
7. Full-domain spec-reproduction ≥ 0.95. Then unbind the teacher
   (GGUF handle dropped from the oracle). Serve the student only
   (`cnet_weight_serve` is `btn_forward` only; it never calls the
   labeler and never touches the GGUF).

## First domain

- Name: `u8_inc16`
- Meaning: increment on 0..15 (wrap 15→0)
- Combo count: **16**
- Ports: `PORT_BINARY_MSB` width 4 / 4. No RAW fields.
- Generated in the harness. Does **not** read ASI-5 448,
  excluded_prompts.tsv, F11 journals, CHAT-1 row bodies, or compete TSVs.

## What this is not

- Not a naive `W_q` / `W_k` / `W_v` / MLP tensor remap.
  `cnet_weight_tensor_remap_admit` always refuses. `gguf_partial_convert`
  is not this door.
- Not WordLM growth. WordLM is not linked and not trained.
- Not F11. The 448 held-out is not read. Floors unchanged.
- Not open chat. Not beat-8B. Not a general mind.
- Not skip-transcoder / ACDC / SAE training. Those stay later slices.
- Not auto-CERT. Compression / reconstruction / probe accuracy is not
  an admit signal. `CnetWeightConvertReport.certified` stays 0.
  A student may be CERT only through the existing certify doors after
  the table passes. `specialist_admit` is not called.
- Not an API extraction attack. Local file the operator has, only.
- Soft router over CERT is forbidden. Residual / teacher never speaks
  and never admits.

## Gate

```
make cnet_weight_convert   # CNET_WEIGHT_CONVERT_PASS / checks=44
make cnet_weight_gguf      # alias
```

Native C11 `-Wall -Wextra -pedantic -Werror`. Seconds, not hours.
No Python.
