# Capsule / chunk swap law

Status: **UNIT PASS — SWAP LAW ONLY**

Native C. Residual is not the mouth. Compression is not an admit or
swap signal. Soft learned routers are not used. Not an ASI-5 or CHAT-1
unit. Floors unchanged. Broader / general-mind claims **WITHHELD**.

A new certified capsule or chunk may **replace** an old one only if it
**dominates on the old coverage** and every existing CERT composition
that used the old brick still passes hop guards. Otherwise
**add-alongside** (Progressive Nets / MoCL freeze). The old brick is
not deleted.

An explicit replace that would break a CERT composition is **refused**.

Teacher / residual adapters never admit through this door.

## What was already there (not the swap law)

| Existing | What it does | Why it is not this law |
|---|---|---|
| `contract_better_if` / `contract_swap_if_better` | same-contract margin, reliability tie-break | not coverage subset, no hop guards |
| `registry_add_certified` | same name: replace if better, else **reject** | reject is not add-alongside |
| `library_evolve` `contract_already_known` | skip an identical already-certified contract | dedup, not swap |
| HybridCoverage + `route_execute_guarded` | per-hop coverage membership | the guard surface this law reuses |

## Decision

```text
adapter / teacher-residual  -> REFUSE (never admits)
dominate AND compositions hold -> REPLACE
otherwise                      -> ADD-ALONGSIDE
explicit replace when law fails -> REFUSE (old stays)
```

Dominate means: old coverage is a subset of new, **or** the new brick
replays every old coverage row (`btn_certify` on those rows). When both
a new table and a new brick+targets are given, both must hold. Empty
old coverage cannot dominate.

Compression / MAC / `compute_beneficial` are not read.

## Leftover PR

PR #1 (`claude/stash-pull-master-9ee988`, MinGW restore) is leftover:
it is hundreds of commits behind current `origin/master` (`18fce8b`)
and is superseded by later master work. It will be closed after this
lands. This PR does not close it.

## Gate

```text
make cnet_swap
```

No 8B compete. No GitHub Actions. No fake PASS.
