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
expect_fail "member_unopenable" "missing sidecar" \
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

# ---- artifact authority ----------------------------------------------------
# A manifest that merely declares the wrong root: caught by the byte-recomputed
# comparison against the committed protocol constant.
build_cache
sed -i 's/^artifact_root .*/artifact_root 0000000000000000000000000000000000000000000000000000000000000000/' \
    "$W/cache/manifest.txt"
expect_fail "artifact_root_vs_manifest" "manifest declares a false artifact root" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"

# ---- artifact authority: canonical sidecars, substituted pack bytes --------
# The manufactured cache keeps every canonical identity sidecar and rewrites the
# manifest member digests to match its own tampered pack. Only the committed
# artifact root catches it.
build_cache
python3 - "$W/cache" <<'PYEOF'
import hashlib, os, sys
# Manufacture a fully SELF-CONSISTENT cache: substitute feature bytes in a pack,
# then repair the member digest AND recompute the manifest's own artifact root so
# nothing inside the cache disagrees with anything else. Only the root committed
# in the protocol can catch this.
MEM = ["train.pack","val.pack","test.pack","pca.bin","ids_train.txt","ids_val.txt",
       "ids_test.txt","content_train.txt","content_val.txt","content_test.txt"]
d = sys.argv[1]
p = os.path.join(d, "test.pack")
b = bytearray(open(p, "rb").read())
b[-4:] = b"\x01\x02\x03\x04"
open(p, "wb").write(b)
digest = {n: hashlib.sha256(open(os.path.join(d, n), "rb").read()).hexdigest() for n in MEM}
buf = b"VDCACHEROOT1\nschema 1\nmembers 10\n"
for n in MEM:
    buf += ("%s %d %s\n" % (n, os.path.getsize(os.path.join(d, n)), digest[n])).encode()
root = hashlib.sha256(buf).hexdigest()
out = []
for line in open(os.path.join(d, "manifest.txt")).read().split("\n"):
    if line.startswith("sha256_test_pack "):
        out.append("sha256_test_pack " + digest["test.pack"])
    elif line.startswith("artifact_root "):
        out.append("artifact_root " + root)
    else:
        out.append(line)
open(os.path.join(d, "manifest.txt"), "w").write("\n".join(out))
PYEOF
expect_fail "artifact_root_vs_protocol" "self-consistent manufactured cache" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" --json "$JSON"

# ---- BTN failure paths must never yield a verdict -------------------------
for nth in 1 2 3; do
  build_cache
  expect_fail "VD_BENCH_FAIL" "BTN failure injected at call $nth" \
    $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" \
           --btn-fail-at "$nth" --json "$JSON"
done

# A forward failure anywhere in the run -- including in the RANDOM control,
# where a silent 0.0 would LOWER its AP and flatter the ratio and margin -- must
# make the verdict ineligible. Indices are taken from the actual call count of a
# clean run so the injection is guaranteed to be reachable.
build_cache
clean=$(timeout 200 $BENCH --protocol synthetic-test --cache "$W/cache" \
          --prev-test "$W/prev.pack" --json "$JSON" 2>&1 | grep -o 'btn_calls_total=[0-9]*' | cut -d= -f2)
if [ -z "${clean:-}" ] || [ "$clean" -lt 10 ]; then
  echo "  BTN forward injection: FAIL (no call count to target)"; fails=$((fails+1))
else
  for frac in 50 75 95; do
    nth=$(( clean * frac / 100 ))
    build_cache
    out=$(timeout 200 $BENCH --protocol synthetic-test --cache "$W/cache" \
            --prev-test "$W/prev.pack" --btn-fail-at "$nth" --json "$JSON" 2>&1); rc=$?
    checks=$((checks+1))
    if [ $rc -ne 0 ]; then
      echo "  BTN forward failure at ${frac}% of calls: PASS (refused, exit $rc)"
    elif grep -q "eval_fault=1" <<<"$out"; then
      echo "  BTN forward failure at ${frac}% of calls: PASS (verdict ineligible)"
    else
      echo "  BTN forward failure at ${frac}% of calls: FAIL (fault not recorded)"
      fails=$((fails+1))
    fi
  done
