# Mojo Kernel Bridge — one kernel source for NVIDIA and AMD, behind a C ABI

Status: DESIGN (no code yet). Date: 2026-08-16.
Origin: "Integrating Mojo framework inside this stack, but without using
python, keeping C/C++." Goal chosen: collapse the multi-backend GPU problem —
"for GPU this will be a must to have."

Scope: the bridge (C ABI, build probe, dispatch, equivalence and benchmark
gates) plus the first kernel. **The CPU trit kernel is stage 1 and is NOT the
goal** — it is the safest surface on which to prove the toolchain end to end.
The goal is stage 2: a GPU trit kernel compiled from one source to both
AMDGPU IR and PTX. §8 states what stage 2 needs.

---

## 1. Why now, and the facts this rests on

Verified 2026-08-16, not recalled:

| fact | status |
|---|---|
| Mojo can export C-ABI symbols with no Python | **YES** — `@export` decorator, `abi("C")` function effect, `ffi` module; shared libraries callable from C |
| Mojo 1.0 | **released 2026-08-12**, four days before this spec; stable API after three years of churn |
| AMD RDNA consumer GPU support | **YES since Modular Platform 26.2** (March 2026); MI300/MI325 since 25.4. Mojo emits AMDGPU IR natively — one kernel compiles to ROCm and PTX with no source change |
| Native Windows | **NO** — WSL2 only; a native port is still "mid-term" |
| Compiler licence | **proprietary**; stdlib Apache-2.0 since 2024; open-sourcing committed for Fall 2026 |
| Governance | **Qualcomm closed its acquisition of Modular on 2026-07-28** |

The motivating problem is already in the tree: `cce_clgemm` (OpenCL) is the
only GPU backend carrying backward/STE GEMMs, while `hipgemm` and `cudagemm`
are forward-only. Training is therefore pinned to one backend per vendor.
Mojo's single-source → PTX + AMDGPU IR is a direct answer to that.

## 2. Two constraints that bound what can be claimed

### 2.1 Only the AMD half is verifiable on current hardware

"One source → NVIDIA + AMD" needs both vendors exercised. The NVIDIA card
(4070 Ti Super) sits in the **Windows** box, and Mojo has no native Windows
support; WSL2 was declined. The Linux box is 2×R9700 — AMD.

So: **compiles for both** and **verified on both** are different statements.
This spec only claims the former for NVIDIA. Any NVIDIA claim requires WSL2,
a CI runner, or another NVIDIA host, and until one exists the NVIDIA path is
recorded as unverified rather than assumed working.

### 2.2 Bit-identity here is a float problem, not an integer one

An early assumption in discussion — that the trit kernel is integer and so
byte-equality is easy — is **wrong**, and the source says so. From
`src/cce/cce_block.c`, the inner loop is:

```c
for (int o = 0; o < w; ++o) out[o] += a * (float)codes[o];
```

The trits decode to `int8` codes, but the activation `a` is float, so the
accumulation is float. The existing comment relies on this explicitly:
*"each output o still accumulates a*code over i ascending -> bit-identical to
the int8 ternary path."*

Byte-equality therefore requires the Mojo kernel to reproduce **both**:
1. per-output accumulation order, `i` ascending, no reassociation and no tree
   reduction; and
2. the same FMA-contraction decision as the C build — GCC defaults to
   `-ffp-contract=fast`, so the C side may already be fusing `a*code + out`
   into one rounding step.

(2) is an empirical question about two different compilers, not something
this design can guarantee. **Fallback, stated up front:** if byte-equality is
unattainable on the Linux box, the gate drops to a stated epsilon AND the
measured divergence and its cause are written into this spec. The bar is not
loosened silently.

## 3. Architecture

```
cce_block.c ──► cce_mojo_dispatch_trit() ──► C trit kernel    (always compiled)
                                        └──► Mojo trit kernel (iff CNET_HAVE_MOJO
                                                               AND CNET_MOJO=1)
```

- **Dispatch** is one C function mirroring the existing kernel's signature.
- **Default is OFF.** Even with the toolchain present and `CNET_HAVE_MOJO`
  defined, the Mojo path runs only when `CNET_MOJO=1`. Every existing gate
  keeps running the C path unless deliberately asked otherwise, mirroring how
  `CNET_GPU=1` gates the OpenCL path today.
