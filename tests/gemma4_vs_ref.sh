#!/usr/bin/env bash
# Thin orchestrator: tokenize via llama-server, greedy-generate on CNET driver,
# gate on ARGMAX IDENTITY. Requires bin/gemma4_vs_ref built + matching server.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

SERVER="${GVR_SERVER:-http://127.0.0.1:8083}"
MODEL="${GVR_MODEL:-/home/marble/AI/Models/gemma4-v2-Q4_K_M.gguf}"
STEPS="${GVR_STEPS:-16}"
BIN=./bin/gemma4_vs_ref

while [[ $# -gt 0 ]]; do
    case "$1" in
        --server) SERVER=$2; shift 2 ;;
        --steps) STEPS=$2; shift 2 ;;
        --model) MODEL=$2; shift 2 ;;
        *) printf 'unknown arg: %s\n' "$1" >&2; exit 2 ;;
    esac
done

[[ -x "$BIN" ]] || {
    printf 'GATE FAILED: %s not built (make gemma4_vs_ref_build)\n' "$BIN" >&2
    exit 1
}
command -v curl >/dev/null 2>&1 || {
    printf 'GATE FAILED: curl required\n' >&2
    exit 1
}
command -v jq >/dev/null 2>&1 || {
    printf 'GATE FAILED: jq required\n' >&2
    exit 1
}

post_json() {
    local path=$1 body=$2
    curl -sS --max-time 300 -H 'Content-Type: application/json' \
        -d "$body" "${SERVER}${path}"
}

PROMPTS=(
    'The capital of France is'
    '2 + 2 ='
    'The chemical symbol for gold is'
    'Roses are red, violets are'
    'def fibonacci(n):'
    'The Eiffel Tower is located in the city of'
)

if [[ -n "${GVR_ONLY:-}" ]]; then
    filtered=()
    IFS=',' read -r -a keys <<<"$GVR_ONLY"
    for p in "${PROMPTS[@]}"; do
        pl=${p,,}
        for k in "${keys[@]}"; do
            kl=${k,,}
            kl=${kl// /}
            [[ -z "$kl" ]] && continue
            if [[ "$pl" == *"$kl"* ]]; then
                filtered+=("$p")
                break
            fi
        done
    done
    PROMPTS=("${filtered[@]}")
fi

fails=0
total=${#PROMPTS[@]}
for p in "${PROMPTS[@]}"; do
    tok_json=$(post_json /tokenize "$(jq -nc --arg c "$p" '{content:$c, add_special:true}')")
    ids_csv=$(printf '%s' "$tok_json" | jq -r '.tokens | map(tostring) | join(",")')
    [[ -n "$ids_csv" && "$ids_csv" != null ]] || {
        printf 'EMPTY        steps=  0  %q\n' "$p"
        fails=$((fails + 1))
        continue
    }
    ids_json=$(printf '%s' "$tok_json" | jq -c '.tokens')

    ref_json=$(post_json /completion "$(jq -nc \
        --argjson ids "$ids_json" --argjson steps "$STEPS" \
        '{prompt:$ids, n_predict:$steps, temperature:0.0, n_probs:2}')")
    mapfile -t ref < <(printf '%s' "$ref_json" |
        jq -r '.completion_probabilities[:'"$STEPS"'][].id')

    cnet_out=$("$BIN" "$MODEL" "$ids_csv" "$STEPS" 3) || {
        printf 'DIVERGES@?   steps=  0  %q (cnet driver failed)\n' "$p"
        fails=$((fails + 1))
        continue
    }
    mapfile -t got < <(printf '%s\n' "$cnet_out" |
        sed -n 's/^STEP[[:space:]]*[0-9][0-9]* next=\([0-9][0-9]*\).*/\1/p' |
        head -n "$STEPS")

    n=${#ref[@]}
    (( ${#got[@]} < n )) && n=${#got[@]}
    div=
    for ((i = 0; i < n; i++)); do
        if [[ "${ref[$i]}" != "${got[$i]}" ]]; then
            div=$i
            break
        fi
    done
    if [[ -z "$div" && $n -gt 0 ]]; then
        printf 'IDENTICAL    steps=%3d  %q\n' "$n" "$p"
    elif [[ -n "$div" ]]; then
        printf 'DIVERGES@%-3s steps=%3d  %q\n' "$div" "$n" "$p"
        fails=$((fails + 1))
    else
        printf 'EMPTY        steps=%3d  %q\n' "$n" "$p"
        fails=$((fails + 1))
    fi
done

ok=$((total - fails))
if [[ $fails -eq 0 ]]; then
    printf '\nGATE PASSED: %d/%d prompts argmax-identical x %s steps\n' \
        "$ok" "$total" "$STEPS"
    exit 0
fi
printf '\nGATE FAILED: %d/%d prompts argmax-identical x %s steps\n' \
    "$ok" "$total" "$STEPS"
exit 1
