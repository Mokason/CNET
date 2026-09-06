#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d /tmp/cnet-capsule-interfaces-XXXXXX)
trap 'rc=$?; if ((rc)); then echo CAPSULE_PUBLICATION_INTERFACES_RED; fi; rm -rf -- "$root"' EXIT
printf '0\t0\n1\t1\n' > "$root/rows.tsv"
failed=0
for direction in input output; do
    mkdir "$root/$direction"
    env -u CNET_CAPSULE_EVAL_FILE bin/cnet_capsule_core teach "$root/$direction" stable alpha beta 2 2 verified_tool "$root/rows.tsv"
    if [[ $direction == input ]]; then args=(conflict alpha gamma 3 2); else args=(conflict gamma beta 2 3); fi
    if env -u CNET_CAPSULE_EVAL_FILE bin/cnet_capsule_core teach "$root/$direction" "${args[@]}" verified_tool "$root/rows.tsv"; then
        echo "CAPSULE_PUBLICATION_INTERFACES_RED incompatible_${direction}_published"
        failed=1
    else
        test ! -e "$root/$direction/conflict"
        bin/cnet_capsule_core ask "$root/$direction" 'capsule alpha beta 1' | grep -q 'verified=1 value=1'
    fi
done
(( !failed ))
echo CAPSULE_PUBLICATION_INTERFACES_PASS
