# DreamCoder type logic in CNET Core

Status: **UNIT + BENCH PASS — TYPED CORE ONLY**

DreamCoder (Ellis et al., PLDI 2021; `dreamcoder/type.py`, `type.ml`)
uses Hindley–Milner types to keep enumeration well-typed:

- `TypeConstructor` / `TypeVariable`
- `instantiate` (freshen)
- `unify` with occurs check
- `apply` substitution
- arrows `a -> b`, `list`, `pair`

CNET already had the wake/sleep *library* loop (`library_evolve`). It did
not have the type kernel. Core composition used monomorphic
`port_compatible` only.

## What landed

Native C module `cnet_dc_type` (no Python, no residual voice):

- same operations as DreamCoder's Core type module
- Port / Contract mapped onto DC types
- `library_evolve` route distillation fail-closed if a plan is ill-typed
- existing monomorphic ports keep the same meaning as `port_compatible`

Not an ASI-5 or CHAT-1 unit. Floors unchanged. Broader claims
**WITHHELD**.

## Gates

```text
make cnet_dc_type          # CNET_DC_TYPE_PASS
make cnet_dc_type_bench    # CNET_DC_TYPE_BENCH_PASS
make library               # existing DreamCoder-style evolve loop
```
