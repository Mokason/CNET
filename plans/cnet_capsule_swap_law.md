# Capsule / chunk swap law

Status: **LIBRARY SWAP LAW — wired into registry_add_certified + library_evolve**
(honest on this branch / PR. Do not claim `origin/master` obeys it until
Ruufus says DONE.)

This slice is the **library wiring**. The unit-tested law in
`include/cnet_swap.h` + `src/cnet_swap.c` is unchanged. Live doors now
call `cnet_swap_admit`:

| Door | What it used to do | What it does now |
|---|---|---|
| `registry_add_certified` | same-name `contract_better_if` replace-or-reject | same-name incumbent → `cnet_swap_registry_hook` → `cnet_swap_admit` |
| `library_evolve` / `finalize_chunk` | `contract_already_known` exact-dedup, then `specialist_admit` | `library_admit_candidate`: exact-same digest still skips; already-known contract + `n_comps==0` still skips; same-name goes through `cnet_swap_admit` |
| LIBRARY / `cnet.so` | `src/cnet_swap.c` not linked | `src/cnet_swap.c` is in `LIBRARY`, so `cnet_dll` / `cnet.so` contains the law |

Teacher / residual adapters **REFUSE as a replacement** (`btn_is_adapter`
in the hook / admit). Residual first-add via `registry_add_certified`
is still possible (CCE/oracle). Honest line: never as a replacement,
not "never admits." `n_comps==0` still cannot **REPLACE**.

`library_evolve` never binds CERT compositions, so default evolve
cannot REPLACE. Evolve REPLACE only when real compositions are bound
(`cnet_swap_bind_compositions`); otherwise ADD-ALONGSIDE /
`no_compositions_to_prove`. Do not fake compositions to force REPLACE.

Still no F11 / 448 / CHAT-1 claim. No GitHub Actions. No 8B. No soft
router. WordLM untouched.

## Incumbent choice

- `registry_add_certified`: the same-name slot only. First add (no
  incumbent) still appends. CCE/oracle adapter first-add is unchanged.
- `library_admit_candidate` / `library_evolve`: **same name only** for
  REPLACE. Cross-name first-certify on the incoming contract is not a
  family proof: that path ADD-ALONGSIDE (or skips when `n_comps==0` and
  the contract is already known). It never REPLACE the wrong name.

## Coverage (old_cov is persisted, not incoming)

`old_cov` is the **persisted incumbent certification table**: a
deep-copied exemplar table stored on `RegistryEntry.cert_cov` when the
brick is first added or replaced. The hook and `library_admit_candidate`
read THAT table.

Incoming is `new_cov` only.

Passing the same table as both `old_cov` and `new_cov` is a **dominate
lie**: `coverage_subset(A,A)` is always 1, and replay of the incoming
rows is what `btn_certify` just proved. Live dominate would collapse
to "compositions hold."

If the incumbent has no persisted table, `old_cov` is empty: empty
coverage cannot dominate, so the door ADD-ALONGSIDE (`no_old_coverage`),
never REPLACE.

CERT compositions are borrowed via `cnet_swap_bind_compositions`
(NULL/0 = no composition proof).

## Decision (unchanged law)

```text
adapter / teacher-residual     -> REFUSE (never as a replacement)
n_comps==0                     -> ADD-ALONGSIDE (no composition proof; never REPLACE)
dominate AND compositions hold -> REPLACE
otherwise                      -> ADD-ALONGSIDE
explicit replace when law fails -> REFUSE (old stays)
```

Dominate means: old coverage is a subset of new, **or** the new brick
replays every old coverage row (`btn_certify` on those rows). When both
a new table and a new brick+targets are given, both must hold. Empty
old coverage cannot dominate. The new table must actually cover the
incumbent's **original** rows, not just equal the incoming table.

Compression / MAC / `compute_beneficial` are not read.

## Gate

```text
make cnet_swap
make library_swap
```

`make cnet_swap` must print `CNET_SWAP_PASS` and `checks=51`.
`make library_swap` must print `CNET_LIBRARY_SWAP_PASS` and the gated
check count in the Makefile. No 8B compete. No GitHub Actions. No fake
PASS.
