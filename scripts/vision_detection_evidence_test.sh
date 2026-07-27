#!/usr/bin/env bash
# Negative controls for the bench's evidence gate.
#
# Every case here is an attempt to obtain a scored run from a cache that cannot
# prove where it came from. All of them must fail closed: nonzero exit, the
# specific refusal marker, and never VISION_DETECTION_MECHANISM_PASS.
#
# Cached/synthetic only: no VOC data, no network, no GPU.
set -uo pipefail
export ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES=''

W="${TMPDIR:-/tmp}/vd_evidence_$$"
BENCH=./bin/vd_bench
MK=./bin/vd_mkcache
JSON="$W/out.json"
fails=0
checks=0

cleanup() { rm -rf "$W"; }
trap cleanup EXIT

mkdir -p "$W"

# expect_fail <marker> <name> -- run must exit nonzero, print marker, and never PASS
expect_fail() {
  local marker="$1" name="$2"; shift 2
  local out rc
  out=$(timeout 120 "$@" 2>&1); rc=$?
  checks=$((checks+1))
  if [ $rc -eq 0 ]; then
    echo "  $name: FAIL (exit 0, expected refusal)"; fails=$((fails+1)); return
  fi
  if grep -q "VISION_DETECTION_MECHANISM_PASS" <<<"$out"; then
    echo "  $name: FAIL (printed PASS while refusing)"; fails=$((fails+1)); return
  fi
  if ! grep -q "$marker" <<<"$out"; then
    echo "  $name: FAIL (missing marker '$marker')"
    echo "$out" | tail -3 | sed 's/^/      /'
    fails=$((fails+1)); return
  fi
  echo "  $name: PASS (refused with $marker)"
}

expect_gate_open() {
  local name="$1"; shift
  local out rc
  out=$(timeout 120 "$@" 2>&1); rc=$?
  checks=$((checks+1))
  # A complete synthetic run: the gate opens, the run scores, and the results
  # document is published. Metrics are meaningless here by construction; what
  # this asserts is that the evidence and publication paths work end to end.
  if grep -q "roots match pinned protocol roots" <<<"$out" && grep -q "results published" <<<"$out" \
     && [ $rc -eq 0 ]; then
    echo "  $name: PASS (gate opened, run completed, results published)"
  else
    echo "  $name: FAIL (gate did not open)"
    echo "$out" | tail -4 | sed 's/^/      /'
    fails=$((fails+1))
  fi
  return 0
}

echo "== vision benchmark evidence-gate negative controls =="

build_cache() {
  rm -rf "$W/cache" "$W/prev.pack"
  "$MK" --out "$W/cache" --prev "$W/prev.pack" --variant synthetic-test --test-offset 1000 >/dev/null || exit 9
}

# 0. The synthetic cache is deliberately NOT the real V2 cache: its roots do not
# match the pinned protocol roots, so the gate must refuse it. That is the point
# -- protocol identity cannot be manufactured.
build_cache
expect_fail "protocol_mismatch" "synthetic cache cannot pass as VOC/v2" \
  $BENCH --protocol v2 --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"

expect_gate_open "synthetic cache under its own pinned protocol" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"

# 0b. Codex's exact bypass: a cache self-declaring test_offset 0 must not be
# excused from the spent-holdout requirement, which now comes from the protocol.
rm -rf "$W/bypass" "$W/bypass.pack"
"$MK" --out "$W/bypass" --prev "$W/bypass.pack" --variant synthetic-test --test-offset 0 >/dev/null
expect_fail "protocol_mismatch" "offset=0 bypass (no --prev-test)" \
  $BENCH --protocol synthetic-test --cache "$W/bypass" --json "$JSON"

# 0b-ii. and on a fully protocol-conformant cache, simply omitting --prev-test
# must still fail: the requirement comes from the protocol, not the manifest.
build_cache
expect_fail "prev_test_required" "missing --prev-test on a conformant cache" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --json "$JSON"
expect_fail "protocol_mismatch" "offset=0 bypass (with prev-test)" \
  $BENCH --protocol synthetic-test --cache "$W/bypass" --prev-test "$W/bypass.pack" --json "$JSON"

# 0c. a scoring run with no protocol selected at all
build_cache
expect_fail "protocol_required" "no --protocol given" \
  $BENCH --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"
expect_fail "unknown_protocol" "unknown protocol name" \
  $BENCH --protocol v9 --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"

