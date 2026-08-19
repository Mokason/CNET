#!/usr/bin/env bash
# Back-compat: DS4 dual launcher renamed to CNET Ember residual host.
# Prefer: scripts/run_cnet_ember_dual.sh
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# Map legacy CNET_DS4_* → CNET_EMBER_*
for k in ROOT SERVER MODEL API_HOST API_PORT DIST_HOST DIST_PORT LAYER_SPLIT \
         LAYER_AHEAD PREFILL_WINDOW CTX CACHE_EXPERTS RESERVE_GB ARENA_CHUNK_MB \
         NO_Q8_F16_CACHE BOUNDED_LAYER_MAP READY_TIMEOUT_SEC STATE_DIR UNIT_PREFIX \
         ALLOW_SHARED_GPU IDENTITY_FILE; do
  eval "v=\${CNET_DS4_${k}-}"
  if [[ -n ${v:-} ]]; then
    export "CNET_EMBER_${k}=$v"
  fi
done
exec bash "$ROOT/run_cnet_ember_dual.sh" "$@"
