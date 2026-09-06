#!/bin/sh
# test_recipe_gates.sh -- static regression gate: Makefile recipes must
# propagate non-zero exits from test/model/demo executables instead of
# swallowing them with `|| echo ...` (which produces a false-green build).
#
# Legitimate patterns that are ALLOWED:
#   - Metadata fallback:  `git rev-parse ... || echo unknown`
#   - Optional artifact:   `test -f X || ./bin/gen_...` (generates if absent)
#   - Deliberate grep:     `grep ... || true`
#   - Symbol check:        `nm ... || exit 1`
#   - Model evidence collector: `CNET_REQUIRE_REAL_MODEL=1 ... || :` is
#     allowed because the strict run-scoped ledger is the target's final exit.
#   - Shell var assignment with $(shell ...) fallback
#
# What is REJECTED:
#   - Recipe lines that run an executable (./$(BIN_DIR)/X, or $(BIN_DIR)/X)
#     followed by `|| echo ...` — the echo masks a non-zero exit.
#   - Recipe lines that compile && run followed by `|| echo ...` — both
#     compile and run failures are swallowed.
#
# Usage: sh tests/test_recipe_gates.sh [Makefile-path]
# Exit 0 = pass (no swallowed exits), exit 1 = fail (swallowed exits found).

set -u
MAKEFILE="${1:-Makefile}"

if [ ! -f "$MAKEFILE" ]; then
    printf 'FAIL: Makefile not found at %s\n' "$MAKEFILE"
    exit 1
fi

fail=0
found=""

# Hoisted out of the loop on purpose. This used to be `"$(printf '\t')"*)`
# evaluated as a case pattern on every iteration, which forked a subshell per
# line; combined with the per-line `sed` below that was two process spawns for
# each of ~5900 Makefile lines. On Windows/MinGW, where fork+exec is orders of
# magnitude costlier than on Linux, that made this gate take >90s of almost
# pure sys time. It is the FIRST prerequisite of `verify`, so that cost was
# paid on every single gate run. Pure parameter expansion is exact-equivalent
# here and spawns nothing.
TAB=$(printf '\t')

# Read the Makefile line by line. We only inspect recipe lines (tab-indented).
# Comments and variable assignments are skipped.
while IFS= read -r rawline; do
    # Skip lines that don't start with a tab (non-recipe lines: vars, comments, targets).
    case "$rawline" in
        "$TAB"*) ;;  # tab-prefixed: recipe line, continue
        *) continue ;;
    esac

    # Strip leading @ prefix and leading whitespace for analysis.
    # (was: printf | sed 's/^\t//; s/^@//' -- one subprocess per recipe line)
    line=${rawline#"$TAB"}
    line=${line#@}

    # Skip comment-only recipe lines.
    case "$line" in
        \#*) continue ;;
    esac

    # Skip empty lines.
    [ -z "$line" ] && continue

    # --- Check for `|| echo`, `|| true`, or `|| :` swallowing patterns ---
    # We need to distinguish guarded evidence collection from an executable exit
    # being silently converted to success.

    # ALLOW: git rev-parse ... || echo unknown  (metadata fallback)
    # ALLOW: $(shell ... || echo ...)  (make variable assignment)
    # ALLOW: test -f X || ./bin/gen/...  (optional artifact generation)
    # ALLOW: grep ... || true  (deliberate grep)
    # ALLOW: nm ... || exit 1  (symbol check, already exits)
    # ALLOW: $@ || ./bin/gen  (optional artifact generation)

    case "$line" in
        *'git rev-parse'*'||'*) continue ;;   # metadata fallback
        *'$(shell '*'||'*) continue ;;           # make shell var
        *'git '*'|| echo'*) continue ;;          # other git metadata
    esac

    # Check if this line has a swallowed executable pattern.
    # Pattern: runs a binary from BIN_DIR and swallows with || echo
    # Also catch: compile && run || echo (both failures swallowed)

    has_swallow=0
    case "$line" in
        *'|| echo '*|*'|| echo \"'*)
            has_swallow=1
            ;;
        *'|| true'*|*'|| true '*)
            # Only flag || true if it's NOT a grep (grep || true is allowed).
            case "$line" in
                *'grep '*'|| true'*) ;;  # deliberate grep, allowed
                *'|| true'*)
                    # Check if an executable is being swallowed.
                    #
                    # ORDER IS LOAD-BEARING. The executable patterns must be
                    # tested BEFORE the generic `$(` skip: POSIX `case` takes
                    # the first matching arm, and `$(BIN_DIR)` itself contains
                    # `$(`, so a leading `*'$('*)` arm silently swallowed every
                    # `$(BIN_DIR)/x || true` line and made this whole branch
                    # dead code. Keep the executable arm first.
                    case "$line" in
                        *'./'*|*'$(BIN_DIR)'*) has_swallow=1 ;;
                        *'$('*)  ;;  # other make/shell function, skip
                    esac
                    ;;
            esac
            ;;
        *'|| :'*)
            # Per-program model evidence exits are collected so both logs are
            # produced; strict gen_claims.sh is the final non-zero authority.
            # Any other executable `|| :` is an unguarded false-green.
            case "$line" in
                *'CNET_REQUIRE_REAL_MODEL=1'*'scripts/gen_claims.sh'*) ;; # same-line collector
                *'CNET_REQUIRE_REAL_MODEL=1'*) ;;                        # model_evidence recipe
                *'./'*|*'$(BIN_DIR)'*) has_swallow=1 ;;
            esac
            ;;
    esac

    if [ "$has_swallow" -eq 0 ]; then
        continue
    fi

    # Now check if the line actually runs an executable (not just metadata).
    # We look for patterns indicating executable execution:
    #   - ./$(BIN_DIR)/  or  $(BIN_DIR)/
    #   - compile && ./$(BIN_DIR)/  (compile+run)
    #   - Direct binary execution like ./bin/...

    runs_executable=0
    case "$line" in
        *'./$(BIN_DIR)/'*|*'$(BIN_DIR)/'*|*'./bin/'*)
            runs_executable=1
            ;;
    esac

    # Also catch compile && run patterns (cc/gcc/$(CC) ... && ./$(BIN_DIR)/...)
    case "$line" in
        *'&&'*'./$(BIN_DIR)/'*|*'&&'*'$(BIN_DIR)/'*)
            runs_executable=1
            ;;
    esac

    if [ "$runs_executable" -eq 1 ]; then
        # This is a swallowed executable failure.
        fail=$((fail + 1))
        # Extract a short snippet for the report.
        snippet=$(printf '%s' "$line" | cut -c1-120)
        found="${found}  SWALLOWED: ${snippet}\n"
    fi

