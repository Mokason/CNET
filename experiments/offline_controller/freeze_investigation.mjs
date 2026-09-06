// Run only after development. Fails closed on a missed selection floor.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import assert from 'node:assert/strict';
const [root,baseline,repo,binary]=process.argv.slice(2);assert(root&&baseline&&repo&&binary);
assert(!fs.existsSync(path.join(root,'confirmation.tsv')),'confirmation already exists');
const sha=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const freeze={candidate:'aligned_structural_stream',updates:30000,seeds:[1,2,3],reachable_floor:.95,unreachable_floor:.95,cases:2048,
  split:'sorted joint out/in degree pairs; FNV1a64 modulo5=0; excludes fixed effective graphs',generator_seed:96062026,
  preprocessing:'effective adjacency; current0 goal7; remaining IDs ascending; INPUTS144 padded',mask_modes:[0,1],created_utc:new Date().toISOString(),checkpoints:[],sources:[],invocations:[]};
fs.mkdirSync(path.join(root,'checkpoints'),{recursive:true});fs.mkdirSync(path.join(root,'source'),{recursive:true});fs.mkdirSync(path.join(root,'bin'),{recursive:true});
function retain(src,file){const dest=path.join(root,file);fs.copyFileSync(src,dest,fs.constants.COPYFILE_EXCL);return {file,sha256:sha(dest)};}
for(const seed of freeze.seeds){
  const rows=fs.readFileSync(path.join(root,`curve-7-${seed}-30000.jsonl`),'utf8').trim().split('\n').map(JSON.parse);
  const validation=rows.find(r=>r.kind==='curve'&&r.split==='validation'&&r.step===30000);assert(validation);
  assert(validation.completed/validation.reachable>=.95&&validation.unreachable_abstained/validation.unreachable>=.95,`CONTROLLER_SELECTION_RED seed=${seed}`);
  freeze.checkpoints.push({...retain(path.join(root,`diagnostic-aligned_structural_stream-${seed}-30000.weights`),`checkpoints/candidate-seed${seed}.native-weights`),mode:freeze.candidate,variant:7,seed,updates:30000,validation});
  freeze.checkpoints.push({...retain(path.join(baseline,`recurrent4-seed${seed}.native-weights`),`checkpoints/baseline-seed${seed}.native-weights`),mode:'raw',variant:0,seed,updates:600});
}
const files=['investigate.c','test_investigate.c','confirm.c','bench_train.c','net.c','net.h','fixture.c','fixture.h','offline.c','offline.h','audit_investigation.mjs','freeze_investigation.mjs','Makefile'];
for(const name of files)freeze.sources.push(retain(path.join(repo,'experiments/offline_controller',name),`source/${name}`));
freeze.sources.push(retain(path.join(repo,'src/cce/amdmath/cce_amdmath.cpp'),'source/cce_amdmath.cpp'));
freeze.sources.push(retain(path.join(repo,'include/cce/cce_amdmath.h'),'source/cce_amdmath.h'));
freeze.sources.push(retain(binary,'bin/confirm'));
freeze.sources.push({file:'training-source-v7.c',sha256:sha(path.join(root,'training-source-v7.c'))});
for(const item of freeze.checkpoints)for(const mask of freeze.mask_modes)freeze.invocations.push(['bin/confirm','evaluate',item.file,String(item.variant),String(item.seed),String(mask),'confirmation']);
fs.writeFileSync(path.join(root,'freeze.json'),JSON.stringify(freeze,null,2)+'\n',{flag:'wx'});
console.log('CONTROLLER_SELECTION_FROZEN variant=7 seeds=3 validation_floors_pass=1 confirmation_not_read=1');
