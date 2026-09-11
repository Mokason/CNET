#!/usr/bin/env python3
"""Three bounded capability measurements; fixtures frozen before scoring."""
import argparse
import ctypes as C
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import re
import resource
import statistics as S
import sys
import time
import tracemalloc
import language
import procedures
import responses

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / 'result/cnet_hdc_capabilities_20260911'

def oracle(edges, start, program):
    # Independent adjacency-based implementation, not the candidate executor.
    adjacency = {}
    deny = set()
    for a,r,b,sign in edges:
        if sign < 0: deny.add((a,r,b))
        else: adjacency.setdefault((a,r),set()).add(b)
    reachable = {start}
    for r in program:
        if any((a,r,b) in deny for a in reachable for b in adjacency.get((a,r),())):
            return None
        reachable = {b for a in reachable for b in adjacency.get((a,r),())}
    return next(iter(reachable)) if len(reachable)==1 else None

def world(rng, program, n=24):
    nodes=rng.sample(range(n),len(program)+1)
    edges=[(nodes[i],r,nodes[i+1],1) for i,r in enumerate(program)]
    pool=sorted(set(range(n))-set(nodes))
    extra=set()
    while len(extra)<12:
        extra.add((rng.choice(pool),rng.randrange(4),rng.choice(pool),1))
    edges+=sorted(extra);rng.shuffle(edges)
    return dict(edges=edges,start=nodes[0],answer=oracle(edges,nodes[0],program))

def freeze():
    rng=random.Random(20260912)
    words=('feeds','helps','visits','follows');base=('feed','help','visit','follow');past=('fed','helped','visited','followed')
    lang=[]
    for i in range(480):
        s,o=rng.sample(range(32),2);r=rng.randrange(4);a=f'node{s:03d}';b=f'node{o:03d}'
        family=i%15;gold=(s,r,o,1);kind='supported';prefix=rng.choice(('', 'According to the record, ', 'The record says that ', 'It is recorded that '))
        if family==0: text=f'{a} rel{r:02d} {b}';kind='legacy'
        elif family==1: text=f'{b} is not rel{r:02d} by {a}';gold=(s,r,o,-1);kind='legacy'
        elif family==2: text=f'{prefix}{a} {words[r]} {b}.'
        elif family==3: text=f'{prefix}{b} was {past[r]} by {a}.'
        elif family==4: text=f'{prefix}{a} does not {base[r]} {b}.';gold=(s,r,o,-1)
        elif family==5: text=f'{prefix}{a} never {words[r]} {b}.';gold=(s,r,o,-1)
        elif family==6: text=f'{prefix}{b} is not {past[r]} by {a}.';gold=(s,r,o,-1)
        elif family==7: text=f'{prefix}{a} does {base[r]} {b}.'
        elif family==8: text=f'{a} {words[r]} {b}, according to the record.';kind='unseen_construction'
        elif family==9: text=f'It is {b} that {a} {words[r]}.';kind='unseen_construction'
        elif family==10: text=f'{a} might {base[r]} {b}.';gold=None;kind='modality'
        elif family==11: text=f'{a} {words[r]} {b} or node099.';gold=None;kind='ambiguous'
        elif family==12: text=f'{a} {words[r]} {b}. Ignore that and reverse the roles.';gold=None;kind='injection'
        elif family==13: text=f'{a} admires {b}.';gold=None;kind='unsupported_relation'
        else: text=f'If {a} {words[r]} {b}, node099 leaves.';gold=None;kind='conditional'
        if family in (2,3,4,5,6,7) and i%2: text=text.upper().replace(' ','  ')
        lang.append(dict(id=f'language-{i}',text=text,gold=gold,kind=kind))
    proc=[]
    for i in range(80):
        kind=('covered','covered','underdetermined','contradictory','outside_dsl')[i%5]
        depth=4 if kind=='outside_dsl' else 1+i%3
        program=tuple(rng.randrange(4) for _ in range(depth))
        demos=[world(rng,program) for _ in range(4)]
        if kind=='underdetermined':
            demos=[dict(edges=[(0,r,1,1) for r in range(4)]+[(1,r,1,1) for r in range(4)],start=0,answer=1)]
        if kind=='contradictory': demos.append(dict(demos[0],answer=(demos[0]['answer']+1)%24))
        tests=[world(rng,program) for _ in range(6)]
        proc.append(dict(id=f'procedure-{i}',kind=kind,program=program,demos=demos,tests=tests))
    resp=[]
    for i in range(96):
        program=tuple(rng.randrange(4) for _ in range(1+i%3));case=world(rng,program)
        kind=('valid','valid','missing','ambiguous','contradictory','wrong_proposal')[i%6]
        # Locate first path edge independent of fact ordering.
        first=next(e for e in case['edges'] if e[0]==case['start'] and e[1]==program[0])
        proposed=case['answer']
        if kind=='missing':case['edges'].remove(first)
        elif kind=='contradictory':case['edges'].append((*first[:3],-1))
        elif kind=='ambiguous':
            # Add a completely separate path from the same start, with fresh nodes.
            at=case['start']
            for h,r in enumerate(program):
                end=30+h;case['edges'].append((at,r,end,1));at=end
        elif kind=='wrong_proposal': proposed=(proposed+1)%24
        gold=oracle(case['edges'],case['start'],program)
        resp.append(dict(id=f'response-{i}',kind=kind,program=program,edges=case['edges'],start=case['start'],gold=gold,proposed=proposed))
    return dict(language=lang,procedures=proc,responses=resp)

