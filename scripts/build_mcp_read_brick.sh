#!/usr/bin/env bash
# Owner-run capsule publication; runtime never executes this script.
# Labels are the fixed read dispatch ABI in cnet_mcp_read_brick.c, not web data.
set -euo pipefail
if [[ $# != 1 || "$1" != /* || "$1" == / ]]; then
    echo "usage: bash scripts/build_mcp_read_brick.sh ABSOLUTE_CAPSULE_ROOT" >&2
    exit 2
fi
read_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
read_core="$read_repo/bin/cnet_capsule_core"
if [[ ! -x "$read_core" ]]; then
    echo "MCP_READ_BRICK_REFUSED build bin/cnet_capsule_core first (make capsule_core)" >&2
    exit 1
fi
export CNET_CAPSULE_EVAL_FILE="$read_repo/config/mcp_read_dispatch_eval.tsv"
"$read_core" teach "$1" mcp_read_dispatch_v1 mcp_read_action mcp_read_tool \
    2 2 verified_tool "$read_repo/config/mcp_read_dispatch.tsv"
for read_action in 0 3; do
    if read_receipt=$("$read_core" ask "$1" "capsule mcp_read_action mcp_read_tool $read_action"); then
        echo "MCP_READ_BRICK_REFUSED out_of_domain_action=$read_action" >&2
        exit 1
    else
        read_rc=$?
        if [[ $read_rc != 3 || "$read_receipt" != "ABSTAIN verified=0 reason=no_covered_certified_plan" ]]; then
            echo "MCP_READ_BRICK_REFUSED invalid_coverage_check action=$read_action exit=$read_rc" >&2
            exit 1
        fi
    fi
    echo "$read_receipt"
done
echo "MCP_READ_BRICK_PASS certified_dispatch_rows=2 covered_actions=1,2 uncovered_actions=0,3 web_claims=WITHHELD"
