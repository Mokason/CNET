#!/usr/bin/env bash
# Off-T1 gate for CERT solver loop. Never CERT. Never make cnetd-run.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p logs
make cert_solver
grep -q CERT_SOLVER_PASS logs/cert_solver.log
# Not FAQ: two different 24-puzzles both bind.
./bin/cnet_cert_solver "make 24 from 8 8 3 3" | tee logs/cert_solver_24a.log
grep -q "claimed_cert=1" logs/cert_solver_24a.log
grep -q "BIND 24" logs/cert_solver_24a.log
./bin/cnet_cert_solver "make 24 from 1 3 4 6" | tee logs/cert_solver_24b.log
grep -q "claimed_cert=1" logs/cert_solver_24b.log
# Unshaped must not claim.
set +e
./bin/cnet_cert_solver "hello world" > logs/cert_solver_miss.log 2>&1
rc=$?
set -e
test "$rc" -ne 0
grep -q CERT_SOLVER_MISS logs/cert_solver_miss.log
echo CERT_SOLVER_GATE_PASS
