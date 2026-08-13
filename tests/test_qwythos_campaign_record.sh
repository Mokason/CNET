#!/usr/bin/env bash
# Verify the committed Qwythos campaign record without rewriting history.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
fail() {
    printf 'QWYTHOS_CAMPAIGN_RECORD_FAIL: %s\n' "$1" >&2
    exit 1
}

command -v jq >/dev/null 2>&1 || fail "jq required to read campaign manifest"
MANIFEST=qwythos_english_v1.cnb.manifest.json
[[ -f "$MANIFEST" ]] || fail "unreadable manifest: missing $MANIFEST"

failures=0
note() {
    failures=$((failures + 1))
    printf 'QWYTHOS_CAMPAIGN_RECORD_FAIL: %s\n' "$1" >&2
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

is_sha256() {
    [[ "$1" =~ ^[0-9a-f]{64}$ ]]
}

for field in provenance_version build_rev source_dirty executable_sha256 \
    model model_sha256 window_source window_sha256 \
    oracle_golden oracle_golden_sha256 base base_sha256
do
    jq -e --arg f "$field" 'has($f)' "$MANIFEST" >/dev/null ||
        note "manifest missing fields: $field"
done

[[ "$(jq -r '.provenance_version' "$MANIFEST")" == "1" ]] ||
    note "unsupported provenance_version"
[[ "$(jq -r '.source_dirty' "$MANIFEST")" == "1" ]] ||
    note "historical record must retain source_dirty=1 truth"

for field in executable_sha256 model_sha256 window_sha256 oracle_golden_sha256 base_sha256; do
    value=$(jq -r --arg f "$field" '.[$f] // empty' "$MANIFEST")
    is_sha256 "$value" || note "$field is not lowercase SHA-256"
done

for pair in window_source:window_sha256 oracle_golden:oracle_golden_sha256; do
    path_field=${pair%%:*}
    hash_field=${pair##*:}
    rel=$(jq -r --arg f "$path_field" '.[$f] // empty' "$MANIFEST")
    artifact="$ROOT/$rel"
    expect=$(jq -r --arg f "$hash_field" '.[$f] // empty' "$MANIFEST")
    if [[ ! -f "$artifact" ]]; then
        note "committed record artifact missing: $path_field"
    elif [[ "$(sha256_file "$artifact")" != "$expect" ]]; then
        note "$path_field content disagrees with $hash_field"
    fi
done

base_rel=$(jq -r '.base // empty' "$MANIFEST")
base="$ROOT/$base_rel"
base_present=0
if [[ -f "$base" ]]; then
    base_present=1
    expect=$(jq -r '.base_sha256' "$MANIFEST")
    [[ "$(sha256_file "$base")" == "$expect" ]] ||
        note "local base content disagrees with base_sha256"
fi

sha_file=qwythos_english_v1.cnb.sha256
if [[ ! -f "$sha_file" ]]; then
    note "base sha256 sidecar unreadable: missing"
else
    recorded=$(awk '{print $1; exit}' "$sha_file")
    expect=$(jq -r '.base_sha256' "$MANIFEST")
    [[ "$recorded" == "$expect" ]] || note "base SHA sidecar disagrees with manifest"
fi

if git ls-files --error-unmatch bin/flagship_run >/dev/null 2>&1; then
    note "historical flagship executable unexpectedly became tracked"
fi

body=$(awk '
  /^campaign_provenance:/ {grab=1; next}
  grab && /^[^[:space:]#]/ && !/^$/ {exit}
  grab {print}
' Makefile)
[[ -n "$body" ]] || note "Makefile has no campaign_provenance target"
# Accept .sh (post-rewire C/shell path).
if ! printf '%s' "$body" | grep -Eq 'test_qwythos_campaign_record\.(sh|py)'; then
    note "campaign_provenance does not run record-integrity verification"
fi
if printf '%s' "$body" | grep -Fq 'bin/flagship_run'; then
    note "campaign_provenance compares historical record to live executable"
fi

[[ $failures -eq 0 ]] || exit 1
printf 'QWYTHOS_CAMPAIGN_RECORD_PASS base_present=%d replayable=0 reason=dirty_source_and_historical_executable_not_retained\n' \
    "$base_present"
