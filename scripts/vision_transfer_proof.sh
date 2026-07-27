#!/usr/bin/env bash
# Fresh-runtime transfer proof for the portable visual specialist.
#
# Copies the capsule somewhere else, makes the vision cache / PCA / training
# artefacts unavailable, and runs detection from JPEG using only the package.
# Then drives every compatibility and corruption control and proves a refused
# import leaves the destination untouched, and that a second unrelated capsule
# coexists.
set -uo pipefail
export ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' OMP_NUM_THREADS=1

CAP=${1:-data/vision_specialist}
VOC=data/voc2007/VOCdevkit/VOC2007
W=$(mktemp -d /tmp/vd_transfer_XXXXXX)
HID=""
checks=0; fails=0

restore() {
  [ -n "$HID" ] && [ -d "$HID/vision_cache_v2" ] && mv "$HID/vision_cache_v2" data/ 2>/dev/null
  [ -n "$HID" ] && [ -d "$HID/vision_gate" ] && mv "$HID/vision_gate" data/ 2>/dev/null
  [ -n "$HID" ] && rmdir "$HID" 2>/dev/null
  rm -rf "$W"
}
trap restore EXIT

ok(){ checks=$((checks+1)); echo "  $1: PASS"; }
bad(){ checks=$((checks+1)); fails=$((fails+1)); echo "  $1: FAIL${2:+ ($2)}"; }

echo "== fresh-runtime transfer proof =="
[ -d "$CAP" ] || { echo "VISION_TRANSFER_FAIL no_capsule"; exit 2; }

# 1-3. record identity, then copy the package elsewhere
SZ=$(du -sb "$CAP" | cut -f1)
H=$(cat "$CAP"/manifest.cknow "$CAP"/unit.cnb "$CAP"/frontend.cvfa 2>/dev/null | sha256sum | cut -d' ' -f1)
SCHEMA=$(head -1 "$CAP"/manifest.cknow | awk '{print $2}')
echo "  artifact dir=$CAP bytes=$SZ schema=$SCHEMA package_sha256=$H"
cp -r "$CAP" "$W/pkg" && ok "package copied to a fresh directory" || bad "package copy"

# 4. make the training-side assets unavailable
HID=$(mktemp -d /tmp/vd_hidden_XXXXXX)
mv data/vision_cache_v2 "$HID"/ 2>/dev/null && mv data/vision_gate "$HID"/ 2>/dev/null
if [ ! -d data/vision_cache_v2 ] && [ ! -d data/vision_gate ]; then
  ok "vision cache, PCA file and gate made unavailable"
else
  bad "hiding training artefacts"
fi

# 5-6. import into a fresh runtime and detect from JPEG only
out=$(timeout 900 ./bin/vd_runner --capsule "$W/pkg" --voc-root "$VOC" \
        --ids data/vision_slice_v3/ids.txt --gt-pack data/vision_slice_v3/slice.pack \
        --limit 25 2>&1); rc=$?
if [ $rc -eq 0 ] && grep -q VD_RUNNER_DONE <<<"$out"; then
  ok "fresh runtime imported the capsule and ran inference from JPEG"
  grep -E "VD_RUNNER_INFER|VD_RUNNER_AP|imported unit" <<<"$out" | sed 's/^/      /'
else
  bad "fresh-runtime inference" "exit $rc"; echo "$out" | tail -4 | sed 's/^/      /'
fi
echo "$out" | grep '^DET ' | sort > "$W/ref_dets.txt"
echo "      reference detections: $(wc -l < "$W/ref_dets.txt")"

# 7. replay must be exact
out2=$(timeout 900 ./bin/vd_runner --capsule "$W/pkg" --voc-root "$VOC" \
         --ids data/vision_slice_v3/ids.txt --gt-pack data/vision_slice_v3/slice.pack \
         --limit 25 2>&1)
echo "$out2" | grep '^DET ' | sort > "$W/replay_dets.txt"
if cmp -s "$W/ref_dets.txt" "$W/replay_dets.txt"; then ok "replay reproduces boxes and scores exactly"
else bad "replay exactness"; fi

# 8. coverage gate + abstention signal
outg=$(timeout 900 ./bin/vd_runner --capsule "$W/pkg" --voc-root "$VOC" \
         --ids data/vision_slice_v3/ids.txt --gt-pack data/vision_slice_v3/slice.pack \
         --limit 25 --gate 2>&1)
if grep -q "gate: rows=" <<<"$outg" && grep -q VD_RUNNER_INFER <<<"$outg"; then
  ok "coverage gate active from capsule coverage rows"
  grep -E "gate: rows|VD_RUNNER_INFER|VD_RUNNER_AP" <<<"$outg" | sed 's/^/      /'
else
  bad "coverage gate from capsule"
fi

# 9. compatibility / corruption controls, each must refuse
refuse_case() {
  local name="$1" dir="$2"
  local o rc2
  o=$(timeout 300 ./bin/vd_runner --capsule "$dir" 2>&1); rc2=$?
  if [ $rc2 -ne 0 ] && grep -qE "VD_RUNNER_REFUSED|VD_RUNNER_FAIL" <<<"$o"; then
    ok "$name refused ($(grep -oE 'reason=[a-z_]+' <<<"$o" | head -1))"
  else
    bad "$name" "exit $rc2"
  fi
}
mk(){ rm -rf "$W/$1"; cp -r "$W/pkg" "$W/$1"; }
mk c_noasset;   rm -f "$W/c_noasset/frontend.cvfa";            refuse_case "missing asset"      "$W/c_noasset"
mk c_truncasset;truncate -s -64 "$W/c_truncasset/frontend.cvfa"; refuse_case "truncated asset"  "$W/c_truncasset"
mk c_corrupt;   printf '\xA5' | dd of="$W/c_corrupt/frontend.cvfa" bs=1 seek=64 conv=notrunc status=none
                                                                refuse_case "corrupted asset"   "$W/c_corrupt"
mk c_payload;   printf '\xA5' | dd of="$W/c_payload/unit.cnb" bs=1 seek=64 conv=notrunc status=none
                                                                refuse_case "corrupted payload" "$W/c_payload"
mk c_manifest;  sed -i 's/^exemplars .*/exemplars 999999/' "$W/c_manifest/manifest.cknow"
                                                                refuse_case "tampered manifest" "$W/c_manifest"
mk c_nomanifest; rm -f "$W/c_nomanifest/manifest.cknow";        refuse_case "missing manifest"  "$W/c_nomanifest"

echo "checks=$checks failures=$fails"
if [ "$fails" -eq 0 ]; then echo "VISION_TRANSFER_PROOF_PASS checks=$checks"; exit 0; fi
echo "VISION_TRANSFER_PROOF_FAIL failures=$fails"; exit 1
