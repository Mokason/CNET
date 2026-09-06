#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
[[ $# -eq 1 ]]
scratch=$(mktemp -d /tmp/cnet-controller-audit-fault-XXXXXX)
trap 'rm -rf -- "$scratch"' EXIT
# Deliberately corrupt a trace, not the original evidence.
node --input-type=module - "$1" "$scratch" <<'JS'
import fs from 'node:fs';
import path from 'node:path';
const [source,out]=process.argv.slice(2);
for(const file of ['train.tsv','validation.tsv','test.tsv','fixtures.sha256'])
  fs.copyFileSync(path.join(source,file),path.join(out,file));
const records=fs.readFileSync(path.join(source,'results.jsonl'),'utf8').trim().split('\n').map(JSON.parse);
const bad=records.find(r=>r.kind==='action'&&r.check===0&&r.proposal<8);
if(!bad) throw Error('no negative action to corrupt');
bad.check=2;
fs.writeFileSync(path.join(out,'results.jsonl'),records.map(r=>JSON.stringify(r)).join('\n')+'\n');
JS
if node "$here/audit.mjs" "$scratch" >"$scratch/audit.log" 2>&1; then
  echo CONTROLLER_AUDIT_RED_accepted_false_arrival; exit 1
fi
grep -q 'independent edge/type verifier mismatch' "$scratch/audit.log"
echo CONTROLLER_AUDIT_FAULT_PASS corrupted_acceptance_refused=1
