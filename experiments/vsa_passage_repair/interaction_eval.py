"""Fixed richer-feature candidate: capsule-disjoint validation before exposed-set work."""
import os
os.environ['OPENBLAS_NUM_THREADS']='1'
import json,hashlib,time,math
import numpy as np
from probe import Native,CACHE
from interaction import Interaction,OUT
from train import features
from boost import fit,predict
from evaluate import threshold

def split(rows):
    train=[];valid=[]
    for row in rows:(valid if int(hashlib.sha256(row['capsule'].encode()).hexdigest(),16)%5==0 else train).append(row)
    return train,valid

def assess(rows):
    train,valid=split(rows)
    x=np.array([f for r in train for f in r['features']],np.float32);y=np.array([v for r in train for v in r['labels']])
    model=fit(x,y);vx=np.array([f for r in valid for f in r['features']],np.float32);vy=np.array([v for r in valid for v in r['labels']])
    vs=predict(model,vx).astype(np.float32);floor=max(math.log(.67/.33),threshold(vs[~vy]));correct=wrong=0
    for row in valid:
        scores=predict(model,row['features']);i=int(scores.argmax())
        if scores[i]>=floor:
            if row['labels'][i]:correct+=1
            else:wrong+=1
    stats={'validation_capsules':len(valid),'validation_pairs':len(vy),'pair_correct':int(((vs>=floor)&vy).sum()),'pair_wrong':int(((vs>=floor)&~vy).sum()),'top1_correct':correct,'top1_wrong':wrong,'top1_net':correct-2*wrong,'floor':float(floor)}
    model['validation_floor']=float(floor)
    return model,stats

def main():
    native=Native();matcher=Interaction(native);source=CACHE/'training_labels.json';data=json.loads(source.read_text());rows=data['rows'];new=[];wall=[];cpu=[]
    for i,row in enumerate(rows):
        cap=native.load(row['capsule']);matcher.prepare(cap.texts)
        begin=time.perf_counter_ns();cpu_begin=time.thread_time_ns()
        co,ov=cap.scores([row['question']]);f=np.column_stack([features(co[0],ov[0],row['question'],cap.texts),matcher.score(row['question'])])
        cpu.append((time.thread_time_ns()-cpu_begin)/1000);wall.append((time.perf_counter_ns()-begin)/1000)
        new.append(dict(row,features=f[row['indices']].tolist()));cap.close()
        if i%100==0:print('FEATURES',i,flush=True)
    (OUT/'interaction_labels.json').write_text(json.dumps({'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'rows':new},indent=2)+'\n')
    report={'candidates':{},'component_latency_us':{f'p{p}':float(np.percentile(wall,p)) for p in (50,95,99)},'component_mean_cpu_us':float(np.mean(cpu)),'component_max_us':float(max(wall)),'projected_word_bytes':matcher.vectors.nbytes,'scope':'warm passage feature computation; excludes route, trees, assembly and cache preparation'}
    for name,candidate in [('baseline12',rows),('alignment16',json.loads((CACHE/'training_labels_late.json').read_text())['rows']),('interaction37',new)]:
        model,stats=assess(candidate);report['candidates'][name]=stats
        (OUT/(name+'_model.json')).write_text(json.dumps(model,indent=2)+'\n');print(name,stats,flush=True)
    old=report['candidates']['alignment16'];newstat=report['candidates']['interaction37']
    report['validation_improved']=newstat['top1_correct']>old['top1_correct'] and newstat['top1_net']>old['top1_net']
    (OUT/'interaction_validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
