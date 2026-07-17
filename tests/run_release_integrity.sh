#!/usr/bin/env bash
# Fail-closed single release authority for CNET.
# Every step executes at top level under strict Bash so one nonzero status
# stops the sequence, publishes the failure log, and can never emit PASS.
set -Eeuo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

LOG_DIR=logs
LOG_TMP="$LOG_DIR/release_integrity.log.tmp"
LOG_FINAL="$LOG_DIR/release_integrity.log"
MAKE_BIN=${CNET_RELEASE_MAKE:-make}
mkdir -p "$LOG_DIR"
rm -f "$LOG_TMP"

cleanup_release_generated() {
    if [[ -f docs/verified-today.generated.md ]]; then
        cp docs/verified-today.generated.md \
            logs/verified-today.release.md || true
        git restore -- docs/verified-today.generated.md || true
    fi
}

publish_failure() {
    local rc=$?
    trap - EXIT INT TERM
    cleanup_release_generated
    if [[ -f "$LOG_TMP" ]]; then
        cat "$LOG_TMP" >&2
        mv -f "$LOG_TMP" "$LOG_FINAL"
    fi
    exit "$rc"
}

run_make() {
    "$MAKE_BIN" --no-print-directory "$@"
}

trap publish_failure EXIT
trap 'exit 130' INT TERM

{
    if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
        echo "RELEASE_INTEGRITY_DIRTY_START" >&2
        exit 1
    fi

    run_make release_integrity_authority
    run_make license_metadata_test
    run_make real_model_control_plane_test
    run_make phase123_benchmark_test
    run_make phase5_integration_test
    run_make gguf_integrity
    run_make model_runtime_integrity
    run_make specialist_authority
    run_make persistence_integrity
    run_make mcp_protocol_survival
    run_make release_package
    run_make PORTABLE=1 ci_core
    run_make SKIP_RELEASE_PACKAGE=1 priority_acceptance

    cleanup_release_generated
    git diff --check
    git diff --cached --check
    if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
        echo "RELEASE_INTEGRITY_DIRTY_END" >&2
        exit 1
    fi
} >"$LOG_TMP" 2>&1

trap - EXIT INT TERM
printf '%s\n' 'CNET_RELEASE_INTEGRITY_PASS' >>"$LOG_TMP"
mv -f "$LOG_TMP" "$LOG_FINAL"
cat "$LOG_FINAL"
