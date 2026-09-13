"""Compare frozen full CLI decisions/passages; report warm latency, never assert a speed pass."""
import json,re,subprocess,hashlib
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[2]
CACHE=ROOT/'var/passage_repair_20260913'
def run(binary,queries,label):
    result=subprocess.run([str(binary),'answer-batch',str(ROOT/'bin'),'1'],input='\n'.join(queries)+'\n',capture_output=True,text=True,check=True)
    (CACHE/(label+'.txt')).write_text(result.stdout)
    chunks=re.split(r'=== QUERY \d+ ===\n',result.stdout)[1:]
    assert len(chunks)==len(queries)
    stable=[re.sub(r'cold=\d+ route_us=[\d.]+ rank_us=[\d.]+','TIMING',c.strip()) for c in chunks]
    times=np.array([list(map(float,re.search(r'route_us=([\d.]+) rank_us=([\d.]+)',c).groups())) for c in chunks])
    return stable,times

def main():
    source=ROOT/'benchmarks/vsa_routing_arena_20260911/questions.tsv'
    qs=[l.split('\t')[1] for l in source.read_text().splitlines() if l and not l.startswith('#')]
    aliens=(source.parent/'queries_alien.txt').read_text().splitlines()
    qs+= [q for q in aliens if q and not q.startswith('#')]
    # Force token-cap boundary cases through the real caller, including stopwords.
    for length in (124,125,126,249,250,251):
        qs.append(' '.join('the' if i%5==3 else ('pressure' if i%2 else 'temperature') for i in range(length)))
    old,_=run(CACHE/'baseline_cli',qs,'parity_before')
    new,_=run(ROOT/'bin/cnet_vsa_cli',qs,'parity_after')
    assert old==new,[(i,a,b) for i,(a,b) in enumerate(zip(old,new)) if a!=b][:2]
    report={'parity_queries':len(qs),'question_source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'unchanged':True,'runs':[]}
    warm=[r['q'] for r in json.loads((ROOT/'var/claude_scratch/answer_quality_q400_base.json').read_text())]
    for name,binary in [('before',CACHE/'baseline_cli'),('after',ROOT/'bin/cnet_vsa_cli')]:
        stable,t=run(binary,warm*2,'final_warm_'+name);t=t[len(warm):];total=t.sum(axis=1)
        report['runs'].append({'name':name,'route_p50_us':float(np.median(t[:,0])),'route_p95_us':float(np.percentile(t[:,0],95)),'total_p50_us':float(np.median(total)),'total_p95_us':float(np.percentile(total,95)),'max_us':float(total.max()),'binary_bytes':binary.stat().st_size})
    report['meets_25us_limit']=report['runs'][-1]['max_us']<=25
    (CACHE/'runtime_verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
