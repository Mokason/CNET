#!/usr/bin/env bash
# Pins the ISA of the tracked release artifacts — make artifact_isa_gate.
#
# cnet.so is versioned on purpose (docs/RELEASE_POLICY.md: build-only clones
# must receive the accepted ABI artifact), so whatever is committed is what
# consumers get. Nothing previously constrained HOW it was built, and the
# Makefile rebuilds it inside `verify` at whatever ARCH_CFLAGS the invocation
# happened to carry. Both failure modes have actually occurred here:
#
#   -march=native   -> AVX-512, SIGILLs on any CPU without it (incl. current
#                      Intel consumer parts). This is the bug PORTABLE exists
#                      to prevent.
#   PORTABLE=1      -> baseline, ~3x slower on the int8 oracle matvec.
#
# Shipped artifacts are built PORTABLE=v3: AVX2 present, AVX-512 absent.
#
# The check reads the COMMITTED blob, not the working tree. A local `make ci`
# legitimately rewrites these files at whatever ISA that build used; what must
# stay correct is the thing under version control. Reading the blob also makes
# the gate independent of whatever the developer last built.
#
# Scope honestly: this detects the two regressions above by looking for AVX2
# and AVX-512 register operands. It is not a proof of full ISA conformance —
# an EVEX-encoded VL instruction using only xmm/ymm and no mask register would
# slip past. It is a tripwire for the mistakes that actually happen, not a
# verifier.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "ARTIFACT_ISA_PASS status=skipped_not_a_git_checkout"
    exit 0
fi
if ! command -v objdump >/dev/null 2>&1; then
    echo "ARTIFACT_ISA_FAIL reason=objdump_unavailable" >&2
    exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
failures=0

for artifact in cnet.so cce.dll; do
    if ! git ls-files --error-unmatch -- "$artifact" >/dev/null 2>&1; then
        echo "ARTIFACT_ISA_FAIL reason=untracked artifact=$artifact" >&2
        failures=$((failures + 1))
        continue
    fi
    blob="$tmp/$artifact"
    git cat-file blob "HEAD:$artifact" > "$blob"
    dis="$tmp/$artifact.dis"
    objdump -d "$blob" > "$dis" 2>/dev/null || true

    ymm=$(grep -cE '%ymm[0-9]' "$dis" || true)
    zmm=$(grep -cE '%zmm[0-9]' "$dis" || true)
    kreg=$(grep -cE '%k[0-7]' "$dis" || true)

    if [ "$zmm" -ne 0 ] || [ "$kreg" -ne 0 ]; then
        echo "ARTIFACT_ISA_FAIL artifact=$artifact reason=avx512_present" \
             "zmm=$zmm kmask=$kreg — rebuild with 'make PORTABLE=v3'," \
             "not the default native build" >&2
        failures=$((failures + 1))
    elif [ "$ymm" -eq 0 ]; then
        echo "ARTIFACT_ISA_FAIL artifact=$artifact reason=no_avx2" \
             "— looks like a PORTABLE=1 baseline build, which costs ~3x on the" \
             "int8 oracle matvec; rebuild with 'make PORTABLE=v3'" >&2
        failures=$((failures + 1))
    else
        echo "  $artifact: AVX2 ymm=$ymm, no AVX-512 (zmm=0 kmask=0)"
    fi
done

if [ "$failures" -ne 0 ]; then
    echo "$failures FAILURES" >&2
    exit 1
fi
echo "ARTIFACT_ISA_PASS status=committed_blobs_are_x86-64-v3"
