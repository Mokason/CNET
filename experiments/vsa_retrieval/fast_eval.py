"""Measure the complete native query call and strict exposed-set accuracy."""
import os
os.environ['OPENBLAS_NUM_THREADS']='1'
import argparse,json,time
import numpy as np
from retrieval import ROOT,CACHE,load_corpora
from run import questions,EXTRAS,metrics
from fast import load_fast


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--terms',type=int,choices=[256,1024],default=256);args=ap.parse_args()
    engine=load_fast(args.terms)
    fixture=ROOT/'benchmarks/vsa_routing_arena_20260911'
    names,_,_,tests,gold=load_corpora(fixture/'corpora.tsv')
    blocks={'test_sentences':(tests,gold,[{int(g)} for g in gold]),'questions':questions(fixture/'questions.tsv',names)}
    blocks.update({'questions_extra:'+n:questions(fixture/n,names) for n in EXTRAS+['questions_indep_mistral_20260912.tsv']})
    output={'preset_terms':args.terms,'runtime_data_bytes':engine.receipt['runtime_data_bytes'],'results':{},'timing_scope':'one query at a time: UTF-8 encoding, CNET tokenization, scoring, top20, ctypes and result arrays; excludes loading and JSON serialization','limit_us':25}
    for mode,label in [(0,'semantic'),(1,'fusion')]:
        result={};times=[];over=[]
        for key,(texts,gold,valid) in blocks.items():
            score=np.empty((len(texts),len(names)),np.float32)
            for i,text in enumerate(texts):
                start=time.perf_counter_ns();ids,values=engine.query(text,mode);elapsed=(time.perf_counter_ns()-start)/1000
                times.append(elapsed)
                if elapsed>25:over.append({'block':key,'row':i,'us':elapsed,'bytes':len(text.encode())})
                score[i]=engine.scores(text,mode)
                ranked=np.argsort(-score[i],kind='stable')[:20];ranked=ranked[score[i,ranked]>0]
                if not np.array_equal(ids,ranked):raise AssertionError('native rank differs')
            m=metrics(score,gold,valid);m.pop('lenient1',None);result[key]=m
        output['results'][label]={'metrics':result,'latency':{'n':len(times),'p50_us':float(np.median(times)),'p95_us':float(np.percentile(times,95)),'p99_us':float(np.percentile(times,99)),'max_us':max(times),'over_25_us':len(over)},'slowest':sorted(over,key=lambda x:-x['us'])[:20]}
        print(args.terms,label,output['results'][label]['latency'],[round(v['top1']*100,2) for v in result.values()],flush=True)
    (CACHE/f'fast{args.terms}_results.json').write_text(json.dumps(output,indent=2)+'\n')

if __name__=='__main__':main()
