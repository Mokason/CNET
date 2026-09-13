"""Fixed two-passage evidence control; identical route gates and pinned negatives."""
import os
os.environ['OPENBLAS_NUM_THREADS']='1'
import json,urllib.request,re
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from probe import Native,ROOT,CACHE
from evaluate import threshold

def composite(scores,pp):
    top=np.argsort(-scores,axis=1,kind='stable')[:,:8]
    rows=np.arange(len(scores));chosen=top[:,0].copy();other=chosen.copy();best=scores[rows,chosen].copy();original=best.copy()
    for a in range(8):
        for b in range(a+1,8):
            i=top[:,a];j=top[:,b]
            value=(scores[rows,i]+scores[rows,j])/np.sqrt(np.maximum(2+2*pp[i,j],1e-6))
            take=(value>best)&(value>=original+.05)&(pp[i,j]>=.15)
            best[take]=value[take];chosen[take]=i[take];other[take]=j[take]
    x=scores.astype(np.float64);mask=np.ones(scores.shape,bool);mask[rows,chosen]=False;mask[rows,other]=False
    n=mask.sum(axis=1);mean=(x*mask).sum(axis=1)/n;var=(((x-mean[:,None])**2)*mask).sum(axis=1)/(n-1)
    z=(best-mean)/np.sqrt(np.maximum(var,1e-12))
    return chosen,other,z.astype(np.float32)

def main():
    native=Native();base=json.loads((ROOT/'var/claude_scratch/answer_quality_q400_base.json').read_text());output=[dict(r) for r in base];cal=[]
    for name in sorted({r['cap'] for r in base if r['route']=='accept'}):
        cap=native.load(name);pp,_=cap.scores(cap.texts);negs=json.loads((CACHE/'negatives'/f'{name}.json').read_text());ns,_=cap.scores(negs);_,_,nz=composite(ns,pp);floor=max(1.,threshold(nz))
        cal.append({'capsule':name,'floor':floor,'rejection':float((nz<floor).mean())})
        for i,r in enumerate(base):
            if r['route']!='accept' or r['cap']!=name:continue
            qs,_=cap.scores([r['q']]);a,b,z=composite(qs,pp);ok=z[0]>=floor
            text=cap.texts[int(a[0])]+((' '+cap.texts[int(b[0])]) if a[0]!=b[0] else '')
            output[i].update({'status':'OK' if ok else 'REFUSE_PASSAGE','passage':text if ok else None,'pair':bool(a[0]!=b[0]),'pair_z':float(z[0]),'pair_floor':floor});output[i].pop('answers',None)
        cap.close()
    known={tuple(k.split('\t',1)):v for k,v in json.loads((CACHE/'judgments.json').read_text()).items()};need=[r for r in output if r['status']=='OK' and (r['q'],r['passage'].strip()) not in known];print('pair new judgments',len(need),flush=True)
    system='You rate short technical texts. Reply with exactly two items separated by a comma: a coherence score from 1 to 5, then YES or NO for whether the text actually answers the question. Nothing else.'
    def judge(r):
        pair=(r['q'],r['passage'].strip());body={'model':'mistral-small-3.2-24b-offline','temperature':0,'max_tokens':8,'messages':[{'role':'system','content':system},{'role':'user','content':f'Question: {pair[0]}\n\nText: {pair[1]}'}]};req=urllib.request.Request('http://127.0.0.1:8092/v1/chat/completions',data=json.dumps(body).encode(),headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(req,timeout=60) as f:text=json.load(f)['choices'][0]['message']['content']
        m=re.fullmatch(r'\s*[1-5]\s*,\s*(YES|NO)\s*[.!]?\s*',text,re.I)
        if not m:raise ValueError(text)
        return pair,m[1].upper()=='YES'
    with ThreadPoolExecutor(max_workers=2) as ex:
        for pair,v in ex.map(judge,need):known[pair]=v
    for r in output:
        if r['status']=='OK':r['answers']=known[(r['q'],r['passage'].strip())]
    c=sum(r['status']=='OK' and r['answers'] for r in output);w=sum(r['status']=='OK' and not r['answers'] for r in output)
    summary={'answered':c+w,'correct':c,'wrong':w,'net':c-2*w,'rescued_correct':sum(a['status']!='OK' and b['status']=='OK' and b['answers'] for a,b in zip(base,output)),'rescued_wrong':sum(a['status']!='OK' and b['status']=='OK' and not b['answers'] for a,b in zip(base,output))}
    print('PAIR',summary,flush=True)
    # Separate judgment artifact: avoid racing another label job's cache.
    (CACHE/'pair_judgments.json').write_text(json.dumps({'\t'.join(k):v for k,v in known.items()},indent=2)+'\n')
    (CACHE/'pair_result.json').write_text(json.dumps({'summary':summary,'records':output,'calibration':cal},indent=2)+'\n')

if __name__=='__main__':main()
