#!/usr/bin/env bash
# Mojo must be OPTIONAL and never load-bearing.
#
# The probe block at the top of the Makefile promises exactly that:
#   "Mojo is OPTIONAL and never load-bearing ... absent, the dispatch layer
#    compiles with its Mojo branch preprocessed out and nothing else changes."
# The ABSENT path was always safe. This gate covers the PRESENT path, which
# was not.
#
# RED marker: MOJO_OPTIONAL_RED
#   A) MOJO_LIB expanded to "/libcnet_mojo.so" because line 129 used immediate
#      expansion (:=) against BIN_DIR, which is not defined until line 235.
#      The build rule is $(BIN_DIR)/libcnet_mojo.so, so the prerequisite matched
#      nothing: "No rule to make target '/libcnet_mojo.so'".
#   B) -DCNET_HAVE_MOJO went into GLOBAL CFLAGS, which switches
#      cce_mojo_dispatch.c onto its Mojo branch for every target. $(CCE)
#      contains that file and 101 recipes link $(CCE) while none of them pass
#      $(MOJO_LDFLAGS) -- so every one of them lost cnet_mojo_trit_matmul /
#      cnet_mojo_init at link time the moment a Mojo toolchain appeared on PATH.
#
# Both checks run regardless of whether the toolchain is installed: with Mojo
# absent they assert the same contract holds trivially.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

LOG="${MOJO_OPTIONAL_LOG:-$ROOT/logs/mojo_optional.log}"
TMP="$(mktemp -d /tmp/mojo-optional-XXXXXX)"
mkdir -p "$(dirname "$LOG")"
: >"$LOG"
trap 'rm -rf "$TMP"' EXIT

fails=0
check() { # name ok detail
  if [[ $2 == 1 ]]; then
    printf '  ok   %-30s %s\n' "$1" "${3:-}" | tee -a "$LOG"
  else
    printf '  FAIL %-30s %s\n' "$1" "${3:-}" | tee -a "$LOG"
    fails=$((fails + 1))
  fi
}

PROBE=$(make --no-print-directory print-MOJO_PROBE 2>/dev/null | tr -d '[:space:]')
MOJO_LIB=$(make --no-print-directory print-MOJO_LIB 2>/dev/null | tr -d '[:space:]')
CFLAGS_ALL=$(make --no-print-directory print-CFLAGS 2>/dev/null)
echo "probe=$PROBE mojo_lib=$MOJO_LIB" | tee -a "$LOG"

# A) MOJO_LIB must sit under BIN_DIR, never at the filesystem root.
if [[ $PROBE == yes ]]; then
  ok_a=$([[ $MOJO_LIB == bin/* || $MOJO_LIB == */bin/* ]] && echo 1 || echo 0)
  check mojo_lib_under_bindir "$ok_a" "MOJO_LIB=$MOJO_LIB"
else
  check mojo_lib_under_bindir "$([[ -z $MOJO_LIB ]] && echo 1 || echo 0)" "toolchain absent, MOJO_LIB empty"
fi

# B) The load-bearing check. Compile the dispatch layer exactly the way a
#    $(CCE)-linked target does -- global CFLAGS, no $(MOJO_LDFLAGS) -- and
#    require that it links. This is the check that catches a global
#    -DCNET_HAVE_MOJO.
cat >"$TMP/main.c" <<'EOF'
/* Stand-in for any of the 101 recipes that link $(CCE) without $(MOJO_LDFLAGS). */
int main(void) { return 0; }
EOF

# shellcheck disable=SC2086
if gcc $CFLAGS_ALL -Iinclude -o "$TMP/probe_link" \
      src/cce/cce_trit_kernel.c src/cce/cce_mojo_dispatch.c "$TMP/main.c" \
      -lm >>"$LOG" 2>&1; then
  check dispatch_links_without_mojo 1 "global CFLAGS link clean"
else
  check dispatch_links_without_mojo 0 "cce_mojo_dispatch.c needs Mojo symbols under global CFLAGS"
fi

# C) -DCNET_HAVE_MOJO must not be global. Structural mirror of (B) so the
#    failure names the cause and not just the symptom.
if [[ $CFLAGS_ALL == *-DCNET_HAVE_MOJO* ]]; then
  check have_mojo_not_global 0 "-DCNET_HAVE_MOJO found in global CFLAGS"
else
  check have_mojo_not_global 1 "scoped to Mojo targets"
fi

if [[ $fails -eq 0 ]]; then
  echo "MOJO_OPTIONAL_PASS checks=3 fails=0 probe=$PROBE" | tee -a "$LOG"
else
  echo "MOJO_OPTIONAL_RED checks=3 fails=$fails probe=$PROBE" | tee -a "$LOG"
  exit 1
fi