- **No Mojo type crosses the boundary.** Entry points take primitives and
  pointers only. `include/cce/cce_mojo_kernel.h` is hand-written C and is the
  contract both sides compile against — Mojo never generates it.

## 4. Build integration

`MOJO_PROBE` mirrors the existing `CURL_PROBE`: if `mojo` is on PATH, build
`libcnet_mojo` and add `-DCNET_HAVE_MOJO`; otherwise the dispatch layer
compiles with the Mojo branch preprocessed out and nothing else changes.

A box without the toolchain — including the Windows test box — builds and
gates exactly as today. Link-time rather than `dlopen` keeps the artifact
pinned and under the same digest discipline as everything else; a runtime
`.so` could drift from what was recorded.

## 5. Removability is a requirement, not a nicety

The compiler is proprietary and its owner changed three weeks ago. Deleting
`mojo/`, one Makefile stanza and one `#ifdef` must return the tree to exactly
today's behaviour. A gate asserts the C path is bit-identical with the Mojo
path compiled out, so this stays true rather than being believed.

Mojo is never load-bearing: no CNET capability may exist only in Mojo.

## 6. Gates

1. **Harness self-proof.** With no Mojo present, the equivalence harness
   compares the C kernel against itself and must report byte-equality over
   the full shape matrix. This proves the harness works *before* it ever sees
   a second implementation — the same discipline used for the attribution
   registry gate.
2. **Compiled-out identity.** With `CNET_HAVE_MOJO` undefined, `cce_block`
   output is bit-identical to the pre-bridge build. This is what makes §5 a
   property rather than a hope.

   Measured the same way the QAT block's legacy anchor was: the harness runs
   a fixed-seed deterministic case through the C kernel and prints an
   **anchor** — an FNV digest over the raw output bytes — which is recorded
   in the implementation plan the first time it runs, before the dispatch
   layer is introduced. Every later gate must reproduce that digest. A
   printed float would not be sufficient; the digest covers every output
   byte, not one value.
3. **Default-off identity.** With `CNET_HAVE_MOJO` defined but `CNET_MOJO`
   unset, output is bit-identical to the C path.
4. **Equivalence** (Linux, Mojo present). C vs Mojo, byte-for-byte, over a
   deterministic shape matrix covering: tile boundaries (`in_dim`/`out_dim`
   either side of the 510 tile width), the packed-byte edge (dims not a
   multiple of 5), a single-row case, and a case large enough to cross the
   OpenMP threshold (`in_dim*out_dim >= 1<<21`).
5. **Benchmark** (Linux). C vs Mojo on identical shapes, reported against the
   existing 27.9× trit kernel. This is the number that decides whether the
   bridge earns its place.
6. Existing gates unaffected: `make trit_bench`, `make mutate`,
   `make qat_block`, `make attribution` all still pass.

Gates 1–3 run on any box including Windows. Gates 4–5 are Linux-only and are
stated as such rather than assumed.

## 7. Error handling

- Probe absent → no Mojo, no error, no behaviour change.
- `CNET_MOJO=1` with `CNET_HAVE_MOJO` undefined → one warning to stderr, then
  the C path. Silently ignoring the request would hide a misconfigured box.
- Mojo entry point returns non-zero → fall back to the C path for that call
  and count it. A kernel that fails must not fail the model.
- Shape unsupported by the Mojo kernel → dispatch returns "not handled" and
  the C path runs. The Mojo kernel is allowed to be partial.

## 8. Stage 2 — the actual goal

Stage 1 proves the bridge on CPU. Stage 2 is the reason for the exercise:

- the same ternary kernel compiled to **AMDGPU IR** for the 2×R9700 and to
  **PTX** for NVIDIA hosts, from one source;
- equivalence gated against `cce_clgemm` on AMD;
- the NVIDIA half compiled and, until a host exists, explicitly unverified.

Stage 2 gets its own spec. It is deliberately not attempted here, because a
GPU kernel whose bridge is unproven would confound two unknowns — the same
sequencing argument that put the attribution core before the ternary
proposer.

## 9. Explicitly out of scope

- Any Python, at any point, including Mojo's Python interop.
- Replacing C for general development; Mojo is a kernel language here.
- FP GEMM (`cce_clgemm`) — it cannot be byte-identical across vendors and is
  a stage-2-or-later question.
- MAX (the inference engine). Only the Mojo language and its C ABI are used.
