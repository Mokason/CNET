#!/usr/bin/env bash
# Native workflow regression: only newly allocated private artifacts.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d /tmp/cnet-native-workflows.XXXXXXXX)"
trap 'echo "NATIVE_WORKFLOWS_ARTIFACTS path=$WORK"' EXIT
export BIN_DIR="${BIN_DIR:-$ROOT/bin}"
export ROE_LIVE=0 ROE_LLM=0 ROE_LOOKUP=0 ROE_NO_THOUGHT=1

if ! CNET_HARVEST_LOG="$WORK/harvest.log" bash "$ROOT/scripts/cert_coverage_harvest.sh" "$WORK/packs" >"$WORK/harvest.out" 2>&1; then
  echo "NATIVE_WORKFLOWS_RED native_harvest_failed log=$WORK/harvest.out" >&2
  exit 1
fi
grep -q '^CERT_COVERAGE_HARVEST_PASS ' "$WORK/harvest.out"
[[ -f $WORK/packs/pack_personal/catalog.jsonl ]]
[[ ${1:-} != --harvest-only ]] || exit 0

# A packager must not run phony build gates that seed default runtime paths.
mkdir "$WORK/failbin"
cp "$ROOT/tests/fixtures/native_workflows/fail-make.sh" "$WORK/failbin/make"
chmod +x "$WORK/failbin/make"
export PATH="$WORK/failbin:$PATH"
trap 'rc=$?; if [[ $rc -ne 0 ]]; then echo "NATIVE_WORKFLOWS_RED package_boundary log_root=$WORK"; fi; echo "NATIVE_WORKFLOWS_ARTIFACTS path=$WORK"' EXIT

refuse_package() {
  local name="$1" out="$2" archive="$3" marker="$4"
  if CNET_MINIMAL_OUT="$out" CNET_MINIMAL_TAR="$archive" \
      bash "$ROOT/scripts/package_cnet_minimal.sh" >"$WORK/$name.log" 2>&1; then
    echo "NATIVE_WORKFLOWS_FAIL package accepted $name" >&2; exit 1
  fi
  grep -q "$marker" "$WORK/$name.log"
  ! grep -q '^PACKAGE_OK ' "$WORK/$name.log"
}
mkdir "$WORK/existing"
printf 'preserve\n' >"$WORK/existing/sentinel"
refuse_package existing "$WORK/existing" "$WORK/existing.tar.gz" output_exists
[[ $(<"$WORK/existing/sentinel") == preserve ]]
refuse_package root / "$WORK/root.tar.gz" output_exists
refuse_package workspace "$ROOT" "$WORK/workspace.tar.gz" output_exists
refuse_package home "$HOME" "$WORK/home.tar.gz" output_exists
refuse_package alias "$WORK/unused/../existing" "$WORK/alias.tar.gz" output_exists
ln -s "$WORK" "$WORK/link"
refuse_package symlink "$WORK/link/unsafe" "$WORK/symlink.tar.gz" symlink_path
refuse_package archive_symlink "$WORK/archive-link" "$WORK/link/unsafe.tar.gz" symlink_path
printf 'archive sentinel\n' >"$WORK/existing.tar.gz"
refuse_package archive_exists "$WORK/archive-existing" "$WORK/existing.tar.gz" archive_exists
[[ $(<"$WORK/existing.tar.gz") == 'archive sentinel' ]]
refuse_package overlap "$WORK/overlap" "$WORK/overlap/archive.tar.gz" overlapping_outputs
refuse_package quote "$WORK/quote'path" "$WORK/quote.tar.gz" unsafe_path
mkdir "$WORK/empty-bin"
BIN_DIR="$WORK/empty-bin" refuse_package missing_binary "$WORK/missing-bin" "$WORK/missing-bin.tar.gz" missing_binary
[[ ! -e $WORK/missing-bin ]]

CNET_MINIMAL_OUT="$WORK/package" CNET_MINIMAL_TAR="$WORK/package.tar.gz" \
  bash "$ROOT/scripts/package_cnet_minimal.sh" >"$WORK/package.log" 2>&1
