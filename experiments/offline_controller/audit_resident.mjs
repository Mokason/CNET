// Regression-only audit against the already frozen research suite. Independent
// Floyd-Warshall labels and transition checks; no C teacher/encoder imported.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import assert from 'node:assert/strict';
const [frozen,root]=process.argv.slice(2);
assert(frozen&&root,'usage: node audit_resident.mjs frozen-evidence resident-evidence');
const tasks=fs.readFileSync(path.join(frozen,'confirmation.tsv'),'utf8').trim().split('\n').map(line=>{
  const [e,c,s,g]=line.split(' '),key=BigInt(`0x${e}`)&BigInt(`0x${c}`);
  const legal=(u,v)=>Number.isInteger(u)&&u>=0&&u<8&&Number.isInteger(v)&&v>=0&&v<8&&!!(key&(1n<<BigInt(u*8+v)));
  const d=Array.from({length:8},(_,u)=>Array.from({length:8},(_,v)=>u===v?0:legal(u,v)?1:99));
  for(let k=0;k<8;k++)for(let u=0;u<8;u++)for(let v=0;v<8;v++)d[u][v]=Math.min(d[u][v],d[u][k]+d[k][v]);
  return {s:+s,g:+g,legal,d};
});assert.equal(tasks.length,2048);
const sha=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const groups=new Map(),summaries=[];let actions=0;
for(const line of fs.readFileSync(path.join(root,'confirmation.jsonl'),'utf8').trim().split('\n')){
  const r=JSON.parse(line);assert.equal(r.split,'confirmation');assert([1,2,3].includes(r.seed));assert([0,1].includes(r.mask));
  assert.equal(r.mode??r.variant,'aligned_structural_stream');const key=`${r.seed}:${r.mask}`;
  if(r.kind==='checkpoint'){
    assert(!groups.has(key));assert.equal(r.loaded_sha256,sha(path.join(root,`resident-${r.seed}-30000.weights`)));
    groups.set(key,{states:tasks.map(t=>({cur:t.s,done:false,steps:0,complete:0,abstain:0,illegal:0,raw:0,cycles:0,transitions:0,seen:new Set([t.s])})),curve:false});continue;
  }
  const group=groups.get(key);assert(group&&!group.curve);
  if(r.kind==='action'){
    assert(Number.isInteger(r.case)&&r.case>=0&&r.case<2048);const t=tasks[r.case],s=group.states[r.case];
    assert(!s.done&&s.steps<8);assert.equal(r.current,s.cur);assert.equal(r.step,s.steps++);
    assert(Number.isInteger(r.proposal)&&r.proposal>=0&&r.proposal<=8);
    assert(Number.isInteger(r.pre_mask_proposal)&&r.pre_mask_proposal>=0&&r.pre_mask_proposal<=8);
    s.raw+=Number(r.pre_mask_proposal!==8&&!t.legal(s.cur,r.pre_mask_proposal));if(!r.mask)assert.equal(r.proposal,r.pre_mask_proposal);
    const checked=t.legal(s.cur,r.proposal)?r.proposal===t.g?2:1:0;assert.equal(r.check,checked);
    if(r.mask&&r.proposal!==8)assert(checked);
    if(r.proposal===8){s.done=true;s.abstain=Number(t.d[t.s][t.g]===99);}
    else if(!checked)s.illegal++;
    else {s.transitions++;s.cycles+=Number(s.seen.has(r.proposal));s.seen.add(r.proposal);s.cur=r.proposal;if(checked===2){s.done=true;s.complete=1;}}
    actions++;continue;
  }
  assert.equal(r.kind,'curve');group.curve=true;
  const counts={completed:0,unreachable_abstained:0,illegal:0,raw_illegal:0,transitions:0,cycles:0,attempts:0,timeouts:0};
  const bins=Array(7).fill(0),solved=Array(7).fill(0);let reachable=0;
  for(let i=0;i<2048;i++){
    const t=tasks[i],s=group.states[i],d=t.d[t.s][t.g];assert(s.steps>0&&(s.done||s.steps===8));
    if(d<99){reachable++;bins[d-1]++;solved[d-1]+=s.complete;}
    counts.completed+=s.complete;counts.unreachable_abstained+=s.abstain;counts.illegal+=s.illegal;counts.raw_illegal+=s.raw;
    counts.transitions+=s.transitions;counts.cycles+=s.cycles;counts.attempts+=s.steps;counts.timeouts+=Number(!s.done);
  }
  for(const [name,count] of Object.entries(counts))assert.equal(r[name],count,name);
  assert.equal(r.reachable,reachable);assert.equal(r.unreachable,2048-reachable);assert.equal(r.cases,2048);
  assert.deepEqual(r.distance_cases,bins);assert.deepEqual(r.distance_completed,solved);
  const completion=r.completed/reachable,abstention=r.unreachable_abstained/r.unreachable;
  assert(completion>=.95&&abstention>=.95,'unchanged task floor failed');
  summaries.push({seed:r.seed,mask:r.mask,completion,abstention,completed:r.completed,unreachable_abstained:r.unreachable_abstained});
}
assert.equal(groups.size,6);assert.equal(summaries.length,6);
console.log(JSON.stringify({marker:'RESIDENT_CONFIRMATION_AUDIT_PASS',regression_not_new_holdout:true,cases:2048,checked_actions:actions,invalid_acceptances:0,summaries},null,2));
