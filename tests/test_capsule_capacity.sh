#!/usr/bin/env bash
set -euo pipefail
trap 'rc=$?; (( !rc )) || echo CAPSULE_CAPACITY_RED >&2' EXIT
report=$(mktemp /tmp/cnet-capacity-XXXXXX.jsonl)
CNET_SCALE_COUNTS='64 66' CNET_SCALE_REPEATS=1 bash scripts/cnet_capsule_scale_bench.sh > "$report"
jq -es 'length == 2 and all(.[]; .status == "pass" and .wrong_certified == 0) and
  .[-1].capsules == 66 and .[-1].correct == 3168 and .[-1].refused == 100' "$report" >/dev/null
root=$(jq -sr '.[-1].artifact_root' "$report")
capsules=$root/capsules
# Mix exhaustive and sampled imports. Order by directories so prefix and
# suffix of group 0 sit in different banks, with exhaustive coverage in bank 0.
printf '0 0\n1 1\n' > "$root/exhaustive.tsv"
env -u CNET_CAPSULE_EVAL_FILE bin/cnet_capsule_core teach "$root/candidate" exhaustive left right 1 1 \
  verified_tool "$root/exhaustive.tsv" > "$root/candidate.log"
bin/test_capsule_replay_scale "$capsules" "$root/candidate/exhaustive"
env -u CNET_CAPSULE_EVAL_FILE bin/cnet_capsule_core teach "$capsules" exhaustive left right 1 1 \
  verified_tool "$root/exhaustive.tsv" > "$root/exhaustive.log"
mv "$capsules/exhaustive" "$capsules/000_exhaustive"
mv "$capsules/edge_0" "$capsules/zzz_prefix"
bin/cnet_capsule_core ask "$capsules" 'capsule alpha000q000 gamma000q000 3' | rg -q 'verified=1 value=11 hops=2'
bin/cnet_capsule_core ask "$capsules" 'capsule left right 1' | rg -q 'verified=1 value=1'
for query in 'capsule alpha000q000 gamma000q000 32' 'capsule beta000q000 gamma000q000 32'; do
    if bin/cnet_capsule_core ask "$capsules" "$query"; then exit 1; fi
done
# A later-bank duplicate must still be refused by shared base identity.
cp -a "$capsules/edge_1" "$capsules/zzz_duplicate"
if bin/cnet_capsule_core ask "$capsules" 'capsule left right 1'; then exit 1; fi
mv "$capsules/zzz_duplicate" "$root/duplicate"
cp -a "$capsules/edge_1" "$capsules/zzz_corrupt"
printf 'corrupt\n' > "$capsules/zzz_corrupt/unit.cnb"
if bin/cnet_capsule_core ask "$capsules" 'capsule left right 1'; then exit 1; fi
mv "$capsules/zzz_corrupt" "$root/corrupt"
bin/cnet_capsule_core ask "$capsules" 'capsule alpha000q000 gamma000q000 3' | rg -q 'verified=1 value=11 hops=2'
echo CAPSULE_CAPACITY_PASS boundary=64_to_66 public_abi=unchanged
