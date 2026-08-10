# Gap-lane window v2 unblock (2026-08-02)

## Root cause
- HTTP Bonsai teacher was healthy (`:8080`).
- `teachable=0` / `drain will stay at 0` was **not** a dead teacher.
- 256-id window `english_window_256_bonsai.txt` was fully closed in the ledger
  (`AUTOTEACH_INJECT reason=window_exhausted covered=745 remaining=0`).
- The 9 `retry_candidates` were legacy **16-wide** `waiting_oracle` gaps that
  `http_shape_ok` rejects at W=256 (`rejected_shape=9`).
- PEFT/cert path via `cnet_cert_learn_tick` was still progressing separately.

## Fix
1. Built `english_window_256_bonsai_v2.txt` — 256 fresh Bonsai token ids with
   zero overlap against closed ledger token goals and the v1 window.
2. Pointed lane drop-ins + autoteach/inject defaults at v2.
3. Parked 9 legacy w16 gaps: `waiting_oracle` → `unsupported_shape_w16`
   (ledger backup retained) so they stop masquerading as drainable demand.
4. Restart lane, inject fresh NO_PLAN seeds, verify `bound>0` / `closed>0`.

## Non-goals
- No cert floor changes.
- No resurrection of w16 teachers.
