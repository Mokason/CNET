"""Accepted-route experiment with pinned actual seal negatives and cached judgments."""
import os
os.environ['OPENBLAS_NUM_THREADS']='1'
import json,hashlib,math,subprocess,re,time,urllib.request
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from probe import Native,ROOT,CACHE

def zscore(s):
    v=s.astype(np.float64);best=v.max(axis=1);n=v.shape[1]
    mean=(v.sum(axis=1)-best)/(n-1)
    var=((v*v).sum(axis=1)-best*best-(n-1)*mean*mean)/(n-2)
    return ((best-mean)/np.sqrt(np.maximum(var,1e-12))).astype(np.float32)

def threshold(s,frac=.9):
    # >= acceptance: advance past the last score that must be rejected, including ties.
    rank=math.ceil(frac*len(s))-1
    t=np.nextafter(np.sort(s)[rank],np.float32(np.inf))
    assert (s<t).mean()>=frac
    return float(t)

def main():
    base=json.loads((ROOT/'var/claude_scratch/answer_quality_q400_base.json').read_text())
    raw=subprocess.run([str(CACHE/'baseline_cli'),'answer-batch','bin','1'],input=''.join(r['q']+'\n' for r in base),capture_output=True,text=True,check=True).stdout
    (CACHE/'baseline400.txt').write_text(raw)
    chunks=re.split(r'=== QUERY \d+ ===\n',raw)[1:]
    assert len(chunks)==len(base)
    for r,b in zip(base,chunks):
        m=re.search(r'Answer: (\S+) capsule=(\S+) route=(\S+)',b)
        assert tuple(m.groups())==(r['status'],r['cap'],r['route']),r['q']
        if r['status']=='OK':assert re.search(r'P1: sim=[^|]+\| (.*)',b).group(1)==r['passage']
    n=Native();entries=[]
    for entry in os.scandir(ROOT/'var/distill'):
        if not entry.name.endswith('.txt'):continue
        content=Path(entry.path).read_bytes();assert all(len(x)<1023 for x in content.splitlines())
        rows=[x.strip() for x in content.decode().splitlines() if x.strip() and not x.lstrip().startswith('#')]
        entries.append((entry.name,rows,hashlib.sha256(content).hexdigest()))
    (CACHE/'negative_sources.json').write_text(json.dumps([(a,h) for a,_,h in entries],indent=2)+'\n')
    records={k:[dict(r) for r in base] for k in ['control','hybrid_z','absolute']};calib=[]
    caps=sorted({r['cap'] for r in base if r['route']=='accept'})
    (CACHE/'negatives').mkdir(exist_ok=True)
    for ci,cap in enumerate(caps):
        c=n.load(cap);idx=[i for i,r in enumerate(base) if r['route']=='accept' and r['cap']==cap]
        rows=[t for name,lines,_ in entries if name not in (cap+'_corpus.txt',cap+'_probes.txt') for t in lines]
        path=CACHE/'negatives'/f'{cap}.json'
        if path.exists():negs=json.loads(path.read_text())
        else:negs=n.sample(cap,rows);path.write_text(json.dumps(negs)+'\n')
        ns,no=c.scores(negs);qs,qo=c.scores([base[i]['q'] for i in idx])
        zs=zscore(ns);original=float(np.sort(zs)[int(len(zs)*.9)])
        assert abs(max(1,original)-c.floor)<2e-4,(cap,original,c.floor)
        thresholds={'control':c.floor,'hybrid_z':max(1.,threshold(zscore(ns+.2*no))),'absolute':threshold((ns+.2*no).max(axis=1))}
        for mode in records:
            scores=qs if mode=='control' else qs+.2*qo
            values=scores.max(axis=1) if mode=='absolute' else zscore(scores)
            for j,i in enumerate(idx):
                r=records[mode][i];accept=bool(values[j]>=thresholds[mode]);r['status']='OK' if accept else 'REFUSE_PASSAGE'
                r['passage']=c.texts[int(scores[j].argmax())] if accept else None
                r['new_statistic']=float(values[j]);r['new_floor']=thresholds[mode];r.pop('answers',None)
        calib.append({'capsule':cap,'negative_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'thresholds':thresholds,'old_rejection':float((zs<c.floor).mean()),'hybrid_rejection':float((zscore(ns+.2*no)<thresholds['hybrid_z']).mean()),'absolute_rejection':float(((ns+.2*no).max(axis=1)<thresholds['absolute']).mean())})
        c.close()
        if ci%40==0:print('calibrated',ci,len(caps),flush=True)
    for a,b in zip(records['control'],base):assert (a['status'],a['passage'])==(b['status'],b['passage']),a['q']
    known={}
    for path in (ROOT/'var/claude_scratch').glob('answer_quality_q400*.json'):
        for r in json.loads(path.read_text()):
            if isinstance(r.get('answers'),bool) and r.get('passage'):known.setdefault((r['q'],r['passage'].strip()),r['answers'])
    for r in json.loads((ROOT/'var/claude_scratch/overlap_rank.json').read_text()):
        if isinstance(r.get('overlap_judged'),bool):known.setdefault((r['q'],r['overlap_passage'].strip()),r['overlap_judged'])
    for key,v in json.loads((ROOT/'var/claude_scratch/rlm/judged_pairs.json').read_text()).items():
        if isinstance(v,bool):q,p=key.split('\t',1);known.setdefault((q,p.strip()),v)
    cache=CACHE/'judgments.json'
    if cache.exists():known.update({tuple(k.split('\t',1)):v for k,v in json.loads(cache.read_text()).items()})
    need=sorted({(r['q'],r['passage'].strip()) for rows in records.values() for r in rows if r['status']=='OK'}-known.keys())
    print('new judgments',len(need),flush=True)
    sys='You rate short technical texts. Reply with exactly two items separated by a comma: a coherence score from 1 to 5, then YES or NO for whether the text actually answers the question. Nothing else.'
    def judge(pair):
        body={'model':'mistral-small-3.2-24b-offline','temperature':0,'max_tokens':8,'messages':[{'role':'system','content':sys},{'role':'user','content':f'Question: {pair[0]}\n\nText: {pair[1]}'}]}
        req=urllib.request.Request('http://127.0.0.1:8092/v1/chat/completions',data=json.dumps(body).encode(),headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(req,timeout=60) as f:answer=json.load(f)['choices'][0]['message']['content']
        m=re.fullmatch(r'\s*[1-5]\s*,\s*(YES|NO)\s*[.!]?\s*',answer,re.I)
        if not m:raise ValueError('invalid judge response '+answer)
        return pair,m[1].upper()=='YES'
    with ThreadPoolExecutor(max_workers=4) as ex:
        for pair,v in ex.map(judge,need):known[pair]=v;cache.write_text(json.dumps({'\t'.join(k):v for k,v in known.items()},indent=2)+'\n')
    output={'calibration':calib,'results':{},'records':records}
    for mode,rows in records.items():
        for r in rows:
            if r['status']=='OK':r['answers']=known[(r['q'],r['passage'].strip())]
        c=sum(r['status']=='OK' and r['answers'] for r in rows);w=sum(r['status']=='OK' and not r['answers'] for r in rows)
        summary={'answered':c+w,'correct':c,'wrong':w,'net':c-2*w,'rescued_correct':sum(a['status']!='OK' and b['status']=='OK' and b['answers'] for a,b in zip(base,rows)),'rescued_wrong':sum(a['status']!='OK' and b['status']=='OK' and not b['answers'] for a,b in zip(base,rows))}
        output['results'][mode]=summary;print(mode,summary,flush=True)
    (CACHE/'experiment.json').write_text(json.dumps(output,indent=2)+'\n')

if __name__=='__main__':main()
