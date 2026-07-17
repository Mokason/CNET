#!/usr/bin/env bash
# Post-seal serve proof: hermetic gate + optional live CNB Tier A sample.
#
# Usage:
#   scripts/personal_ai_serve_proof.sh              # hermetic C gate only
#   scripts/personal_ai_serve_proof.sh live         # live base sample
#   scripts/personal_ai_serve_proof.sh all          # hermetic + live
#   BASE_PATH=/path/to/soul.cnb scripts/personal_ai_serve_proof.sh live
set -euo pipefail
# shellcheck source=personal_ai_common.sh
. "$(cd "$(dirname "$0")" && pwd)/personal_ai_common.sh"
cnet_load_personal_env
BASE="$(cnet_default_base)"
MODE="${1:-hermetic}"
MAX="${SERVE_PROOF_MAX:-24}"
JOBS="$(cnet_nproc)"

info() { echo "serve_proof: $*"; }
die() { echo "serve_proof: $*" >&2; exit 1; }

mkdir -p "$REPO/logs"

hermetic() {
  info "building + running hermetic post-seal serve gate"
  make -C "$REPO" post_seal_serve -j"$JOBS"
  grep -E "POST_SEAL_SERVE_PASS" "$REPO/logs/post_seal_serve.log"
}

live() {
  info "live serve proof on $BASE"
  [ -f "$BASE" ] || die "base missing: $BASE"
  make -C "$REPO" serve_proof_cli -j"$JOBS"
  [ -x "$REPO/bin/serve_proof" ] || die "bin/serve_proof missing"
  # Capture exit of serve_proof, not tee
  set +e
  "$REPO/bin/serve_proof" "$BASE" --max "$MAX" | tee "$REPO/logs/serve_proof_live.log"
  local rc=${PIPESTATUS[0]}
  set -e
  [ "$rc" -eq 0 ] || die "serve_proof exited $rc"
  grep -q "SERVE_PROOF_PASS" "$REPO/logs/serve_proof_live.log" || die "SERVE_PROOF_PASS missing"
  grep "SERVE_PROOF_PASS" "$REPO/logs/serve_proof_live.log"
}

case "$MODE" in
  hermetic|"") hermetic ;;
  live) live ;;
  all)
    hermetic
    if [ -f "$BASE" ]; then
      live || info "WARN: live sample failed (hermetic still passed)"
    else
      info "WARN: no live base at $BASE — skipped live"
    fi
    ;;
  *)
    echo "usage: $0 [hermetic|live|all]" >&2
    exit 2
    ;;
esac

echo "PERSONAL_AI_SERVE_PROOF_OK mode=$MODE"
