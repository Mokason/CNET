import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
const [input,original]=process.argv.slice(2);assert(input&&original);
const root=path.resolve(input),scratch=fs.mkdtempSync(path.join(os.tmpdir(),'cnet-investigation-audit-'));
for(const name of ['source','bin','checkpoints','training-source-v7.c','freeze.json','confirmation.tsv'])fs.symlinkSync(path.join(root,name),path.join(scratch,name));
const rows=fs.readFileSync(path.join(root,'confirmation.jsonl'),'utf8').trim().split('\n').map(JSON.parse);
const audit=()=>spawnSync(process.execPath,[path.join(root,'source/audit_investigation.mjs'),scratch,original],{encoding:'utf8'});
function trial(name,change){
  const copy=structuredClone(rows);if(change)change(copy);
  fs.writeFileSync(path.join(scratch,'confirmation.jsonl'),copy.map(r=>JSON.stringify(r)).join('\n')+'\n');
  const result=audit();assert.equal(result.signal,null);
  if(change){assert.notEqual(result.status,0,name);assert(result.stderr.includes('AssertionError'),name);}
  else assert.equal(result.status,0,result.stderr);
}
trial('valid',null);
trial('false acceptance',r=>{r.find(x=>x.kind==='action'&&x.proposal!==8&&x.check===0).check=2;});
trial('raw illegal total',r=>{r.find(x=>x.kind==='curve').raw_illegal++;});
trial('checkpoint substitution',r=>{r.find(x=>x.kind==='checkpoint'&&x.seed===2).loaded_sha256=r.find(x=>x.kind==='checkpoint'&&x.seed===1).loaded_sha256;});
trial('completion inflation',r=>{r.find(x=>x.kind==='curve').completed++;});
trial('missing action',r=>{r.splice(r.findIndex(x=>x.kind==='action'),1);});
trial('distance bucket inflation',r=>{r.find(x=>x.kind==='curve').distance_completed[1]++;});
console.log('CONTROLLER_INVESTIGATION_AUDIT_MUTATION_PASS corruptions_refused=6 valid_accepted=1');
