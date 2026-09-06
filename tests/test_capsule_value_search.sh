#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d /tmp/cnet-value-search-XXXXXX)
trap 'rc=$?; (( !rc )) || echo CAPSULE_VALUE_SEARCH_RED; rm -rf -- "$root"' EXIT
mkdir "$root/capsules"
printf '0 0\n1 2\n' > "$root/ab"
printf '0 0\n1 1\n' > "$root/ac"
printf '0 0\n1 3\n' > "$root/cb"
printf '3 1\n' > "$root/bd"
# Independent portable units can be imported without a CLI publication history.
teach() {
    local unit=$1; shift
    env -u CNET_CAPSULE_EVAL_FILE bin/cnet_capsule_core teach "$root/$unit" "$unit" "$@"
    cp -a "$root/$unit/$unit" "$root/capsules/$unit"
}
teach short_ab alpha beta 1 2 verified_tool "$root/ab"
teach long_ac alpha gamma 1 1 verified_tool "$root/ac"
teach long_cb gamma beta 1 2 verified_tool "$root/cb"
teach suffix_bd beta delta 2 1 verified_tool "$root/bd"
bin/cnet_capsule_core ask "$root/capsules" 'capsule alpha delta 1' | grep -q 'verified=1 value=1 hops=3'
if bin/cnet_capsule_core ask "$root/capsules" 'capsule alpha delta 0'; then exit 1; fi
# A nonzero certified identity cycle must not be pruned as the initial state.
mkdir "$root/cycle"
bin/cnet_capsule_core teach "$root/cycle" forward alpha gamma 1 1 verified_tool "$root/ac"
bin/cnet_capsule_core teach "$root/cycle" backward gamma alpha 1 1 verified_tool "$root/ac"
bin/cnet_capsule_core ask "$root/cycle" 'capsule alpha alpha 1' | grep -q 'verified=1 value=1 hops=2'
echo CAPSULE_VALUE_SEARCH_PASS
