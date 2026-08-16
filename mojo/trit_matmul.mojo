# Ternary (1.6-bit packed) matvec — the Mojo side of the C ABI bridge.
#
# NO PYTHON. This file uses only Mojo and the C ABI. It is compiled to a
# shared library; the C side declares these entry points BY HAND in
# include/cce/cce_mojo_kernel.h. Mojo never generates that header.
#
# ---------------------------------------------------------------------------
# UNVERIFIED SYNTAX — READ BEFORE BUILDING
#
# This file has NEVER been compiled. Mojo reached 1.0 on 2026-08-12, four days
# before it was written, and it was authored on a Windows box that cannot run
# Mojo at all (no native Windows support; WSL2 was declined). The following
# are inferences from the 1.0 docs, not verified facts, and the FIRST build on
# the Linux box should expect to correct them:
#
#   1. The placement of the `abi("C")` function effect. The 1.0 docs describe
#      `abi("C")` for FFI *callbacks* ("You must mark it with abi(\"C\")") but
#      do not show the export direction. If the form below is wrong, the fix
#      is a syntax change only — the kernel body is independent of it.
#   2. Whether `@export` alone already gives C linkage, making `abi("C")`
#      redundant here.
#   3. The import path for `exp`. Adjust to whatever 1.0 actually provides.
#
# What IS verified (docs.modular.com / mojolang.org, 2026-08-16):
#   - C interop is via the ffi module, the @export decorator, and the abi("C")
#     function effect.
#   - @export("name") sets a custom linkage name.
#   - `mojo build --emit shared-lib` produces the shared library.
#   - runtime.initialize_runtime() initialises the Mojo runtime when Mojo code
#     built as a shared library is called from a non-Mojo host such as C. It is
#     idempotent and required before runtime-dependent APIs like parallelize().
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

from math import exp
from memory import UnsafePointer
from runtime import initialize_runtime


@export("cnet_mojo_init")
fn cnet_mojo_init() -> None:
    # Idempotent. The C dispatch layer calls this once before the first kernel
    # invocation, because no Mojo main() runs in a C-hosted shared library.
    initialize_runtime()


# code(b, k) == ((b // 3^k) % 3) - 1, matching include/cce/cce_trit_lut.h for
# ALL 256 byte values including the >= 243 ones that only corrupt files
# contain — the C table is defined over the full range and the mutate fuzz gate
# depends on that behaviour being unchanged.
@always_inline
fn trit_code(b: UInt8, k: Int) -> Int:
    var v = Int(b)
    var d = 1
    for _ in range(k):
        d *= 3
    return (v // d) % 3 - 1


@export("cnet_mojo_trit_matmul")
fn cnet_mojo_trit_matmul(
    input: UnsafePointer[Float32],
    w_trit: UnsafePointer[UInt8],
    w_scale: UnsafePointer[Float32],
    bias: UnsafePointer[Float32],
    output: UnsafePointer[Float32],
    in_dim: Int32,
    out_dim: Int32,
    w_trit_bpr: Int32,
    apply_sigmoid: Int32,
) -> Int32:
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
