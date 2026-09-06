// Independent confirmation audit: no C encoder, reverse BFS or verifier used.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
const [root,original,candidate='aligned_structural_stream']=process.argv.slice(2);
assert(root&&original,'usage: node audit_investigation.mjs evidence original-evidence [candidate]');
function read(file,count) {
  const tasks=fs.readFileSync(file,'utf8').trim().split('\n').map(line=>{
    const [e,c,s,g]=line.split(' ');const t={e:BigInt(`0x${e}`),c:BigInt(`0x${c}`),s:+s,g:+g};
    assert(Number.isInteger(t.s)&&t.s>=0&&t.s<8&&Number.isInteger(t.g)&&t.g>=0&&t.g<8&&t.s!==t.g);
    t.key=t.e&t.c;return t;
  });assert.equal(tasks.length,count);return tasks;
}
function partition(key) {
  const pairs=Array.from({length:8},(_,u)=>{
    let incoming=0,outgoing=0;
    for(let v=0;v<8;v++){incoming+=Number((key>>BigInt(v*8+u))&1n);outgoing+=Number((key>>BigInt(u*8+v))&1n);}
    return outgoing*9+incoming;
  }).sort((a,b)=>a-b);
  let hash=14695981039346656037n;
  for(const pair of pairs)hash=BigInt.asUintN(64,(hash^BigInt(pair))*1099511628211n);
  return Number(hash%5n);
}
function legal(t,u,v){return Number.isInteger(v)&&v>=0&&v<8&&Boolean(t.key&(1n<<BigInt(u*8+v)));}
const old=new Set(['train','validation','test'].flatMap((s,i)=>read(path.join(original,`${s}.tsv`),i?512:8192).map(t=>t.key)));
const tasks=read(path.join(root,'confirmation.tsv'),2048),seen=new Set();
for(const t of tasks){
  assert(!old.has(t.key)&&!seen.has(t.key),'effective topology leakage');seen.add(t.key);assert.equal(partition(t.key),0);
  t.d=Array.from({length:8},(_,u)=>Array.from({length:8},(_,v)=>u===v?0:legal(t,u,v)?1:99));
  for(let k=0;k<8;k++)for(let u=0;u<8;u++)for(let v=0;v<8;v++)t.d[u][v]=Math.min(t.d[u][v],t.d[u][k]+t.d[k][v]);
}
const rows=fs.readFileSync(path.join(root,'confirmation.jsonl'),'utf8').trim().split('\n').map(JSON.parse);
const freeze=JSON.parse(fs.readFileSync(path.join(root,'freeze.json'),'utf8'));
assert.equal(freeze.candidate,candidate);
for(const entry of [...freeze.checkpoints,...freeze.sources]) {
  const actual=crypto.createHash('sha256').update(fs.readFileSync(path.resolve(root,entry.file))).digest('hex');
  assert.equal(actual,entry.sha256,'frozen artifact changed');
}
const groups=new Map(),evaluations=[],identities=new Set();let actions=0;
function key(r){return [r.seed,r.mode??r.variant,r.mask].join(':');}
for(const r of rows){
  assert.equal(r.split,'confirmation');assert([1,2,3].includes(r.seed));assert([0,1].includes(r.mask));
  assert(['raw',candidate].includes(r.mode??r.variant));
  if(r.kind==='checkpoint'){
    const expected=freeze.checkpoints.find(x=>x.mode===r.mode&&x.seed===r.seed);assert(expected);
    assert.equal(r.loaded_sha256,expected.sha256,'loaded checkpoint identity mismatch');
    assert(!identities.has(key(r)));identities.add(key(r));continue;
  }
  assert(identities.has(key(r)),'missing checkpoint identity');
  if(r.kind==='curve'){evaluations.push(r);continue;}assert.equal(r.kind,'action');
  const k=key(r);if(!groups.has(k))groups.set(k,new Map());const group=groups.get(k);
  assert(Number.isInteger(r.case)&&r.case>=0&&r.case<2048);const t=tasks[r.case];
  if(!group.has(r.case))group.set(r.case,{cur:t.s,steps:0,done:false,complete:0,abstain:0,illegal:0,rawIllegal:0,cycles:0,transitions:0,visited:new Set([t.s]),optimal:0,immediate:0});
  const s=group.get(r.case);assert(!s.done);assert.equal(r.step,s.steps);assert.equal(r.current,s.cur);
  assert(s.steps<8&&Number.isInteger(r.proposal)&&r.proposal>=0&&r.proposal<=8);
  assert(Number.isInteger(r.pre_mask_proposal)&&r.pre_mask_proposal>=0&&r.pre_mask_proposal<=8);
  s.rawIllegal+=Number(r.pre_mask_proposal!==8&&!legal(t,s.cur,r.pre_mask_proposal));
  if(!r.mask)assert.equal(r.proposal,r.pre_mask_proposal);
  const checked=legal(t,s.cur,r.proposal)?r.proposal===t.g?2:1:0;
  assert.equal(r.check,checked,'independent verifier mismatch');if(r.mask&&r.proposal!==8)assert(checked>0);
  if(!s.steps){s.optimal=Number(r.proposal===8?t.d[t.s][t.g]===99:checked>0&&t.d[r.proposal][t.g]===t.d[t.s][t.g]-1);s.immediate=Number(r.proposal===8&&t.d[t.s][t.g]<99);}
  s.steps++;actions++;
  if(r.proposal===8){s.done=true;s.abstain=Number(t.d[t.s][t.g]===99);}
  else if(!checked)s.illegal++;
  else {s.transitions++;s.cycles+=Number(s.visited.has(r.proposal));s.visited.add(r.proposal);s.cur=r.proposal;if(checked===2){s.done=true;s.complete=1;}}
}
assert.equal(identities.size,12);assert.equal(evaluations.length,12);assert.equal(groups.size,12);assert.equal(new Set(evaluations.map(key)).size,12);
const summary=[];
for(const r of evaluations){
  const group=groups.get(key(r));assert.equal(group.size,2048);
  const aggregate={completed:0,unreachable_abstained:0,illegal:0,raw_illegal:0,attempts:0,cycles:0,transitions:0,optimal_first:0,reachable_immediate_abstention:0,timeouts:0};
  const buckets=Array(7).fill(0),solved=Array(7).fill(0);let reachable=0;
  for(const [id,s] of group){
    assert(s.done||s.steps===8);const t=tasks[id],d=t.d[t.s][t.g];if(d<99){reachable++;buckets[d-1]++;solved[d-1]+=s.complete;}
    aggregate.completed+=s.complete;aggregate.unreachable_abstained+=s.abstain;aggregate.illegal+=s.illegal;aggregate.attempts+=s.steps;
    aggregate.raw_illegal+=s.rawIllegal;
    aggregate.cycles+=s.cycles;aggregate.transitions+=s.transitions;aggregate.optimal_first+=s.optimal;aggregate.reachable_immediate_abstention+=s.immediate;aggregate.timeouts+=Number(!s.done);
  }
  for(const [k,v] of Object.entries(aggregate))assert.equal(r[k],v,`aggregate ${k}`);
  assert.equal(r.cases,2048);assert.equal(r.reachable,reachable);assert.equal(r.unreachable,2048-reachable);
  assert.deepEqual(r.distance_cases,buckets);assert.deepEqual(r.distance_completed,solved);
  summary.push({...r,multihop_completed:solved.slice(1).reduce((a,b)=>a+b,0),multihop_cases:reachable-buckets[0],research_hurdle_pass:r.completed/reachable>=.95&&r.unreachable_abstained/(2048-reachable)>=.95});
}
console.log(JSON.stringify({marker:'CONTROLLER_CONFIRMATION_AUDIT_PASS',cases:2048,checked_actions:actions,invalid_acceptances_observed:0,effective_topology_disjoint:true,direct_goal_completed:tasks.filter(t=>legal(t,t.s,t.g)).length,evaluations:summary},null,2));
