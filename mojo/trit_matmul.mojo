# Ternary (1.6-bit packed) matvec — the Mojo side of the C ABI bridge.
#
# NO PYTHON. This file uses only Mojo and the C ABI. It is compiled to a
# shared library; the C side declares these entry points BY HAND in
# include/cce/cce_mojo_kernel.h. Mojo never generates that header.
#
# ---------------------------------------------------------------------------
# SYNTAX VERIFIED — first compiled 2026-08-17 against Mojo 1.0.0 (ed45d567).
#
# This file previously carried an UNVERIFIED SYNTAX banner: it was authored on
# 2026-08-16 on a Windows box that cannot run Mojo, and had never been built.
# That first Linux build has now happened and corrected every open question the
# banner listed:
#
#   1. `abi("C")` placement — it is a function effect and goes AFTER the
#      argument list and BEFORE the return arrow:
#          @export("name")
#          def name(args) abi("C") -> Ret:
#      Verified by `nm -D`: the symbol lands as a plain `T name`.
#   2. `@export` alone is NOT sufficient. Without the effect the compiler
#      errors: "@export requires an explicit 'abi()' effect on the function".
#   3. Import paths — the stdlib now requires the `std.` prefix:
#      `from std.math import exp`, `from std.runtime import initialize_runtime`.
#      `UnsafePointer` is in the prelude and must NOT be imported.
#   4. `fn` was removed in 1.0 and is a hard parse error; every function here
#      is a `def`.
#
# The kernel body was unaffected by all of the above — only the signatures and
# imports changed, exactly as the original banner predicted.
# ---------------------------------------------------------------------------
#
# BIT-IDENTITY REQUIREMENT (spec section 2.2). The C reference accumulates
# per-output over i ASCENDING:
#
#     for i: for o: out[o] += a * (float)codes[o]
#
# This kernel must do the same. Do NOT vectorise across i, do NOT tree-reduce,
# do NOT reorder. Byte-equality also depends on both compilers making the same
# FMA-contraction decision, which is an empirical question — if the equivalence
# gate fails, measure the divergence and record it in the spec before touching
# the bar.
#
# Spec: docs/superpowers/specs/2026-08-16-mojo-kernel-bridge-design.md

from std.math import exp
from std.runtime import initialize_runtime

# UnsafePointer is a prelude type in 1.0 — importing it is an error.


@export("cnet_mojo_init")
def cnet_mojo_init() abi("C") -> None:
    # Idempotent. The C dispatch layer calls this once before the first kernel
    # invocation, because no Mojo main() runs in a C-hosted shared library.
    initialize_runtime()


# code(b, k) == ((b // 3^k) % 3) - 1, matching include/cce/cce_trit_lut.h for
# ALL 256 byte values including the >= 243 ones that only corrupt files
# contain — the C table is defined over the full range and the mutate fuzz gate
# depends on that behaviour being unchanged.
@always_inline
def trit_code(b: UInt8, k: Int) -> Int:
    var v = Int(b)
    var d = 1
    for _ in range(k):
        d *= 3
    return (v // d) % 3 - 1


@export("cnet_mojo_trit_matmul")
def cnet_mojo_trit_matmul(
    # Raw pointers cross the C ABI, so their origins cannot be tracked by the
    # compiler — they are stated explicitly. Inputs are read-only; only
    # `output` is written.
    input: UnsafePointer[Float32, ImmUntrackedOrigin],
    w_trit: UnsafePointer[UInt8, ImmUntrackedOrigin],
    w_scale: UnsafePointer[Float32, ImmUntrackedOrigin],
    bias: UnsafePointer[Float32, ImmUntrackedOrigin],
    output: UnsafePointer[Float32, MutUntrackedOrigin],
    in_dim: Int32,
    out_dim: Int32,
    w_trit_bpr: Int32,
    apply_sigmoid: Int32,
) abi("C") -> Int32:
    var n_in = Int(in_dim)
    var n_out = Int(out_dim)
    var bpr = Int(w_trit_bpr)

    # Declining is normal, not an error: the C reference then runs.
    if n_in <= 0 or n_out <= 0 or bpr <= 0:
        return 1

    for o in range(n_out):
        output[o] = Float32(0.0)

    # i ASCENDING, accumulating into each output — the exact order the C
    # kernel uses and the one its bit-identity claim rests on.
    for i in range(n_in):
        var a = input[i]
        var row = w_trit + i * bpr
        for o in range(n_out):
            var code = trit_code(row[o // 5], o % 5)
            output[o] = output[o] + a * Float32(code)

    for o in range(n_out):
        var v = bias[o] + w_scale[o] * output[o]
        if apply_sigmoid != 0:
            v = Float32(1.0) / (Float32(1.0) + exp(-v))
        output[o] = v

    return 0