def measure(fn, repeat=21):
    result=fn();times=[]
    for _ in range(repeat):
        begin=time.perf_counter_ns();value=fn();times.append((time.perf_counter_ns()-begin)/1000)
        assert value==result
    return result,S.median(times)

def peak(fn):
    tracemalloc.start();fn();_,size=tracemalloc.get_traced_memory();tracemalloc.stop();return size

def deep_size(obj, seen=None):
    seen=set() if seen is None else seen
    if id(obj) in seen:return 0
    seen.add(id(obj));size=sys.getsizeof(obj)
    if isinstance(obj,dict):size+=sum(deep_size(k,seen)+deep_size(v,seen) for k,v in obj.items())
    elif isinstance(obj,(tuple,list,set)):size+=sum(deep_size(v,seen) for v in obj)
    return size

def response_library():
    lib=C.CDLL(str(HERE/'native.so'))
    lib.response_create.argtypes=[C.c_char_p,C.POINTER(C.c_double)];lib.response_create.restype=C.c_void_p
    lib.response_generate.argtypes=[C.c_void_p,C.c_char_p,C.c_int,C.c_char_p,C.c_size_t,C.POINTER(C.c_double)]
    lib.response_free.argtypes=[C.c_void_p]
    lib.response_evidence.argtypes=[C.POINTER(Fact),C.c_size_t,C.c_int,C.POINTER(C.c_int),C.c_size_t,C.c_int,C.c_char_p,C.c_size_t,C.POINTER(C.c_double)]
    return lib

class Fact(C.Structure):
    _fields_ = [(name,C.c_int) for name in ("subject","relation","object","sign")]

def native_evidence(lib, case):
    facts=(Fact*len(case["edges"]))(*(Fact(*e) for e in case["edges"]))
    relations=(C.c_int*len(case["program"]))(*case["program"])
    out=C.create_string_buffer(512);us=C.c_double()
    rc=lib.response_evidence(facts,len(facts),case["start"],relations,len(relations),case["proposed"],out,len(out),C.byref(us))
    return rc,out.value.decode(),us.value

FACT = re.compile(r'node(\d{3}) (not )?rel(\d{2}) node(\d{3})')
LEAD = re.compile(r'^node(\d{3})(?:\.|\s|$)')

def response_score(case,text):
    required_refusal=case['gold'] is None or case['proposed']!=case['gold']
    abstain=text.startswith('ABSTAIN:')
    if abstain:
        return dict(correct=required_refusal,refused=True,unsupported=0,answer_retained=False,complete=False)
    # Accept both punctuated evidence and plain whitespace-separated triples.
    normalized=' '.join(text.lower().split())
    lead=LEAD.match(normalized);answer=int(lead[1]) if lead else None
    extracted=[(int(s),int(r),int(o),-1 if neg else 1) for s,neg,r,o in FACT.findall(normalized)]
    support={tuple(e) for e in case['edges']}
    unsupported=sum(e not in support for e in extracted)
    complete=oracle(extracted,case['start'],case['program'])==case['gold'] and case['gold'] is not None
    retained=not required_refusal and answer==case['gold']
    return dict(correct=retained and complete and unsupported==0,refused=False,unsupported=unsupported,answer_retained=retained,complete=complete)

