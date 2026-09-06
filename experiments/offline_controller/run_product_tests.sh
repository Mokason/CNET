#!/usr/bin/env bash
set -euo pipefail
build=${1:?absolute build directory required}
case "$build" in /*) ;; *) exit 2 ;; esac
evidence=$(mktemp -d /tmp/cnet-gpu-product-XXXXXX)
"$build/test_cell"
"$build/test_selector"
"$build/test_candidate"
"$build/test_worker_sandbox"
"$build/test_workers" "$build/gpu_worker_test" "$evidence/worker.native-weights"
"$build/test_workers" "$build/gpu_worker" "$evidence/production-worker.native-weights" production
cmp "$evidence/worker.native-weights" "$evidence/production-worker.native-weights"
"$build/test_cell_capsule" "$evidence/worker.native-weights" "$evidence/capsules"
mkdir -m 700 "$evidence/registry"
cp -a "$evidence/capsules/gpu_local_or" "$evidence/registry/"
"$build/test_host" "$evidence/worker.native-weights" "$evidence/capsules/empty" "$evidence/registry"
"$build/bench_cell_serving" "$evidence/worker.native-weights" "$evidence/registry"
printf 'GPU_PRODUCT_TEST_PASS evidence=%s live_services_changed=0\n' "$evidence"
