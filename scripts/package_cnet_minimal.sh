#!/usr/bin/env bash
# Build CNET-Minimal distribution: bins + data packs + config + smoke.
# Output: dist/CNET-Minimal-<version>/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VERSION="${CNET_MINIMAL_VERSION:-$(git rev-parse --short HEAD 2>/dev/null || date +%Y%m%d)}"
NAME="CNET-Minimal-${VERSION}"
OUT="${CNET_MINIMAL_OUT:-$ROOT/dist/$NAME}"
BIN_DIR="${BIN_DIR:-bin}"

echo "=== package CNET-Minimal version=$VERSION → $OUT ==="

# Build core bins (fail closed if critical missing)
make -s domain_route roe_chain_think 2>/dev/null || true
make roe_front_door 2>&1 | tail -15 || make roe_front_door
make -s stream_ix_e2e_bench 2>&1 | tail -5 || true
# evolve gate binary (optional)
if [[ -f tools/roe_evolve_tick_gate.c ]]; then
  make -s roe_evolve_tick 2>&1 | tail -8 || true
fi
# seed packs into artifacts (source of truth)
python3 tools/roe_daily_packs_seed.py
# personal queries for gate if present
if [[ -d artifacts/roe_daily_packs/pack_personal/skills ]]; then
  python3 - <<'PY'
from pathlib import Path
root = Path("artifacts/roe_daily_packs/pack_personal")
qs = []
for sk in sorted((root/"skills").glob("*/SKILL.roe")):
    for ln in sk.read_text().splitlines():
        if ln.startswith("pattern "):
            qs.append(ln[8:].strip())
if qs:
    qs.append("quantum personal junk zz_ood")
    (root/"queries_train.txt").write_text("\n".join(qs)+"\n")
PY
fi

rm -rf "$OUT"
mkdir -p "$OUT"/{bin,data/roe_daily_packs,config,scripts,docs}

# Binaries
copy_bin() {
  local b="$1"
  if [[ -x $BIN_DIR/$b ]]; then
    cp -a "$BIN_DIR/$b" "$OUT/bin/"
    echo "  bin $b"
  else
    echo "  WARN missing bin $b" >&2
  fi
}
copy_bin roe_front_door
copy_bin roe_domain_route
copy_bin roe_chain_think
copy_bin roe_daily_packs_gate
copy_bin roe_evolve_tick_gate
copy_bin stream_ix_e2e_bench
copy_bin stream_attend_bench

# Helper scripts (Python evolve — runtime dep: python3)
mkdir -p "$OUT/tools"
cp -a tools/roe_evolve_tick.py "$OUT/tools/" 2>/dev/null || true
cp -a tools/roe_reviewer.py "$OUT/tools/" 2>/dev/null || true
cp -a tools/roe_gold_curriculum_harvest.py "$OUT/tools/" 2>/dev/null || true
cp -a tools/roe_daily_packs_seed.py "$OUT/tools/" 2>/dev/null || true

# Data packs
if [[ -d artifacts/roe_daily_packs ]]; then
  if command -v rsync >/dev/null 2>&1; then
    rsync -a \
      --exclude 'quarantine_bad_auto' \
      --exclude '*.log' \
      artifacts/roe_daily_packs/ "$OUT/data/roe_daily_packs/"
  else
    cp -a artifacts/roe_daily_packs/. "$OUT/data/roe_daily_packs/"
    rm -rf "$OUT/data/roe_daily_packs/quarantine_bad_auto" 2>/dev/null || true
  fi
  : > "$OUT/data/roe_daily_packs/miss_log.jsonl" || true
  echo "  data packs seeded"
fi

# Config (secret-free)
cp -a config/domain_routes.tsv "$OUT/config/"
cp -a config/promote_blocklist.txt "$OUT/config/"
cp -a config/autonomy_charter.yaml "$OUT/config/" 2>/dev/null || true
# templates
cat > "$OUT/config/teacher.env.example" <<'EOF'
# Copy to teacher.env and fill — never commit secrets.
# ROE_TEACHER_BASE_URL=
# ROE_TEACHER_MODEL=deepseek-v4-flash:cloud
# ROE_LLM_THINK=0
EOF
cat > "$OUT/config/reviewer.env.example" <<'EOF'
# Copy to reviewer.env — separate from teacher.
# ROE_REVIEWER_BASE_URL=
# ROE_REVIEWER_MODEL=
# ROE_EVOLVE_REVIEWER=1
EOF

# Smoke + soak
cp -a scripts/cnet_runtime_smoke.sh "$OUT/scripts/" 2>/dev/null || true
cp -a scripts/cnet_runtime_soak_gate.sh "$OUT/scripts/" 2>/dev/null || true
chmod +x "$OUT/scripts/"*.sh 2>/dev/null || true

# Wrapper: run front door against package data root
cat > "$OUT/bin/cnet-ask" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
export CNET_MINIMAL_ROOT="$HERE"
Q="${1:-}"
if [[ -z "$Q" ]]; then
  echo "usage: cnet-ask \"query\"" >&2
  exit 2
fi
exec "$HERE/bin/roe_front_door" ask "$Q" --root "$HERE/data/roe_daily_packs"
EOF
chmod +x "$OUT/bin/cnet-ask"

# VERSION + README
cat > "$OUT/VERSION" <<EOF
name=CNET-Minimal
version=$VERSION
git=$(git rev-parse HEAD 2>/dev/null || echo unknown)
built_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
never_self_cert=1
EOF

cp -a docs/DOMAIN_ROUTE.md "$OUT/docs/" 2>/dev/null || true
cp -a docs/GOLD_CURRICULUM_HARVEST.md "$OUT/docs/" 2>/dev/null || true
cp -a docs/STREAM_INDEX_ATTEND.md "$OUT/docs/" 2>/dev/null || true
cp -a docs/WEIGHT_EPOCH.md "$OUT/docs/" 2>/dev/null || true

# INSTALL written by companion file in repo — copy if present
if [[ -f packaging/CNET-Minimal/INSTALL.md ]]; then
  cp -a packaging/CNET-Minimal/INSTALL.md "$OUT/INSTALL.md"
else
  cp -a "$ROOT/packaging/CNET-Minimal/INSTALL.md" "$OUT/INSTALL.md" 2>/dev/null || true
fi

# tarball
mkdir -p "$ROOT/dist"
TAR="$ROOT/dist/${NAME}.tar.gz"
tar -C "$(dirname "$OUT")" -czf "$TAR" "$(basename "$OUT")"
echo "PACKAGE_OK path=$OUT tar=$TAR"
echo "$OUT" > "$ROOT/dist/CNET-Minimal-latest.path"
ls -lh "$TAR"
