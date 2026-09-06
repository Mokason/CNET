#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d /tmp/cnet-finite-compile-XXXXXX)
trap 'rc=$?; if ((rc)); then echo CAPSULE_FINITE_COMPILE_RED; fi; rm -rf -- "$root"' EXIT
mkdir "$root/capsules"
for ((x=0; x<256; x++)); do printf '%d %d\n' "$x" "$((65535-x))"; done > "$root/rows.tsv"
for ((x=0; x<256; x++)); do printf 'word complemented %d %d\n' "$x" "$((65535-x))"; done > "$root/eval.tsv"
CNET_CAPSULE_EVAL_FILE="$root/eval.tsv" bin/cnet_capsule_core teach "$root/capsules" finite_complement word complemented 16 16 verified_tool "$root/rows.tsv" > "$root/receipt"
grep -q 'cases=256 before_correct=0 after_correct=256 wrong_certified=0' "$root/receipt"
grep -q 'method=finite_domain_compile' "$root/receipt"
if bin/cnet_capsule_core ask "$root/capsules" 'capsule word complemented 256'; then exit 1; fi
echo CAPSULE_FINITE_COMPILE_PASS
