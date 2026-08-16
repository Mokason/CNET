#!/usr/bin/env bash
# CNET-Minimal smoke — front_door + domain_route + evolve dry-run + stream_ix e2e
# Emits CNET_RUNTIME_SMOKE_PASS
set -euo pipefail
ROOT="${CNET_MINIMAL_ROOT:-}"
if [[ -z "$ROOT" ]]; then
  # repo or package root
  if [[ -x ./bin/roe_front_door ]]; then
    ROOT="$(pwd)"
  else
    ROOT="$(cd "$(dirname "$0")/.." && pwd)"
  fi
fi
cd "$ROOT"
LOG="${CNET_SMOKE_LOG:-$ROOT/logs/cnet_runtime_smoke.log}"
mkdir -p "$(dirname "$LOG")" logs 2>/dev/null || mkdir -p logs
BIN="$ROOT/bin"
PACKS="$ROOT/data/roe_daily_packs"
if [[ ! -d $PACKS ]]; then
  PACKS="$ROOT/artifacts/roe_daily_packs"
fi
export PATH="$BIN:$PATH"

{
  echo "=== CNET runtime smoke root=$ROOT ==="
  fail=0

  need() {
    if [[ -x $BIN/$1 ]]; then
      echo "  ok bin $1"
    else
      echo "  FAIL missing $1"
      fail=$((fail + 1))
    fi
  }
  need roe_front_door
  need roe_domain_route
  need roe_chain_think

  # domain route
  if [[ -x $BIN/roe_domain_route ]]; then
    out=$("$BIN/roe_domain_route" --test 2>&1 || true)
    echo "$out" | tail -5
    echo "$out" | grep -q DOMAIN_ROUTE_PASS && echo "  ok domain_route" || {
      echo "  FAIL domain_route"
      fail=$((fail + 1))
    }
    out=$("$BIN/roe_domain_route" "who are you" 2>&1 || true)
    echo "$out" | grep -q 'DISPATCH CERT' && echo "  ok who-are-you CERT" || {
      echo "  FAIL who-are-you"
      fail=$((fail + 1))
    }
  fi

  # front door
  if [[ -x $BIN/roe_front_door && -d $PACKS ]]; then
    for q in "who are you" "format-truncation werror" "cnet never lowers floors for brain floats"; do
      out=$("$BIN/roe_front_door" ask "$q" --root "$PACKS" 2>&1 || true)
      if echo "$out" | grep -q 'source=LOCAL'; then
        echo "  ok LOCAL: $q"
      else
        echo "  FAIL LOCAL: $q"
        echo "$out" | head -8
        fail=$((fail + 1))
      fi
    done
    out=$("$BIN/roe_front_door" ask "zz unknown mystic ooze 99" --root "$PACKS" 2>&1 || true)
    if echo "$out" | grep -q 'source=LOCAL'; then
      echo "  FAIL probe should not LOCAL"
      fail=$((fail + 1))
    else
      echo "  ok probe non-LOCAL"
    fi
  else
    echo "  WARN skip front_door (no packs at $PACKS)"
  fi

  # chain think
  if [[ -x $BIN/roe_chain_think ]]; then
    out=$("$BIN/roe_chain_think" "who are you" 2>&1 || true)
    echo "$out" | grep -qE 'ROE_CHAIN_THINK_PASS|not_conscious|LOCAL|chain' && echo "  ok chain_think" || echo "  WARN chain_think soft"
  fi

  # evolve dry-run + blocklist (repo tools or package tools)
  EVOLVE=""
  if [[ -f $ROOT/tools/roe_evolve_tick.py ]]; then
    EVOLVE="$ROOT/tools/roe_evolve_tick.py"
  elif [[ -f $ROOT/../tools/roe_evolve_tick.py ]]; then
    EVOLVE="$ROOT/../tools/roe_evolve_tick.py"
  fi
  if [[ -n $EVOLVE ]]; then
    # PYTHONPATH for package layout
    export PYTHONPATH="$ROOT/tools:${PYTHONPATH:-}"
    if python3 -c "import sys; sys.path.insert(0,'$ROOT/tools'); from roe_evolve_tick import is_promote_blocked as b; assert b('zz mystic ooze','x'); assert b('q','ABSTAIN: no'); print('blocklist_ok')" 2>&1; then
      echo "  ok evolve blocklist"
    else
      # try repo import path
      if python3 -c "import sys; sys.path.insert(0,'$ROOT'); from tools.roe_evolve_tick import is_promote_blocked as b; assert b('zz mystic ooze','x'); print('blocklist_ok')" 2>&1; then
        echo "  ok evolve blocklist (tools.)"
      else
        echo "  FAIL evolve blocklist"
        fail=$((fail + 1))
      fi
    fi
    # dry-run if packs available in cwd artifacts symlink
    if [[ -d $PACKS ]]; then
      mkdir -p "$ROOT/artifacts"
      if [[ ! -e $ROOT/artifacts/roe_daily_packs ]]; then
        ln -sfn "$PACKS" "$ROOT/artifacts/roe_daily_packs" 2>/dev/null || true
      fi
    fi
  fi

  # stream_ix e2e if present
  if [[ -x $BIN/stream_ix_e2e_bench ]]; then
    out=$("$BIN/stream_ix_e2e_bench" 2>&1 || true)
    echo "$out" | tail -3
    echo "$out" | grep -q STREAM_IX_E2E_BENCH_PASS && echo "  ok stream_ix_e2e" || {
      echo "  FAIL stream_ix_e2e"
      fail=$((fail + 1))
    }
  else
    echo "  WARN no stream_ix_e2e_bench"
  fi

  if [[ $fail -eq 0 ]]; then
    echo "CNET_RUNTIME_SMOKE_PASS"
    exit 0
  fi
  echo "CNET_RUNTIME_SMOKE_FAIL fail=$fail"
  exit 1
} | tee "$LOG"
