#!/usr/bin/env bash
# RSI live proof — recursive self-improve under CNET law (no self-CERT monobrain)
set -euo pipefail
ROOT="${CNET_ROOT:-$HOME/AI/CNET}"
BR="${CNET_CORE_BUS_BRICKS_DIR:-$HOME/.local/share/cnet-bricks}"
PEER="${CNET_PEER_BIN:-$ROOT/bin/cnet_peer}"
LOG="${1:-$ROOT/logs/rsi_live_proof.log}"
mkdir -p "$(dirname "$LOG")" "$BR"
exec > >(tee "$LOG") 2>&1

echo "=== RSI_LIVE_PROOF $(date -Is) ==="
fail=0
pass() { echo "PASS $1"; }
failm() { echo "FAIL $1"; fail=$((fail+1)); }

# 0) bins
[[ -x "$PEER" ]] || failm "cnet_peer missing"
[[ -x "$ROOT/bin/cnet_core_evolve" ]] || failm "cnet_core_evolve missing"
[[ -x "$ROOT/bin/cnetd" ]] || failm "cnetd missing"
systemctl --user is-active cnetd.service >/dev/null && pass "cnetd_active" || failm "cnetd_active"

# 1) chain with bricks present
out=$("$PEER" "q1_add16 3 then q1_xor16" 2>/dev/null || true)
echo "$out" | grep -q 'SOURCE LOCAL' && pass "chain_local" || failm "chain_local"
echo "$out" | grep -q 'CLAIMED_CERT 1' && pass "chain_cert" || failm "chain_cert"
echo "$out" | grep -E 'ANSWER 5' >/dev/null && pass "chain_value_5" || failm "chain_value_5:$out"

# 2) delete xor → must remint via evolve
cp -a "$BR/q1_xor16.lut" /tmp/q1_xor16.lut.rsi.bak 2>/dev/null || true
rm -f "$BR/q1_xor16.lut"
systemctl --user restart cnetd.service
sleep 2
out2=$("$PEER" "q1_add16 3 then q1_xor16" 2>/dev/null || true)
echo "$out2" | head -20
if [[ -f "$BR/q1_xor16.lut" ]]; then pass "xor_lut_reminted"; else failm "xor_lut_reminted"; fi
echo "$out2" | grep -q 'SOURCE LOCAL' && pass "retry_local" || failm "retry_local"
echo "$out2" | grep -E 'ANSWER 5' >/dev/null && pass "retry_value_5" || failm "retry_value_5"

# 3) residual must not auto-cert on open chat
out3=$("$PEER" "write a haiku about rain" 2>/dev/null || true)
echo "$out3" | grep -q 'CLAIMED_CERT 0' && pass "open_chat_no_cert" || failm "open_chat_no_cert"
echo "$out3" | grep -E 'SOURCE (CORE|STAGE)' >/dev/null && pass "open_chat_core_or_stage" || failm "open_chat_source"

# 4) journal evidence of evolve switch (soft if remint already proved)
jok=0
if journalctl --user -u cnetd.service --since "10 min ago" --no-pager 2>/dev/null | grep -qE 'CNET_CORE_EVOLVE_OK|self_evolve_tick=1|factory curriculum'; then
  jok=1
fi
if [[ "$jok" -eq 1 ]]; then
  pass "evolve_switch_journal"
elif [[ -f "$BR/q1_xor16.lut" ]]; then
  pass "evolve_switch_journal_soft"
else
  failm "evolve_switch_journal"
fi

echo "=== summary fail=$fail ==="
if [[ "$fail" -eq 0 ]]; then
  echo "RSI_LIVE_PASS"
  exit 0
fi
echo "RSI_LIVE_FAIL"
exit 1
