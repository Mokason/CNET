#!/usr/bin/env bash
set -euo pipefail
# Inspect the full recursive dependency graph without executing fixture writers.
commands=$(make --no-print-directory -n authority)
if printf '%s\n' "$commands" | grep -Eq '^([^[:space:]]*/)?roe_daily_packs_seed([[:space:]]|$)|^\./bin/roe_front_door (selftest|ask)'; then
    echo AUTHORITY_FIXTURE_ISOLATION_RED
    exit 1
fi
echo AUTHORITY_FIXTURE_ISOLATION_PASS