grep -q '^PACKAGE_OK ' "$WORK/package.log"
mkdir "$WORK/extracted" "$WORK/outside"
tar -xzf "$WORK/package.tar.gz" -C "$WORK/extracted"
PKG="$WORK/extracted/package"
before=$(cd "$PKG" && sha256sum -c MANIFEST.sha256)
(cd "$WORK/outside" && "$PKG/bin/cnet-ask" 'who are you') >"$WORK/ask.log"
grep -q '^source=LOCAL ' "$WORK/ask.log"
(cd "$WORK/outside" && CNET_MINIMAL_ROOT="$PKG" bash "$PKG/scripts/cnet_runtime_smoke.sh") >"$WORK/smoke.log" 2>&1
grep -q '^CNET_RUNTIME_SMOKE_PASS ' "$WORK/smoke.log"
(cd "$WORK/outside" && CNET_MINIMAL_ROOT="$PKG" bash "$PKG/scripts/cnet_runtime_soak_gate.sh") >"$WORK/soak.log" 2>&1
grep -q '^CNET_RUNTIME_SOAK_GATE_PASS ' "$WORK/soak.log"
after=$(cd "$PKG" && sha256sum -c MANIFEST.sha256)
[[ $before == "$after" ]]

for fault in ood-front-door domain-route; do
  if [[ $fault == ood-front-door ]]; then
    target=roe_front_door
    expected=ood_query_failed
  else
    target=roe_domain_route
    expected=command_failed
  fi
  mv "$PKG/bin/$target" "$PKG/bin/$target.saved"
  cp "$ROOT/tests/fixtures/native_workflows/fail-$fault.sh" "$PKG/bin/$target"
  chmod +x "$PKG/bin/$target"
  if CNET_MINIMAL_ROOT="$PKG" bash "$PKG/scripts/cnet_runtime_smoke.sh" >"$WORK/$fault.log" 2>&1; then
    echo "NATIVE_WORKFLOWS_FAIL smoke accepted failed $fault" >&2; exit 1
  fi
  grep -q "$expected" "$WORK/$fault.log"
  mv -f "$PKG/bin/$target.saved" "$PKG/bin/$target"
done

mv "$PKG/bin/roe_evolve_tick" "$PKG/bin/roe_evolve_tick.saved"
if CNET_MINIMAL_ROOT="$PKG" bash "$PKG/scripts/cnet_runtime_smoke.sh" >"$WORK/missing-gardener.log" 2>&1; then
  echo 'NATIVE_WORKFLOWS_FAIL smoke skipped missing gardener' >&2; exit 1
fi
grep -q 'missing_binary.*roe_evolve_tick' "$WORK/missing-gardener.log"
mv "$PKG/bin/roe_evolve_tick.saved" "$PKG/bin/roe_evolve_tick"
mv "$PKG/data/roe_daily_packs" "$PKG/data/packs.saved"
if CNET_MINIMAL_ROOT="$PKG" bash "$PKG/scripts/cnet_runtime_smoke.sh" >"$WORK/missing-packs.log" 2>&1; then
  echo 'NATIVE_WORKFLOWS_FAIL smoke skipped missing packs' >&2; exit 1
fi
grep -q 'missing_packs' "$WORK/missing-packs.log"
mv "$PKG/data/packs.saved" "$PKG/data/roe_daily_packs"
if CNET_HARVEST_LOG="$WORK/harvest-existing.log" bash "$ROOT/scripts/cert_coverage_harvest.sh" "$WORK/existing" >"$WORK/harvest-existing.out" 2>&1; then
  echo 'NATIVE_WORKFLOWS_FAIL harvest accepted existing packs' >&2; exit 1
fi
[[ $(<"$WORK/existing/sentinel") == preserve ]]
grep -q 'output_exists' "$WORK/harvest-existing.out"
echo 'NATIVE_WORKFLOWS_PASS outside_repo=1 fixture_only=1 live_calls=0'
