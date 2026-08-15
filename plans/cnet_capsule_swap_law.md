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
| `library_evolve` / `finalize_chunk` | `contract_already_known` exact-dedup, then `specialist_admit` | `library_admit_candidate`: exact-same digest still skips; already-known contract + `n_comps==0` still skips; otherwise `cnet_swap_admit` |
| LIBRARY / `cnet.so` | `src/cnet_swap.c` not linked | `src/cnet_swap.c` is in `LIBRARY`, so `cnet_dll` / `cnet.so` contains the law |

Teacher / residual adapters still **REFUSE** (`btn_is_adapter`). Residual
never admits. `n_comps==0` still cannot **REPLACE**.

Still no F11 / 448 / CHAT-1 claim. No GitHub Actions. No 8B. No soft
router. WordLM untouched.

## Incumbent choice

- `registry_add_certified`: the same-name slot only. First add (no
  incumbent) still appends. CCE/oracle adapter first-add is unchanged.
- `library_admit_candidate` / `library_evolve`: **same name first**, else
  the first **certified** brick that `btn_certify`s on the incoming
  contract (coverage family).

Same-name / family coverage uses the incoming contract table as both
`old_cov` and `new_cov` (the slot's current certification table). CERT
compositions are borrowed via `cnet_swap_bind_compositions` (NULL/0 =
no composition proof).

## Decision (unchanged law)

```text
adapter / teacher-residual     -> REFUSE (never admits)
n_comps==0                     -> ADD-ALONGSIDE (no composition proof; never REPLACE)
dominate AND compositions hold -> REPLACE
otherwise                      -> ADD-ALONGSIDE
explicit replace when law fails -> REFUSE (old stays)
```

Dominate means: old coverage is a subset of new, **or** the new brick
replays every old coverage row (`btn_certify` on those rows). When both
a new table and a new brick+targets are given, both must hold. Empty
old coverage cannot dominate.

Compression / MAC / `compute_beneficial` are not read.

## Gate

```text
make cnet_swap
make library_swap
```

`make cnet_swap` must print `CNET_SWAP_PASS` and `checks=51`.
`make library_swap` must print `CNET_LIBRARY_SWAP_PASS` and the gated
`checks=26` count. No 8B compete. No GitHub Actions. No fake PASS.
