"""Compare independently produced blind labels; no candidate access or scoring."""
import hashlib,json,sys
from pathlib import Path

base=Path('/tmp/cnet-learned-confirmation-xAqJks4W')
version=sys.argv[1]
corpus=Path(sys.argv[2])
review=Path(sys.argv[3])
cases={c['id']:c for c in json.loads(corpus.read_bytes())['cases']}
view=json.loads((base/('blind-view-'+version+'.json')).read_bytes())
rows=json.loads(review.read_bytes())['reviews']
assert len(rows)==len(view)
assert len({r['id'] for r in rows})==len(rows)
assert {r['id'] for r in rows}=={c['id'] for c in view}
disagreements=[]
for r in rows:
    assert set(r)=={'id','status','operation','key','uncertain','rationale'}
    assert r['status'] in {'ready','clarify','abstain'}
    assert type(r['uncertain']) is bool
    assert isinstance(r['rationale'],str)
    if r['status']=='ready':
        assert r['operation'] in {'upper','lower'} and type(r['key']) is int and 0<=r['key']<=255
    else:
        assert r['operation'] is None and r['key'] is None
    c=cases[r['id']]
    mismatches=[k for k in ('status','operation','key') if r[k]!=c[k]]
    if mismatches or r['uncertain']:
        disagreements.append({'id':r['id'],'mismatched_fields':mismatches,'uncertain':r['uncertain'],'reviewer_label':{k:r[k] for k in ('status','operation','key')},'rationale':r['rationale']})
result={'schema':1,'corpus_path':str(corpus),'corpus_sha256':hashlib.sha256(corpus.read_bytes()).hexdigest(),'review_path':str(review),'review_sha256':hashlib.sha256(review.read_bytes()).hexdigest(),'reviewed_count':len(rows),'agreement_count':len(rows)-len(disagreements),'issues':disagreements,'no_score':True}
with (base/('review-comparison-'+version+'.json')).open('x') as out:
    json.dump(result,out,indent=2);out.write('\n')
print(json.dumps(result))
