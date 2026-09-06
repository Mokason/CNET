// Independent trace auditor: BigInt bit checks and Floyd-Warshall, not C BFS.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
const root=process.argv[2];
assert(root, 'usage: node audit.mjs evidence-directory');
function readTasks(name, count) {
  const text=fs.readFileSync(path.join(root,`${name}.tsv`),'utf8');
  const rows=text.trim().split('\n').map(line=>{
    const [e,c,s,g]=line.split(' ');
    const task={e:BigInt(`0x${e}`),c:BigInt(`0x${c}`),s:Number(s),g:Number(g)};
    task.d=Array.from({length:8},(_,u)=>Array.from({length:8},(_,v)=>u===v?0:legal(task,u,v)?1:99));
    for(let k=0;k<8;k++) for(let u=0;u<8;u++) for(let v=0;v<8;v++)
      task.d[u][v]=Math.min(task.d[u][v],task.d[u][k]+task.d[k][v]);
    return task;
  });
  assert.equal(rows.length,count);
  return rows;
}
function legal(t,u,v) {
  if(!Number.isInteger(u)||!Number.isInteger(v)||u<0||u>7||v<0||v>7) return false;
  const mask=1n<<BigInt(u*8+v);
  return Boolean(t.e&t.c&mask);
}
const tasks={train:readTasks('train',8192),validation:readTasks('validation',512),test:readTasks('test',512)};
const topologies=new Set();
for(const t of Object.values(tasks).flat()) {
  const key=`${t.e}:${t.c}`;
  assert(!topologies.has(key),'split topology leakage'); topologies.add(key);
}
for(const line of fs.readFileSync(path.join(root,'fixtures.sha256'),'utf8').trim().split('\n')) {
  const [hash,file]=line.trim().split(/\s+/);
  assert.equal(crypto.createHash('sha256').update(fs.readFileSync(path.join(root,file))).digest('hex'),hash);
}
const rows=fs.readFileSync(path.join(root,'results.jsonl'),'utf8').trim().split('\n').map(JSON.parse);
const groups=new Map(); let checked=0,falseAccepted=0;
function groupKey(r) { return [r.split,r.seed,r.mode,r.injected].join(':'); }
for(const r of rows.filter(r=>r.kind==='action')) {
  const key=groupKey(r);
  if(!groups.has(key)) groups.set(key,new Map());
  const group=groups.get(key), t=tasks[r.split][r.case]; assert(t);
  if(!group.has(r.case)) group.set(r.case,{cur:t.s,attempts:0,illegal:0,complete:0,abstain:0,terminal:false,optimal:0,falseArrival:0});
  const s=group.get(r.case);
  assert(!s.terminal); assert.equal(r.step,s.attempts); assert.equal(r.current,s.cur);
  assert(r.step<8 && Number.isInteger(r.proposal) && r.proposal>=0 && r.proposal<=8);
  const expected=legal(t,s.cur,r.proposal)?(r.proposal===t.g?2:1):0;
  falseAccepted+=Number(r.check===2 && expected!==2);
  assert.equal(r.check,expected,'independent edge/type verifier mismatch'); checked++;
  if(!r.step) s.optimal=Number(r.proposal===8?t.d[t.s][t.g]===99:expected>0 && t.d[r.proposal][t.g]===t.d[t.s][t.g]-1);
  s.attempts++;
  if(r.proposal===8) { s.terminal=true; s.abstain=Number(t.d[t.s][t.g]===99); }
  else if(!expected) { s.illegal++; s.falseArrival+=Number(r.proposal===t.g); }
  else { s.cur=r.proposal; if(expected===2) { s.complete=1; s.terminal=true; } }
}
const evaluations=rows.filter(r=>r.kind==='evaluation');
assert.equal(evaluations.length,27); assert.equal(rows.filter(r=>r.kind==='training').length,9);
for(const r of evaluations) {
  const group=groups.get(groupKey(r)); assert.equal(group.size,512);
  let success=0,invalid=0,attempts=0,abstain=0,falseArrival=0,optimal=0;
  for(const [id,s] of group) {
    assert(s.terminal || s.attempts===8);
    success+=s.complete; invalid+=s.illegal; attempts+=s.attempts; abstain+=s.abstain; falseArrival+=s.falseArrival;
    optimal+=r.injected?groups.get([r.split,r.seed,r.mode,0].join(':')).get(id).optimal:s.optimal;
  }
  assert.equal(r.completed,success); assert.equal(r.illegal,invalid); assert.equal(r.attempts,attempts);
  assert.equal(r.optimal_first,optimal); assert.equal(r.unreachable_abstained,abstain);
  assert.equal(r.false_arrival_proposals,falseArrival);
  assert.equal(r.reachable,tasks[r.split].filter(t=>t.d[t.s][t.g]<99).length);
}
// Deterministic control uses the independent all-pairs planner, then checks each
// selected edge and the final goal. This does not use any trained output.
const control={cases:512,reachable:0,completed:0,unreachable_abstained:0,attempts:0};
for(const t of tasks.test) {
  let cur=t.s;
  if(t.d[cur][t.g]===99) {control.unreachable_abstained++;control.attempts++;continue;}
  control.reachable++;
  for(let step=0;step<8 && cur!==t.g;step++) {
    const next=Array.from({length:8},(_,i)=>i).find(v=>legal(t,cur,v)&&t.d[v][t.g]===t.d[cur][t.g]-1);
    assert.notEqual(next,undefined); assert(legal(t,cur,next)); cur=next; control.attempts++;
  }
  assert.equal(cur,t.g); control.completed++;
}
const summary={marker:'CONTROLLER_INDEPENDENT_AUDIT_PASS',unique_topologies:topologies.size,checked_actions:checked,false_accepted_observed:falseAccepted,deterministic_control:control,models:{}};
for(const mode of ['single','recurrent4','feedforward4']) {
  const selected=evaluations.filter(r=>r.split==='test' && !r.injected && r.mode===mode);
  const recovery=evaluations.filter(r=>r.split==='test' && r.injected && r.mode===mode);
  summary.models[mode]={active_parameters:selected[0].active_parameters,forward_flops_per_action:selected[0].forward_flops_per_action,
    test_completions:selected.map(r=>r.completed),reachable_per_seed:291,optimal_first:selected.map(r=>r.optimal_first),
    illegal:selected.map(r=>r.illegal),unreachable_abstained:selected.map(r=>r.unreachable_abstained),
    recovery_completions:recovery.map(r=>r.completed),subset32_completions:selected.map(r=>r.subset32_completed),
    mean_completion_rate:selected.reduce((s,r)=>s+r.completed/r.reachable,0)/3,
    batch_amortized_ms:selected.map(r=>r.batch_amortized_ms),
    executed_forward_flops_per_512_cases:512*8*selected[0].forward_flops_per_action};
}
const localPath=path.join(root,'local-baseline.jsonl');
if(fs.existsSync(localPath)) {
  const local=fs.readFileSync(localPath,'utf8').trim().split('\n').map(JSON.parse);
  const evals=local.filter(r=>r.kind==='local_evaluation'); assert.equal(evals.length,32);
  assert.equal(new Set(evals.map(r=>r.case)).size,32);
  for(const row of evals) {
    const t=tasks.test[row.case]; let cur=t.s, terminal=false, complete=0, illegal=0, abstain=0, optimal=0;
    const actions=local.filter(r=>r.kind==='local_action' && r.case===row.case);
    for(const [step,r] of actions.entries()) {
      assert(!terminal); assert.equal(r.step,step); assert.equal(r.current,cur); assert(step<8);
      const status=legal(t,cur,r.proposal)?(r.proposal===t.g?2:1):0;
      assert.equal(r.check,status);
      if(!step) optimal=Number(r.proposal===8?t.d[cur][t.g]===99:status>0 && t.d[r.proposal][t.g]===t.d[cur][t.g]-1);
      if(r.proposal===8) {terminal=true;abstain=Number(t.d[t.s][t.g]===99);}
      else if(!status) illegal++;
      else {cur=r.proposal;if(status===2){complete=1;terminal=true;}}
    }
    assert.equal(row.attempts,actions.length); assert.equal(row.completed,complete);
    assert.equal(row.optimal_first,optimal); assert.equal(row.illegal,illegal); assert.equal(row.unreachable_abstained,abstain);
  }
  summary.local_baseline=Object.fromEntries(['reachable','completed','optimal_first','attempts','illegal','unreachable_abstained','seconds'].map(k=>[k,evals.reduce((s,r)=>s+r[k],0)]));
  summary.local_baseline.cases=32;
}
console.log(JSON.stringify(summary,null,2));
