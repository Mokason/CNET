#!/usr/bin/env bash
# Package prebuilt native tools with newly seeded, reviewed example packs.
set -euo pipefail
umask 077
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/packaging/CNET-Minimal/runtime_common.sh"
VERSION="${CNET_MINIMAL_VERSION:-$(git -C "$ROOT" rev-parse --short HEAD)}"
[[ $VERSION =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]] || cnet_fail 'unsafe_version'
OUT=$(cnet_new_path "${CNET_MINIMAL_OUT:-$ROOT/dist/CNET-Minimal-$VERSION}" output)
TAR=$(cnet_new_path "${CNET_MINIMAL_TAR:-$OUT.tar.gz}" archive)
[[ $TAR != "$OUT" && $TAR != "$OUT/"* && $OUT != "$TAR/"* ]] || cnet_fail 'overlapping_outputs'
BIN="$(realpath -m -- "${BIN_DIR:-$ROOT/bin}")"
cnet_require_bins
for name in domain_routes.tsv promote_blocklist.txt probe_shortcircuit.txt query_aliases.tsv utterance_phrases.tsv; do
  [[ -f $ROOT/config/$name && ! -L $ROOT/config/$name ]] || cnet_fail "missing_config name=$name"
done
mkdir -p -- "$(dirname "$OUT")" "$(dirname "$TAR")"
mkdir -- "$OUT"
printf 'incomplete\n' >"$OUT/BUILD_STATUS"
trap 'rc=$?; if [[ $rc -ne 0 ]]; then echo "PACKAGE_FAIL incomplete_path=$OUT" >&2; fi' EXIT
mkdir "$OUT/bin" "$OUT/data" "$OUT/config" "$OUT/scripts"
for name in roe_daily_packs_seed roe_front_door roe_domain_route roe_chain_think roe_evolve_tick roe_gold_put stream_ix_e2e_bench; do
  cp -- "$BIN/$name" "$OUT/bin/$name"
done
for name in domain_routes.tsv promote_blocklist.txt probe_shortcircuit.txt query_aliases.tsv utterance_phrases.tsv; do
  cp -- "$ROOT/config/$name" "$OUT/config/$name"
done
cp -- "$ROOT/packaging/CNET-Minimal/coverage_gold.tsv" "$OUT/config/coverage_gold.tsv"
cp -- "$ROOT/packaging/CNET-Minimal/runtime_common.sh" "$OUT/scripts/cnet_runtime_common.sh"
cp -- "$ROOT/packaging/CNET-Minimal/cnet-ask" "$OUT/bin/cnet-ask"
cp -- "$ROOT/packaging/CNET-Minimal/INSTALL.md" "$OUT/INSTALL.md"
cp -- "$ROOT/scripts/cnet_runtime_smoke.sh" "$ROOT/scripts/cnet_runtime_soak_gate.sh" "$OUT/scripts/"
chmod +x "$OUT/bin/"* "$OUT/scripts/"*.sh
# The seeder and gardener operate only under the exclusively created output.
BIN_DIR="$OUT/bin" CNET_HARVEST_LOG="$OUT/harvest.log" \
  bash "$ROOT/scripts/cert_coverage_harvest.sh" "$OUT/data/roe_daily_packs"
CNET_MINIMAL_ROOT="$OUT" CNET_SMOKE_LOG="$OUT/smoke.log" bash "$OUT/scripts/cnet_runtime_smoke.sh"
printf 'name=CNET-Minimal\nversion=%s\ngit=%s\nfixture_only=1\nlive_calls=0\nnever_self_cert=1\n' \
  "$VERSION" "$(git -C "$ROOT" rev-parse HEAD)" >"$OUT/VERSION"
printf 'complete\n' >"$OUT/BUILD_STATUS"
(
  cd "$OUT"
  find . -type f ! -name MANIFEST.sha256 -print0 | LC_ALL=C sort -z | xargs -0 sha256sum >MANIFEST.sha256
)
# Exclusive redirection prevents overwriting an archive created after preflight.
(
  set -o noclobber
  tar -C "$(dirname "$OUT")" -czf - "$(basename "$OUT")" >"$TAR"
)
echo "PACKAGE_OK path=$OUT tar=$TAR fixture_only=1"
