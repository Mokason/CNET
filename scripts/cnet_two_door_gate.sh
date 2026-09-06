#!/usr/bin/env bash
# Two-door mouth invariants against a live cnetd. Never starts a daemon.
set -uo pipefail
ASK="${CNET_PLAYER_ASK:-$HOME/AI/CNET/scripts/cnet_sock_ask.sh}"
SOCK="${CNET_SOCK:-}"
failures=0
if [[ -z "$SOCK" ]]; then
  if [[ -n "${XDG_RUNTIME_DIR:-}" && -S "$XDG_RUNTIME_DIR/cnet/cnet.sock" ]]; then
    SOCK="$XDG_RUNTIME_DIR/cnet/cnet.sock"
  else
    SOCK="$HOME/.local/share/cnet-minimal/run/cnet.sock"
  fi
fi
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1${2:+: $2}" >&2; failures=$((failures + 1)); }

ask() {
  timeout 8s "$ASK" "$1" 2>&1 || echo "TIMEOUT"
}

field() {
  local key="$1" payload="$2" line value=""
  while IFS= read -r line; do
    case "$line" in
      "$key "*) value="${line#"$key "}" ;;
    esac
  done <<< "$payload"
  printf '%s' "$value"
}

if [[ ! -S "$SOCK" ]]; then
  fail "socket $SOCK"
  echo "TWO_DOOR_GATE_FAIL"
  exit 1
fi

add="$(CNET_SOCK="$SOCK" ask "10+11")"
[[ "$(field CLAIMED_CERT "$add")" == "1" ]] && pass "add CERT 1" || fail "add CERT"
[[ "$(field ANSWER "$add")" == "21" ]] && pass "add 21" || fail "add answer"
[[ "$(field STAGE_DRAFT "$add")" == "0" ]] && pass "add no stage" || fail "add stage"

who="$(CNET_SOCK="$SOCK" ask "who are you")"
[[ "$(field CLAIMED_CERT "$who")" == "1" ]] && pass "who CERT 1" || fail "who CERT"
[[ "$(field STAGE_DRAFT "$who")" == "0" ]] && pass "who never STAGE" || fail "who STAGE"
[[ "$(field SKILL "$who")" == "soul_who" ]] && pass "who skill" || fail "who skill $(field SKILL "$who")"

json="$(CNET_SOCK="$SOCK" ask "Do you know json?")"
[[ "$(field CLAIMED_CERT "$json")" == "0" ]] && pass "json CERT 0" || fail "json CERT"
ans="$(field ANSWER "$json")"
[[ "$ans" == "Not sealed." || "$ans" == "Not sealed. Logged for improve." ]] && pass "json ANSWER starve" || fail "json ANSWER" "$ans"
sd="$(field STAGE_DRAFT "$json")"
if [[ "$sd" == "1" && "$ans" != "Not sealed." && "$ans" != "Not sealed. Logged for improve." ]]; then
  fail "invariant STAGE_DRAFT implies Not sealed" "$ans"
else
  pass "json STAGE vs ANSWER invariant"
fi

# Parse invariants on add+who+json
for blob in "$add" "$who" "$json"; do
  sd="$(field STAGE_DRAFT "$blob")"
  cert="$(field CLAIMED_CERT "$blob")"
  ans="$(field ANSWER "$blob")"
  if [[ "$sd" == "1" && "$cert" != "0" ]]; then
    fail "STAGE_DRAFT implies CERT 0"
  fi
  if [[ "$cert" == "1" && "$sd" != "0" ]]; then
    fail "CERT 1 implies STAGE_DRAFT 0"
  fi
done
pass "bit invariants on sample replies"

if (( failures == 0 )); then
  echo "TWO_DOOR_GATE_PASS"
  exit 0
fi
echo "TWO_DOOR_GATE_FAIL failures=$failures" >&2
exit 1