# 1. wrong class / seed / counts in an otherwise coherent manifest
build_cache
sed -i 's/^class car/class bus/' "$W/cache/manifest.txt"
expect_fail "protocol_mismatch" "wrong class" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"
build_cache
sed -i 's/^seed 20260727/seed 1234/' "$W/cache/manifest.txt"
expect_fail "protocol_mismatch" "wrong seed" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"
build_cache
sed -i 's/^pca_fit_rows 8/pca_fit_rows 99/' "$W/cache/manifest.txt"
expect_fail "protocol_mismatch" "wrong PCA-fit provenance" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"
build_cache
sed -i 's/^train_img 400/train_img 401/' "$W/cache/manifest.txt"
expect_fail "protocol_mismatch" "wrong train count" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"
build_cache
sed -i 's/^seed 20260727/seed 20260727\nseed 20260727/' "$W/cache/manifest.txt"
expect_fail "manifest_duplicate_key" "duplicate manifest key" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"
build_cache
printf 'rogue_key 1\n' >> "$W/cache/manifest.txt"
expect_fail "manifest_unknown_key" "unknown manifest key" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"

# 2. no manifest at all
build_cache; rm -f "$W/cache/manifest.txt"
expect_fail "manifest_unreadable" "manifest deleted" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"

# 3. a pack edited after extraction.
# Flip a byte to a value it definitely did not already hold -- writing a
# coincidentally identical byte would make this control vacuous.
build_cache
orig=$(od -An -tu1 -j 64 -N 1 "$W/cache/test.pack" | tr -d ' ')
new=$(( (orig + 1) % 256 ))
printf "$(printf '\\%03o' "$new")" | dd of="$W/cache/test.pack" bs=1 seek=64 conv=notrunc status=none
if cmp -s <(od -An -tu1 -j 64 -N 1 "$W/cache/test.pack" | tr -d ' ') <(echo "$orig"); then
  echo "  test.pack tamper: FAIL (byte unchanged, control would be vacuous)"; fails=$((fails+1))
fi
expect_fail "artefact_hash_mismatch" "test.pack tampered" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"

# 4. manifest counts edited to describe a different run
build_cache
sed -i 's/^test_img .*/test_img 999/' "$W/cache/manifest.txt"
expect_fail "protocol_mismatch" "manifest counts tampered" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"

# 5. manifest that admits leakage must not be scored
build_cache
sed -i 's/^trainval_id_overlap .*/trainval_id_overlap 3/' "$W/cache/manifest.txt"
expect_fail "protocol_mismatch" "manifest declares leakage" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"

# 6. stale mix: a prev-test pack that is not the one prep checked against
build_cache
# a genuinely different prev pack: same shape, different image IDs
"$MK" --out "$W/other" --prev "$W/other.pack" --variant synthetic-test --test-offset 1000 \
      --prev-base 800000 >/dev/null
if cmp -s "$W/prev.pack" "$W/other.pack"; then
  echo "  prev-test mix: FAIL (packs identical, control would be vacuous)"; fails=$((fails+1))
fi
expect_fail "prev_test_digest_mismatch" "prev-test from a different prep" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/other.pack" --json "$JSON"

# 7. partial spent-holdout evidence
build_cache
sed -i 's/^prev_test_sha_checked .*/prev_test_sha_checked 1/' "$W/cache/manifest.txt"
expect_fail "protocol_mismatch" "partial prev-holdout evidence" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"

# 7b. mixed sidecars: a sidecar swapped between splits
build_cache
cp "$W/cache/ids_val.txt" "$W/cache/ids_test.txt"
expect_fail "artefact_hash_mismatch" "mixed sidecar (val list as test)" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"

# 7c. a missing sidecar entirely
build_cache
rm -f "$W/cache/content_test.txt"
expect_fail "artefact_unreadable" "missing sidecar" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"

# 8. corrupt pack body
build_cache
head -c 40 "$W/cache/val.pack" > "$W/cache/val.pack.trunc" && mv "$W/cache/val.pack.trunc" "$W/cache/val.pack"
expect_fail "pack_invalid" "val.pack truncated" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"

# 9. results must be publishable; an unwritable destination cannot yield a pass
build_cache
expect_fail "json_publish_failed" "unwritable json destination" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$W/no_such_dir/out.json"

echo "checks=$checks failures=$fails"
if [ "$fails" -eq 0 ]; then
  echo "VISION_DETECTION_EVIDENCE_PASS checks=$checks"
  exit 0
fi
echo "VISION_DETECTION_EVIDENCE_FAIL failures=$fails"
exit 1
