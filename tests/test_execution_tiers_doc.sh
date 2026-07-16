#!/bin/sh
set -eu

doc=docs/EXECUTION_TIERS.md
makefile=Makefile
gate=tests/test_alt_paths_gate.c

fail() { printf 'EXECUTION_TIERS_DOC_FAIL: %s\n' "$1" >&2; exit 1; }

grep -q 'CCE_AICIMO.*src/cce/cce_aicimo.c' "$makefile" ||
    fail 'Makefile no longer defines canonical AICIMO source'
grep -q '$(CCE_AICIMO)' "$makefile" ||
    fail 'canonical AICIMO source is absent from CCE aggregate'
grep -q 'AICIMO is in the core CCE aggregate' "$gate" ||
    fail 'alternate-path executable gate no longer requires AICIMO core'

if ! grep -A2 '`src/cce/cce_aicimo.c`' "$doc" | grep -q 'core CCE aggregate'; then
    fail 'documentation does not classify canonical AICIMO as core'
fi
if grep -q '`src/cce/cce_aicimo.c`.*excluded\|`src/cce/cce_aicimo.c`.*quarantined\|CCE_AICIMO_SRC' "$doc"; then
    fail 'documentation still contains the retired AICIMO classification'
fi
if ! grep -A1 'cce_aicimo_bridge.c' "$doc" | grep -q 'experimental'; then
    fail 'documentation lost the bridge-file quarantine boundary'
fi
if ! grep -A1 'src/cnet_lm.c' "$doc" | grep -qi 'not in the core'; then
    fail 'documentation lost the legacy cnet_lm exclusion boundary'
fi

printf 'EXECUTION_TIERS_DOC_PASS\n'