fi

# ---- one-snapshot race ------------------------------------------------------
# The cache pathname is exchanged for a DIFFERENT, individually valid cache
# while the scorer runs. Because every scored member is opened once from a
# single dirfd, the run must either complete on the old generation or refuse --
# it must never validate one generation and score another.
build_cache
"$MK" --out "$W/gen2" --prev "$W/gen2.pack" --variant synthetic-test --test-offset 1000 \
      --prev-base 700000 >/dev/null
mkdir -p "$W/race"
cp -r "$W/cache" "$W/race/live"
cp "$W/prev.pack" "$W/race/prev.pack"
(
  sleep 0.35
  rm -rf "$W/race/live.old"
  mv "$W/race/live" "$W/race/live.old" 2>/dev/null
  cp -r "$W/gen2" "$W/race/live" 2>/dev/null
) &
racer=$!
out=$(timeout 200 $BENCH --protocol synthetic-test --cache "$W/race/live" \
        --prev-test "$W/race/prev.pack" --json "$JSON" 2>&1); rc=$?
wait $racer 2>/dev/null
checks=$((checks+1))
if [ $rc -ne 0 ]; then
  echo "  directory exchanged mid-run: PASS (refused, exit $rc)"
elif grep -q "artifact root verified" <<<"$out" && grep -q "results published" <<<"$out"; then
  # completed: it must have used exactly one generation's artifact root
  got=$(grep -o "artifact root verified from held descriptors: [0-9a-f]*" <<<"$out" | awk "{print \$NF}")
  if [ "$got" = "ca9bb6112e7170becc292c5aff9118ab3403ea1f700ddd6ba190bb3408111441" ]; then
    echo "  directory exchanged mid-run: PASS (single consistent snapshot)"
  else
    echo "  directory exchanged mid-run: FAIL (mixed generations: $got)"; fails=$((fails+1))
  fi
else
  echo "  directory exchanged mid-run: FAIL (indeterminate)"; fails=$((fails+1))
fi

# Deterministic variant: replace the pathname with a cache whose bytes differ,
# after the snapshot is taken. The held descriptors must still govern.
build_cache
cp -r "$W/cache" "$W/swap_live"
cp "$W/prev.pack" "$W/swap_prev.pack"
(
  sleep 0.25
  rm -rf "$W/swap_live"
  cp -r "$W/gen2" "$W/swap_live" 2>/dev/null
) &
racer2=$!
out=$(timeout 200 $BENCH --protocol synthetic-test --cache "$W/swap_live" \
        --prev-test "$W/swap_prev.pack" --json "$JSON" 2>&1); rc=$?
wait $racer2 2>/dev/null
checks=$((checks+1))
if [ $rc -ne 0 ] || grep -q "results published" <<<"$out"; then
  echo "  cache replaced mid-run: PASS (one snapshot or clean refusal)"
else
  echo "  cache replaced mid-run: FAIL (indeterminate)"; fails=$((fails+1))
fi

# ---- previous (spent) holdout is one held object ---------------------------
# The prev pack is opened once and both parsed and hashed from that descriptor.
# Replacing the pathname with a DIFFERENT valid pack while the run proceeds must
# never produce a mixed outcome: the run either used the original object
# throughout, or refused cleanly. Repeated across timings.
"$MK" --out "$W/pv2" --prev "$W/prev_other.pack" --variant synthetic-test \
      --test-offset 1000 --prev-base 600000 >/dev/null
if cmp -s "$W/prev.pack" "$W/prev_other.pack"; then
  echo "  prev-pack race: FAIL (packs identical, control would be vacuous)"; fails=$((fails+1))
