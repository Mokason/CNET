#!/usr/bin/env bash
set -euo pipefail
if [[ ${2:-} == *zz99* ]]; then
  echo 'source=ASK_USER skill=- verified=0 miss=1 tokens=0'
  echo 'A: ABSTAIN: simulated failed request'
  exit 23
fi
exec "$(dirname "$0")/roe_front_door.saved" "$@"
