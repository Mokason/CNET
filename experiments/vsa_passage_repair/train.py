"""Offline external-teacher passage labels, disjoint from frozen evaluation questions."""
import os
os.environ['OPENBLAS_NUM_THREADS']='1'
import json,hashlib,random,re,urllib.request,time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor,as_completed
import numpy as np
from probe import Native,ROOT,CACHE

def features(cosine,overlap,question,passages):
    c=cosine.astype(np.float64);o=overlap.astype(np.float64)
    def z(x):return (x-x.mean())/max(x.std(),1e-6)
    def gap(x):
        best=np.sort(x)[-2:];return x-np.where(x==best[-1],best[-2],best[-1])
    return np.column_stack([c,o,z(c),z(o),gap(c),gap(o),c*o,o*o,
      np.full(len(c),min(len(question.split()),64)/32),
      np.array([min(len(p.split()),128)/64 for p in passages]),
      c/(o+.1),o/(np.maximum(c,0)+.1)]).astype(np.float32)

def main():
    seed=random.Random(20260913)
    forbidden={r['q'] for r in json.loads((ROOT/'var/claude_scratch/answer_quality_q400_base.json').read_text())}
    forbidden.update(l.split('\t')[1] for l in (ROOT/'benchmarks/vsa_routing_arena_20260911/questions.tsv').read_text().splitlines() if l and not l.startswith('#'))
    pairs=defaultdict(list)
    source=ROOT/'benchmarks/vsa_routing_arena_20260911/questions_train_v3_all.tsv'
    for line in source.read_text().splitlines():
        if not line or line.startswith('#'):continue
        name,q,*_=line.split('\t')
        if q not in forbidden and (ROOT/'bin'/f'{name}.gencap').exists():pairs[name].append(q)
    names=sorted(pairs);seed.shuffle(names);names=names[:768]
    requests=[];native=Native()
    for name in names:
        q=seed.choice(pairs[name]);cap=native.load(name);co,ov=cap.scores([q]);f=features(co[0],ov[0],q,cap.texts)
        chosen=set(np.argsort(-co[0],kind='stable')[:2])|set(np.argsort(-ov[0],kind='stable')[:2])
        chosen.update(seed.sample(range(cap.n),min(2,cap.n)));chosen=sorted(chosen)
        requests.append({'capsule':name,'question':q,'passages':[cap.texts[i] for i in chosen],'features':f[chosen].tolist(),'indices':[int(i) for i in chosen]})
        cap.close()
    sourcehash=hashlib.sha256(source.read_bytes()).hexdigest()
    cache=CACHE/'training_labels.json';known={}
    if cache.exists():
        previous=json.loads(cache.read_text());assert previous['source_sha256']==sourcehash
        known={r['capsule']:r for r in previous['rows']}
    missing=[r for r in requests if r['capsule'] not in known]
    print('labeling',len(missing),'separate training questions',flush=True)
    def judge(row):
        n=len(row['passages']);prompt='Question: '+row['question']+'\n\n'+'\n'.join(f'{i+1}. {p}' for i,p in enumerate(row['passages']))
        system=f'For each of the {n} numbered technical passages, decide whether that passage by itself actually answers the question. Being related to the topic is not enough. Return exactly {n} comma-separated YES or NO labels, in passage order. No explanation.'
        body={'model':'mistral-small-3.2-24b-offline','temperature':0,'max_tokens':64,'messages':[{'role':'system','content':system},{'role':'user','content':prompt}]}
        for attempt in range(3):
            req=urllib.request.Request('http://127.0.0.1:8092/v1/chat/completions',data=json.dumps(body).encode(),headers={'Content-Type':'application/json'})
            with urllib.request.urlopen(req,timeout=60) as f:text=json.load(f)['choices'][0]['message']['content']
            (CACHE/'last_training_judge.txt').write_text(text)
            labels=[x.strip().upper() for x in text.strip().strip('[]').strip('.').split(',')]
            if len(labels)==n and all(x in ('YES','NO') for x in labels):return dict(row,labels=[x=='YES' for x in labels],raw_judgment=text)
        raise ValueError('invalid teacher labels '+text)
    with ThreadPoolExecutor(max_workers=2) as ex:
        for begin in range(0,len(missing),2):
            for future in as_completed([ex.submit(judge,r) for r in missing[begin:begin+2]]):
                r=future.result();known[r['capsule']]=r
                cache.write_text(json.dumps({'source_sha256':sourcehash,'model':'mistral-small-3.2-24b-offline','rows':list(known.values())},indent=2)+'\n')
                if len(known)%20==0:print('labeled',len(known),'/',len(requests),flush=True)
    print('TRAIN_LABELS_DONE',len(known),flush=True)

if __name__=='__main__':main()
