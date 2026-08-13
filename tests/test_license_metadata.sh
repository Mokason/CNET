#!/usr/bin/env bash
# Fail closed when CNET's Apache-2.0 metadata becomes inconsistent.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
failures=0
fail() {
    failures=$((failures + 1))
    printf 'LICENSE_METADATA_FAIL: %s\n' "$1" >&2
}

LICENSE=LICENSE
[[ -f "$LICENSE" ]] || fail "LICENSE missing"
license_text=$(cat "$LICENSE")
for fragment in \
    'Apache License' \
    'Version 2.0, January 2004' \
    'TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION' \
    '2. Grant of Copyright License.' \
    '3. Grant of Patent License.' \
    '4. Redistribution.' \
    '7. Disclaimer of Warranty.' \
    '8. Limitation of Liability.' \
    'END OF TERMS AND CONDITIONS'
do
    printf '%s' "$license_text" | grep -Fq "$fragment" ||
        fail "LICENSE omits canonical fragment: $fragment"
done

if command -v sha256sum >/dev/null 2>&1; then
    license_sha256=$(sha256sum "$LICENSE" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
    license_sha256=$(shasum -a 256 "$LICENSE" | awk '{print $1}')
else
    fail "sha256sum/shasum unavailable"
    license_sha256=
fi
pinned=cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30
[[ -z "$license_sha256" || "$license_sha256" == "$pinned" ]] ||
    fail "LICENSE is not the pinned canonical Apache-2.0 text: $license_sha256"

# README: ## License section identifies Apache-2.0 and links LICENSE
if ! awk '
  BEGIN {in_lic=0; ok=0}
  /^## License/ {in_lic=1; next}
  in_lic && /^## / {exit}
  in_lic && /Apache-2\.0/ && /\[LICENSE\]\(LICENSE\)/ {ok=1}
  END {exit ok?0:1}
' README.md; then
    fail "README does not identify Apache-2.0 and link LICENSE"
fi

policy=$(cat docs/RELEASE_POLICY.md)
printf '%s' "$policy" | grep -Fq 'Apache-2.0' || fail "release policy does not identify Apache-2.0"
printf '%s' "$policy" | grep -Fq 'remain private' || fail "release policy does not preserve private status"
if printf '%s' "$policy" | grep -Fq 'no public license is granted'; then
    fail "release policy still denies the configured license grant"
fi

expr=$(sed -n 's/.*<PackageLicenseExpression>\([^<]*\)<\/PackageLicenseExpression>.*/\1/p' \
    dotnet/Cce/Cce.csproj | head -n1)
[[ "$expr" == "Apache-2.0" ]] ||
    fail "Cce.csproj PackageLicenseExpression must be Apache-2.0"

grep -Eq '^license_metadata_test:[[:space:]]*' Makefile ||
    fail "Makefile has no license_metadata_test target"

release_body=$(awk '
  /^release_integrity:/ {grab=1; next}
  grab && /^[^[:space:]#]/ && !/^$/ {exit}
  grab {print}
' Makefile)
printf '%s' "$release_body" | grep -Fq 'bash tests/run_release_integrity.sh' ||
    fail "release_integrity does not delegate to the strict runner"

runner=tests/run_release_integrity.sh
[[ -f "$runner" ]] || fail "tests/run_release_integrity.sh missing"
grep -Fq 'run_make license_metadata_test' "$runner" ||
    fail "release_integrity does not run license_metadata_test"

[[ $failures -eq 0 ]] || exit 1
printf 'LICENSE_METADATA_PASS\n'
