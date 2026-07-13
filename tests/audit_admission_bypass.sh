#!/bin/sh
# Static admission-bypass audit (Part A gate).
#
# The one admission door is specialist_admit (which wraps the low-level
# registry_add_certified). Production code must reach the registry THROUGH the
# specialist door so every admitted node carries a durable SpecialistKind; the
# raw registry_add_certified is an implementation detail of the admission layer
# itself. Only these allowlisted internals may call it directly:
#   - src/contract/contract.c  (defines registry_add_certified AND specialist_admit)
#   - src/specialist.c         (the specialist module internals)
#
# Any other production src/*.c that calls registry_add_certified( is a bypass:
# it would admit a node with an unstamped/garbage kind. This audit greps for
# such calls and fails (RED) until every production path is migrated.
#
# Exit 0 = no bypass (GREEN). Exit 1 = bypass found (RED).

set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
allow="src/contract/contract.c src/specialist.c"

# Files with a real CALL (identifier immediately followed by '(') — comments
# that merely mention the name (no paren) never match.
hits=$(grep -rlE 'registry_add_certified[[:space:]]*\(' "$root/src" --include='*.c' 2>/dev/null || true)

fail=0
for f in $hits; do
    rel=${f#"$root"/}
    case " $allow " in
        *" $rel "*) : ;;                       # sanctioned internal
        *)
            echo "BYPASS: $rel calls registry_add_certified() directly"
            echo "        -> admit through specialist_wrap_btn + specialist_admit"
            fail=1
            ;;
    esac
done

if [ "$fail" -ne 0 ]; then
    echo "ADMISSION_BYPASS_AUDIT_FAIL"
    exit 1
fi
echo "ADMISSION_BYPASS_AUDIT_PASS"
