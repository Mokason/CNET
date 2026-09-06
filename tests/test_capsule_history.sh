#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d /tmp/cnet-history-XXXXXX)
trap 'rc=$?; (( !rc )) || echo CAPSULE_HISTORY_RED; rm -rf -- "$root"' EXIT
printf '1 1\n' > "$root/old"
printf '0 0\n1 0\n' > "$root/shortcut"
printf 'alpha delta 0 0\n' > "$root/eval"
bin/cnet_capsule_core teach "$root/capsules" original_ab alpha beta 1 1 verified_tool "$root/old"
bin/cnet_capsule_core teach "$root/capsules" original_bd beta delta 1 1 verified_tool "$root/old"
if CNET_CAPSULE_EVAL_FILE="$root/eval" bin/cnet_capsule_core teach "$root/capsules" shortcut alpha delta 1 1 verified_tool "$root/shortcut"; then exit 1; fi
test ! -e "$root/capsules/shortcut"
bin/cnet_capsule_core ask "$root/capsules" 'capsule alpha delta 1' | grep -q 'verified=1 value=1 hops=2'
# Even a newly created indirect path must not contradict an existing direct one.
printf '0 0\n' > "$root/zero"
printf '0 1\n' > "$root/one"
bin/cnet_capsule_core teach "$root/new" ab alpha beta 1 1 verified_tool "$root/zero"
bin/cnet_capsule_core teach "$root/new" ad alpha delta 1 1 verified_tool "$root/zero"
if bin/cnet_capsule_core teach "$root/new" bd beta delta 1 1 verified_tool "$root/one"; then exit 1; fi
echo CAPSULE_HISTORY_PASS
