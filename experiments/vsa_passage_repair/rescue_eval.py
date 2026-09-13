import os
os.environ['OPENBLAS_NUM_THREADS']='1'
import json,numpy as np
from probe import Native,CACHE
from boost import predict
from train import features
from evaluate import zscore

def main():
    native=Native();model=json.loads((CACHE/'model.json').read_text());baseline=json.loads((CACHE/'experiment.json').read_text())['records']['control'];learned=json.loads((CACHE/'learned_result.json').read_text())['records'];records=[dict(r) for r in baseline];cal=[]
    for name in sorted({r['cap'] for r in baseline if r['route']=='accept'}):
        cap=native.load(name);negs=json.loads((CACHE/'negatives'/f'{name}.json').read_text());co,ov=cap.scores(negs);rejected=zscore(co)<cap.floor
        x=np.concatenate([features(c,o,q,cap.texts) for c,o,q in zip(co,ov,negs)]);scores=predict(model,x).reshape(len(negs),cap.n).max(axis=1)
        floor=max(model['validation_floor'],float(np.nextafter(scores[rejected].max(),np.inf)))
        added=int((rejected & (scores>=floor)).sum());assert added==0
        cal.append({'capsule':name,'rescue_floor':floor,'baseline_rejected':int(rejected.sum()),'added_negative_accepts':added})
        for i,(b,l) in enumerate(zip(baseline,learned)):
            if b['cap']==name and b['route']=='accept' and b['status']=='REFUSE_PASSAGE' and l.get('learned_score',-99)>=floor:
                records[i]=dict(l);records[i]['rescue']=True;records[i]['rescue_floor']=floor
        cap.close()
    c=sum(r['status']=='OK' and r['answers'] for r in records);w=sum(r['status']=='OK' and not r['answers'] for r in records)
    summary={'answered':c+w,'correct':c,'wrong':w,'net':c-2*w,'rescued_correct':sum(r.get('rescue',False) and r['answers'] for r in records),'rescued_wrong':sum(r.get('rescue',False) and not r['answers'] for r in records)}
    print('RESCUE',summary,flush=True);(CACHE/'rescue_result.json').write_text(json.dumps({'summary':summary,'records':records,'calibration':cal},indent=2)+'\n')

if __name__=='__main__':main()
