#!/bin/sh
# test_recipe_gates.sh -- static regression gate: Makefile recipes must
# propagate non-zero exits from test/model/demo executables instead of
# swallowing them with `|| echo ...` (which produces a false-green build).
#
# Legitimate patterns that are ALLOWED:
#   - Metadata fallback:  `git rev-parse ... || echo unknown`
#   - Optional artifact:   `test -f X || python3 ...`   (generates if absent)
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

# Read the Makefile line by line. We only inspect recipe lines (tab-indented).
# Comments and variable assignments are skipped.
while IFS= read -r rawline; do
    # Skip lines that don't start with a tab (non-recipe lines: vars, comments, targets).
    case "$rawline" in
        "$(printf '\t')"*) ;;  # tab-prefixed: recipe line, continue
        *) continue ;;
    esac

    # Strip leading @ prefix and leading whitespace for analysis.
    line=$(printf '%s' "$rawline" | sed 's/^\t//; s/^@//')

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
    # ALLOW: test -f X || python3/gen/...  (optional artifact generation)
    # ALLOW: grep ... || true  (deliberate grep)
    # ALLOW: nm ... || exit 1  (symbol check, already exits)
    # ALLOW: $@ || python3  (optional artifact generation)

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
                    # Check if an executable is being swallowed
                    case "$line" in
                        *'$('*)  ;;  # shell function, skip
                        *'./'*|*'$(BIN_DIR)'*) has_swallow=1 ;;
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

printf 'RECIPE_GATE_PASS: no swallowed executable exits in Makefile recipes.\n'
exit 0