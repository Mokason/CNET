#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
[[ $# -eq 0 || ( $# -eq 1 && $1 == --local-baseline ) ]] || {
  echo "usage: bash run.sh [--local-baseline]" >&2; exit 2;
}
out=$(mktemp -d /tmp/cnet-controller-run-XXXXXX)
echo "CONTROLLER_ARTIFACTS $out"
make -C "$here" OUT="$out/build" test gpu-test build local-build >"$out/tests.log" 2>&1
(cd "$here" && sha256sum ./*.c ./*.h ./*.mjs ./Makefile ./run.sh) >"$out/source.sha256"
cd "$out"
./build/controller freeze >freeze.log
sha256sum train.tsv validation.tsv test.tsv >fixtures.sha256
./build/controller run >results.jsonl 2>progress.log
if [[ $# -eq 1 ]]; then
  for ((i=0;i<32;i++)); do ./build/local_baseline "$i"; done >local-baseline.jsonl
fi
node "$here/audit.mjs" "$out" >audit.json
sha256sum ./*.tsv ./*.jsonl ./*.json ./*.native-weights >SHA256SUMS
echo "CONTROLLER_EXPERIMENT_PASS artifacts=$out broader_claims=WITHHELD"
