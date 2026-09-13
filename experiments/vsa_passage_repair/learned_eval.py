"""Fit on separate teacher labels; qualify only on capsule-held-out training validation."""
import os
os.environ['OPENBLAS_NUM_THREADS']='1'
import json,hashlib,math,urllib.request,re,sys
from concurrent.futures import ThreadPoolExecutor,as_completed
import numpy as np
from probe import Native,ROOT,CACHE
from train import features
from boost import fit,predict
from evaluate import threshold

def main():
    use_late='--late' in sys.argv
    labels_path=CACHE/('training_labels_late.json' if use_late else 'training_labels.json')
    labeled=json.loads(labels_path.read_text())['rows']
    train=[];valid=[]
    for row in labeled:
        (valid if int(hashlib.sha256(row['capsule'].encode()).hexdigest(),16)%5==0 else train).append(row)
    def arrays(rows):return np.array([f for r in rows for f in r['features']],np.float32),np.array([v for r in rows for v in r['labels']],np.int32)
    x,y=arrays(train);vx,vy=arrays(valid);model=fit(x,y);vs=predict(model,vx).astype(np.float32)
    floor=max(math.log(.67/.33),threshold(vs[vy==0]))
    model.update({'validation_floor':floor,'training_capsules':len(train),'validation_capsules':len(valid),'training_pairs':len(y),'validation_pairs':len(vy),'training_labels_sha256':hashlib.sha256(labels_path.read_bytes()).hexdigest()})
    (CACHE/('model_late.json' if use_late else 'model.json')).write_text(json.dumps(model,indent=2)+'\n')
    print('TRAIN',len(train),len(y),'VALID',len(valid),len(vy),'floor',floor,'accepted',int((vs>=floor).sum()),'correct',int(((vs>=floor)&(vy==1)).sum()),flush=True)
    base=json.loads((ROOT/'var/claude_scratch/answer_quality_q400_base.json').read_text());result=[dict(r) for r in base]
    native=Native();cal=[]
    if use_late:
        from late import Late,extend
        aligner=Late(native)
    def featurize(c,o,q,texts):
        f=features(c,o,q,texts)
        return extend(f,*aligner.score(q)) if use_late else f
    for ci,capname in enumerate(sorted({r['cap'] for r in base if r['route']=='accept'})):
        cap=native.load(capname)
        if use_late:aligner.prepare(cap.texts)
        negs=json.loads((CACHE/'negatives'/f'{capname}.json').read_text());co,ov=cap.scores(negs)
        f=np.concatenate([featurize(c,o,q,cap.texts) for c,o,q in zip(co,ov,negs)])
        ns=predict(model,f).reshape(len(negs),cap.n).max(axis=1).astype(np.float32)
        capfloor=max(floor,threshold(ns));cal.append({'capsule':capname,'floor':capfloor,'rejection':float((ns<capfloor).mean())})
        for i,r in enumerate(base):
            if r['route']!='accept' or r['cap']!=capname:continue
            c,o=cap.scores([r['q']]);s=predict(model,featurize(c[0],o[0],r['q'],cap.texts));j=int(s.argmax());ok=s[j]>=capfloor
            result[i].update({'status':'OK' if ok else 'REFUSE_PASSAGE','passage':cap.texts[j] if ok else None,'learned_score':float(s[j]),'learned_floor':capfloor})
            result[i].pop('answers',None)
        cap.close()
        if ci%50==0:print('calibrated learned',ci,flush=True)
    known={tuple(k.split('\t',1)):v for k,v in json.loads((CACHE/'judgments.json').read_text()).items()}
    need=[r for r in result if r['status']=='OK' and (r['q'],r['passage'].strip()) not in known]
    print('need judgments',len(need),flush=True)
    system='You rate short technical texts. Reply with exactly two items separated by a comma: a coherence score from 1 to 5, then YES or NO for whether the text actually answers the question. Nothing else.'
    def judge(row):
        body={'model':'mistral-small-3.2-24b-offline','temperature':0,'max_tokens':8,'messages':[{'role':'system','content':system},{'role':'user','content':f"Question: {row['q']}\n\nText: {row['passage']}"}]}
        req=urllib.request.Request('http://127.0.0.1:8092/v1/chat/completions',data=json.dumps(body).encode(),headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(req,timeout=60) as f:text=json.load(f)['choices'][0]['message']['content']
        m=re.fullmatch(r'\s*[1-5]\s*,\s*(YES|NO)\s*[.!]?\s*',text,re.I)
        if not m:raise ValueError(text)
        return (row['q'],row['passage'].strip()),m[1].upper()=='YES'
    with ThreadPoolExecutor(max_workers=2) as ex:
        for begin in range(0,len(need),2):
            for future in as_completed([ex.submit(judge,r) for r in need[begin:begin+2]]):
                pair,v=future.result();known[pair]=v
                (CACHE/'judgments.json').write_text(json.dumps({'\t'.join(k):v for k,v in known.items()},indent=2)+'\n')
    for r in result:
        if r['status']=='OK':r['answers']=known[(r['q'],r['passage'].strip())]
    c=sum(r['status']=='OK' and r['answers'] for r in result);w=sum(r['status']=='OK' and not r['answers'] for r in result)
    summary={'answered':c+w,'correct':c,'wrong':w,'net':c-2*w,'rescued_correct':sum(a['status']!='OK' and b['status']=='OK' and b['answers'] for a,b in zip(base,result)),'rescued_wrong':sum(a['status']!='OK' and b['status']=='OK' and not b['answers'] for a,b in zip(base,result))}
    print('LEARNED',summary,flush=True)
    (CACHE/('learned_late_result.json' if use_late else 'learned_result.json')).write_text(json.dumps({'summary':summary,'records':result,'calibration':cal},indent=2)+'\n')

if __name__=='__main__':main()