fi
for delay in 0.02 0.10 0.30; do
  build_cache
  cp "$W/prev.pack" "$W/race_prev.pack"
  ( sleep "$delay"; cp "$W/prev_other.pack" "$W/race_prev.pack" 2>/dev/null ) &
  swapper=$!
  out=$(timeout 200 $BENCH --protocol synthetic-test --cache "$W/cache" \
          --prev-test "$W/race_prev.pack" --json "$JSON" 2>&1); rc=$?
  wait $swapper 2>/dev/null
  checks=$((checks+1))
  if [ $rc -ge 128 ]; then
    echo "  prev-pack replaced at ${delay}s: FAIL (signal $((rc-128)))"; fails=$((fails+1))
  elif [ $rc -eq 0 ] && grep -q "results published" <<<"$out"; then
    echo "  prev-pack replaced at ${delay}s: PASS (one held object throughout)"
  elif grep -qE "VD_BENCH_FAIL prev_test_" <<<"$out"; then
    echo "  prev-pack replaced at ${delay}s: PASS (clean refusal)"
  else
    echo "  prev-pack replaced at ${delay}s: FAIL (indeterminate, exit $rc)"; fails=$((fails+1))
  fi
done

# ---- worker lifecycle fault matrix ----------------------------------------
# Every worker failure mode must refuse and leave no child behind. Short
# test-only deadlines; the production deadline is 3600 s against a measured
# ~1117 s shuffle arm.
count_children() { pgrep -P $$ -x vd_bench 2>/dev/null | wc -l; }

worker_case() {
  local name="$1"; shift
  local out rc before after
  before=$(pgrep -x vd_bench | wc -l)
  out=$(timeout 300 "$@" 2>&1); rc=$?
  sleep 0.4
  after=$(pgrep -x vd_bench | wc -l)
  checks=$((checks+1))
  if [ $rc -eq 0 ]; then
    echo "  $name: FAIL (exit 0, expected refusal)"; fails=$((fails+1)); return
  fi
  if [ $rc -ge 128 ]; then
    # a signal death is a crash, not a controlled refusal
    echo "  $name: FAIL (died on signal $((rc-128)), expected clean refusal)"
    fails=$((fails+1)); return
  fi
  if grep -q "VISION_DETECTION_MECHANISM_PASS" <<<"$out"; then
    echo "  $name: FAIL (PASS printed despite worker fault)"; fails=$((fails+1)); return
  fi
  if [ "$after" -gt "$before" ]; then
    echo "  $name: FAIL (orphaned worker: $before -> $after)"; fails=$((fails+1)); return
  fi
  echo "  $name: PASS (refused exit $rc, no orphan)"
}

build_cache
worker_case "worker hangs (deadline enforced)" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" \
         --jobs 2 --worker-fault hang --shuffle-deadline-s 3 --json "$JSON"
build_cache
worker_case "worker exits early" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" \
         --jobs 2 --worker-fault exit --shuffle-deadline-s 20 --json "$JSON"
build_cache
worker_case "worker writes a partial message" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" \
         --jobs 2 --worker-fault partial --shuffle-deadline-s 5 --json "$JSON"
build_cache
worker_case "worker crashes abnormally" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" \
         --jobs 2 --worker-fault crash --shuffle-deadline-s 20 --json "$JSON"
build_cache
worker_case "parent faults after fork" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" \
         --jobs 2 --parent-fault-after-fork --shuffle-deadline-s 20 --json "$JSON"
build_cache
worker_case "BTN init fails after fork" \
  $BENCH --protocol synthetic-test --cache "$W/cache" --prev-test "$W/prev.pack" \
         --jobs 2 --btn-fail-at 4 --shuffle-deadline-s 20 --json "$JSON"

# jobs=1 and jobs=2 must both produce a complete, published run
for j in 1 2; do
  build_cache
  out=$(timeout 300 $BENCH --protocol synthetic-test --cache "$W/cache" \
          --prev-test "$W/prev.pack" --jobs $j --json "$JSON" 2>&1); rc=$?
  checks=$((checks+1))
  if [ $rc -eq 0 ] && grep -q "results published" <<<"$out"; then
    echo "  jobs=$j smoke run completes: PASS"
  else
    echo "  jobs=$j smoke run completes: FAIL (exit $rc)"; fails=$((fails+1))
  fi
done

echo "checks=$checks failures=$fails"
if [ "$fails" -eq 0 ]; then
  echo "VISION_DETECTION_EVIDENCE_PASS checks=$checks"
  exit 0
fi
echo "VISION_DETECTION_EVIDENCE_FAIL failures=$fails"
exit 1
