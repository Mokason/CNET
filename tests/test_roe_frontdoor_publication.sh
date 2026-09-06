#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d /tmp/cnet-frontdoor-publish-XXXXXX)
trap 'rc=$?; if ((rc)); then echo ROE_FRONTDOOR_PUBLICATION_RED; fi; rm -rf -- "$root"' EXIT
printf '%s\n' '{"pattern":"publication","pack":"pack_personal"}' > "$root/ROUTES.jsonl"
run() { env -i PATH="$PATH" bin/roe_front_door ask "$1" --accept --gold "$2" --promote-pack pack_personal --root "$root"; }
run 'publication alpha request' 'alpha answer' > "$root/a.log" & a=$!
run 'publication beta request' 'beta answer' > "$root/b.log" & b=$!
wait "$a"; wait "$b"
test "$(wc -l < "$root/pack_personal/catalog.jsonl")" -eq 2
run 'publication alpha request' 'alpha answer' > "$root/retry.log"
grep -q 'promote=yes pack=pack_personal .*reused=1' "$root/retry.log"
test "$(wc -l < "$root/pack_personal/catalog.jsonl")" -eq 2
if run 'publication alpha request' 'conflicting answer' > "$root/conflict.log"; then exit 1; fi
test "$(wc -l < "$root/pack_personal/catalog.jsonl")" -eq 2
echo ROE_FRONTDOOR_PUBLICATION_PASS
