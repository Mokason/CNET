#!/usr/bin/env bash
# Hermetic gate for campaign v2-fast prepare/export/estimate path.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
fail() { printf 'CAMPAIGN_V2_FAST_FAIL: %s\n' "$1" >&2; exit 1; }

ANALYZE=bin/margin_sweep_analyze
DRIVER=tools/campaign_v2_fast.sh
REAL_SWEEP=qwythos_english_v1.margin_sweep.tsv

tiny_sweep() {
    # $1=path $2=n_units $3=probes
    local path=$1 n_units=${2:-8} probes=${3:-32}
    {
        printf '# margin_sweep synthetic V=%d units=%d\n' "$probes" "$n_units"
        printf 'unit_idx unit_token w_idx w_token ordered_margin set_margin\n'
        local u w om sm
        for ((u = 0; u < n_units; u++)); do
            tok=$((1000 + u))
            for ((w = 0; w < probes; w++)); do
                if ((u < n_units / 2)); then
                    om=0.05
                    sm=0.10
                else
                    om=0.001
                    sm=0.001
                fi
                printf '%d %d %d %d %.6f %.6f\n' \
                    "$u" "$tok" "$w" "$((2000 + w))" "$om" "$sm"
            done
        done
    } >"$path"
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

sweep="$TMP/s.tsv"
allow="$TMP/minable.ids"
tiny_sweep "$sweep" 8 32

[[ -x "$ANALYZE" ]] || make -s margin_sweep_analyze || fail "bin/margin_sweep_analyze missing"
out=$("$ANALYZE" "$sweep" --eps 0.02 --semantics set --export-minable "$allow") ||
    fail "analyze export-minable failed: $out"
mapfile -t toks < <(grep -vE '^\s*(#|$)' "$allow" || true)
[[ ${#toks[@]} -eq 4 ]] || fail "half of synthetic units minable (got ${#toks[@]})"
expected=(1000 1001 1002 1003)
for i in 0 1 2 3; do
    [[ "${toks[$i]}" == "${expected[$i]}" ]] ||
        fail "minable tokens mismatch: got ${toks[*]}"
done

allow2="$TMP/minable2.ids"
prep=$(bash "$DRIVER" prepare --sweep "$sweep" --out-allowlist "$allow2" \
    --eps 0.02 --semantics set) || fail "driver prepare failed: $prep"
printf '%s' "$prep" | grep -Fq 'CAMPAIGN_V2_FAST_PREPARE_PASS' ||
    fail "prepare missing CAMPAIGN_V2_FAST_PREPARE_PASS"

est=$(bash "$DRIVER" estimate --allowlist "$allow2" --sec-per-unit 70 --workers 1) ||
    fail "driver estimate failed: $est"
printf '%s' "$est" | grep -Fq 'CAMPAIGN_V2_FAST_ESTIMATE' ||
    fail "estimate missing CAMPAIGN_V2_FAST_ESTIMATE"
printf '%s' "$est" | grep -Fq 'minable_units:     4' ||
    fail "estimate missing minable_units:     4"

if [[ -f "$REAL_SWEEP" ]]; then
    real_allow="$TMP/real.ids"
    "$ANALYZE" "$REAL_SWEEP" --eps 0.02 --semantics set \
        --export-minable "$real_allow" >/dev/null ||
        fail "real sweep export failed"
    n=$(grep -vE '^\s*(#|$)' "$real_allow" | wc -l | tr -d ' ')
    [[ "$n" -ge 250 ]] || fail "real sweep minable count $n < 250"
    [[ "$n" -le 256 ]] || fail "real sweep minable count $n > 256"
fi

printf 'CAMPAIGN_V2_FAST_PASS\n'