done < "$MAKEFILE"

if [ "$fail" -gt 0 ]; then
    printf 'RECIPE_GATE: %d swallowed-executable pattern(s) found:\n' "$fail"
    printf '%b' "$found"
    printf '\nRECIPE_GATE: FAIL — executable failures are masked by || echo/|| true.\n'
    exit 1
fi

# --- pipeline status propagation ------------------------------------------
# The `|| echo` scan above is blind to `producer | tee log`: tee exits 0 no
# matter what the producer did, so a crash, timeout or sanitizer teardown
# failure after the PASS marker was still a green recipe. Declaring this gate
# sound while ignoring every pipeline is exactly the kind of misleading gate
# the re-analysis flagged, so the declaration is now a hard requirement.
if ! grep -qE '^SHELL[[:space:]]*:?=[[:space:]]*.*bash' "$MAKEFILE"; then
    printf 'RECIPE_GATE: FAIL — no bash SHELL declared; pipefail is unavailable.\n'
    exit 1
fi
if ! grep -qE '^\.SHELLFLAGS[[:space:]]*:?=.*pipefail' "$MAKEFILE"; then
    printf 'RECIPE_GATE: FAIL — .SHELLFLAGS does not enable pipefail; %s\n' \
        'a producer that prints PASS then exits non-zero would be green.'
    exit 1
fi
pipelines=$(grep -c '| tee' "$MAKEFILE")

# --- headline gates must be .PHONY ----------------------------------------
# A same-named root file newer than its prerequisites makes Make declare an
# action target up to date: the gate reports success without compiling,
# running, or refreshing any evidence.
#
# The list lives in tests/headline_gates.txt so that this gate and its own
# selftest cannot drift apart -- two private copies would mean the selftest was
# validating a shadow of this gate instead of this gate.
GATES_FILE="$(dirname "$0")/headline_gates.txt"
if [ ! -f "$GATES_FILE" ]; then
    printf 'RECIPE_GATE: FAIL — %s is missing; cannot check .PHONY coverage.\n' "$GATES_FILE"
    exit 1
fi
HEADLINE_GATES=$(sed 's/#.*//' "$GATES_FILE" | tr -s '[:space:]' ' ')
if [ -z "$(printf '%s' "$HEADLINE_GATES" | tr -d '[:space:]')" ]; then
    printf 'RECIPE_GATE: FAIL — %s is empty; a silently empty list would pass vacuously.\n' "$GATES_FILE"
    exit 1
fi

# Modular targets are declarations too. Synthetic selftest Makefiles remain
# isolated; the real root includes its checked-in make fragments.
set -- "$MAKEFILE"
if [ "$MAKEFILE" = Makefile ]; then
    for fragment in mk/*.mk; do set -- "$@" "$fragment"; done
fi
phony_lines=$(grep -h '^\.PHONY:' "$@")
missing_phony=""
missing_count=0
for gate in $HEADLINE_GATES; do
    # The target must exist at all -- a renamed gate silently passing this
    # check would be worse than a red one.
    if ! grep -qE "^${gate}:" "$@"; then
        missing_phony="${missing_phony}  MISSING TARGET: ${gate}\n"
        missing_count=$((missing_count + 1))
        continue
    fi
    if ! printf '%s\n' "$phony_lines" | grep -qE "(^|[[:space:]])${gate}([[:space:]]|$)"; then
        missing_phony="${missing_phony}  NOT PHONY: ${gate}\n"
        missing_count=$((missing_count + 1))
    fi
done

if [ "$missing_count" -gt 0 ]; then
    printf 'RECIPE_GATE: %d headline gate(s) are shadowable:\n' "$missing_count"
    printf '%b' "$missing_phony"
    printf '\nRECIPE_GATE: FAIL — every action/headline gate must be .PHONY.\n'
    exit 1
fi

gate_count=$(printf '%s\n' $HEADLINE_GATES | wc -l | tr -d ' ')
printf 'RECIPE_GATE_PASS: no swallowed executable exits; pipefail on for %s pipeline(s); %s headline gate(s) .PHONY.\n' \
    "$pipelines" "$gate_count"
exit 0