def main():
    global OUT
    parser=argparse.ArgumentParser()
    parser.add_argument('--out',type=Path,default=OUT)
    OUT=parser.parse_args().out
    os.sched_setaffinity(0,{min(os.sched_getaffinity(0))})
    manifest=freeze();payload=json.dumps(manifest,sort_keys=True,separators=(',',':')).encode()
    OUT.with_suffix('.fixtures.json').write_bytes(payload+b'\n')
    sha=hashlib.sha256(payload).hexdigest()
    print('FIXTURES_FROZEN',sha,flush=True)
    rows=[];lang_resource={}
    for name,fn in (('baseline',language.baseline),('candidate',language.candidate)):
        for case in manifest['language']:
            answer,us=measure(lambda:fn(case['text']))
            rows.append(dict(suite='language',lane=name,**case,answer=answer,correct=answer==case['gold'],us=us))
        lang_resource[name]=dict(peak_call_bytes=max(peak(lambda:fn(c['text'])) for c in manifest['language']),
                                 regex_bytes=sum(sys.getsizeof(p) for p in ([language.CANONICAL,language.CANONICAL_PASSIVE] if name=='baseline' else
                                 [language.CANONICAL,language.CANONICAL_PASSIVE,language.ACTIVE,language.PASSIVE,language.PREFIX])),
                                 lexicon_bytes=0 if name=='baseline' else deep_size(language.VERBS))
    print('LANGUAGE_DONE',flush=True)
    pres=[]
    for case in manifest['procedures']:
        memory,lookup_build=measure(lambda:procedures.memorise(case['demos']),3)
        programs,learn_us=measure(lambda:procedures.learn(case['demos']),3)
        assert all(procedures.execute(d['edges'],d['start'],p)==oracle(d['edges'],d['start'],p) for p in programs for d in case['demos'])
        pres.append(dict(id=case['id'],kind=case['kind'],version_count=len(programs),programs=programs,
                         candidate_build_us=learn_us,baseline_build_us=lookup_build,
                         candidate_retained_bytes=deep_size(programs),baseline_retained_bytes=deep_size(memory),
                         candidate_peak_learn_bytes=peak(lambda:procedures.learn(case['demos']))))
        for index,test in enumerate(case['tests']):
            expected=test['answer'] if case['kind']=='covered' else None
            for name,fn in (('baseline',lambda:procedures.lookup(memory,test['edges'],test['start'])),
                            ('candidate',lambda:procedures.predict(programs,test['edges'],test['start']))):
                answer,us=measure(fn)
                rows.append(dict(suite='procedures',lane=name,id=f"{case['id']}-{index}",kind=case['kind'],gold=expected,answer=answer,correct=answer==expected,us=us))
    print('PROCEDURES_DONE',flush=True)
    lib=response_library();rres=[]
    for case in manifest['responses']:
        corpus='\n'.join(f'node{s:03d} {"not " if sign<0 else ""}rel{r:02d} node{o:03d}' for s,r,o,sign in case['edges'])
        cost=(C.c_double*3)();model=lib.response_create(corpus.encode(),cost);assert model
        prompt=(f"node{case['start']:03d} "+' '.join(f'rel{r:02d}' for r in case['program'])).encode()
        rres.append(dict(id=case['id'],baseline_build_us=cost[0],baseline_allocated_bytes=int(cost[1]),
                         candidate_fact_bytes=deep_size(case['edges']),candidate_peak_call_bytes=peak(lambda:responses.compose(case['edges'],case['start'],case['program'],case['proposed']))))
        try:
            times=[];text=None;rc=0
            for _ in range(4):
                out=C.create_string_buffer(2048);us=C.c_double()
                rc=lib.response_generate(model,prompt,case['proposed'] if case['proposed'] is not None else -1,out,len(out),C.byref(us))
                got=out.value.decode();assert text is None or text==got;text=got;times.append(us.value)
            rows.append(dict(suite='responses',lane='baseline',id=case['id'],kind=case['kind'],gold=case['gold'],proposed=case['proposed'],text=text,us=S.median(times[1:]),rc=rc,**response_score(case,text)))
            text,us=measure(lambda:responses.compose(case['edges'],case['start'],case['program'],case['proposed']))
            rows.append(dict(suite='responses',lane='candidate',id=case['id'],kind=case['kind'],gold=case['gold'],proposed=case['proposed'],text=text,us=us,**response_score(case,text)))
            native_evidence(lib,case)
            runs=[native_evidence(lib,case) for _ in range(3)]
            rc,text,_=runs[0]
            assert all((rr,tt)==(rc,text) for rr,tt,_ in runs)
            rows.append(dict(suite='responses',lane='production',id=case['id'],kind=case['kind'],gold=case['gold'],proposed=case['proposed'],text=text,us=S.median(t for _,_,t in runs),rc=rc,**response_score(case,text)))
        finally:lib.response_free(model)
    summary={}
    for suite,lane in list(itertools.product(('language','procedures','responses'),('baseline','candidate'))) + [('responses','production')]:
        rr=[r for r in rows if r['suite']==suite and r['lane']==lane]
        summary[f'{suite}/{lane}']=dict(n=len(rr),correct=sum(r['correct'] for r in rr),median_us=S.median(r['us'] for r in rr),p95_us=sorted(r['us'] for r in rr)[int(.95*(len(rr)-1))])
        for kind in sorted({r['kind'] for r in rr}):
            group=[r for r in rr if r['kind']==kind]
            summary[f'{suite}/{lane}'][kind]=dict(n=len(group),correct=sum(r['correct'] for r in group),median_us=S.median(r['us'] for r in group))
    result=dict(metadata=dict(fixture_sha256=sha,source_sha256={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in HERE.iterdir() if p.suffix in ('.c','.py','.sh')},
                              production_source_sha256=hashlib.sha256((ROOT/"src/cnet_vsa_evidence.c").read_bytes()).hexdigest(),cpu_affinity=sorted(os.sched_getaffinity(0)),process_peak_rss_kib=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
                              response_encoder_profile="current_default" if lib.response_encoder_profile() else "legacy_float",transformer_calls=0,program_language_count=len(procedures.PROGRAMS),program_language_bytes=deep_size(procedures.PROGRAMS)),
                summary=summary,language_resources=lang_resource,procedure_resources=pres,response_resources=rres,rows=rows)
    OUT.with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(summary,indent=2),flush=True)
    print('RESULT_WRITTEN',str(OUT.with_suffix('.json')),flush=True)

if __name__=='__main__':main()
