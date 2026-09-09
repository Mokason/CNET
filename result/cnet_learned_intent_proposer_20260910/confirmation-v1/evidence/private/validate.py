"""Custodian-only structural/overlap admission checks; never scores or runs CNET."""
import hashlib,itertools,json,re,sys,unicodedata
from collections import Counter
from pathlib import Path

ROOT=Path('/home/marble/AI/CNET-worktrees/herdr-language-20260909')
PRIVATE=Path('/tmp/cnet-learned-confirmation-xAqJks4W')
def sha(raw): return hashlib.sha256(raw).hexdigest()
def norm(text): return ''.join(c for c in text.casefold() if not c.isspace() and not unicodedata.category(c).startswith('P'))
def hashtext(text): return sha(text.encode('utf-8',errors='surrogatepass'))
def save(path,value):
    with path.open('x',encoding='utf-8') as out:
        json.dump(value,out,ensure_ascii=True,indent=2)
        out.write('\n')

corpus=Path(sys.argv[1])
version=sys.argv[2]
raw=corpus.read_bytes()
# Only the authorized strict loader and its dependencies, never scorer/runner.
with (ROOT/'tools/task_paraphrase_eval/evaluate.py').open() as source:
    validator=''.join(itertools.islice(source,75))
scope={}
exec(compile(validator,'approved_load_corpus','exec'),scope)
cases=scope['load_corpus'](raw,sha(raw),'confirmation')
old_paths=set()
for name in ('freeze.json','confirmation.json','qualification.json'):
    old_paths.update(ROOT.glob('benchmarks/task_paraphrases*/'+name))
old_paths.update(ROOT.glob('result/cnet_herdr_three_heads_20260909/exposed-corpora/*.json'))
inventory=[]; prior_exact=set(); prior_normal=set(); prior_count=0
for path in sorted(old_paths):
    oldraw=path.read_bytes(); obj=json.loads(oldraw)
    oldcases=obj.get('cases',[]) if isinstance(obj,dict) else []
    texts=[c['text'] for c in oldcases if isinstance(c,dict) and isinstance(c.get('text'),str)]
    if texts and len(texts)!=128: raise ValueError('unexpected_previous_corpus_count')
    inventory.append({'path':str(path.relative_to(ROOT)),'sha256':sha(oldraw),'texts':len(texts)})
    prior_count+=len(texts)
    prior_exact.update(hashtext(t) for t in texts)
    prior_normal.update(hashtext(norm(t)) for t in texts)

seen_exact={}; seen_normal={}; collisions=[]; scalar_issues=[]; row_hashes={}
for c in cases:
    eid=hashtext(c['text']); nid=hashtext(norm(c['text']))
    for kind,digest,seen,prior in [('exact',eid,seen_exact,prior_exact),('normalized',nid,seen_normal,prior_normal)]:
        if digest in seen: collisions.append({'id':c['id'],'kind':'internal_'+kind,'other_new_id':seen[digest]})
        if digest in prior: collisions.append({'id':c['id'],'kind':'prior_'+kind})
        seen[digest]=c['id']
    row_hashes[c['id']]=sha(json.dumps(c,sort_keys=True,ensure_ascii=True,separators=(',',':')).encode())
    text=c['text']; n16=len(text.encode('utf-16-le',errors='surrogatepass'))//2
    forbidden=any(unicodedata.category(ch) in {'Cc','Cs'} for ch in text) or n16>256
    if forbidden and c['status']!='abstain': scalar_issues.append({'id':c['id'],'issue':'request_control_surrogate_or_length'})
    if c['status']=='ready':
        if unicodedata.category(chr(c['key'])) in {'Cc','Cs'}: scalar_issues.append({'id':c['id'],'issue':'control_key'})
        values=[]
        values.extend(int(x,16) for x in re.findall(r'\bU\+([0-9a-fA-F]{2,6})\b',text))
        values.extend(int(x) for x in re.findall(r'\bcodepoint\s+([0-9]+)\b',text,re.I))
        values.extend(ord(m.group(2)) for m in re.finditer(r'([\x22\x27])([^\x22\x27])\1',text))
        if values and c['key'] not in values: scalar_issues.append({'id':c['id'],'issue':'explicit_operand_key_mismatch'})

report={'schema':1,'corpus_path':str(corpus),'corpus_sha256':sha(raw),'strict_loader':'pass','strict_loader_source_prefix_sha256':sha(validator.encode()),'counts':dict(Counter(c['status'] for c in cases)),'operations':dict(Counter(c['operation'] or 'null' for c in cases)),'family_count':len({c['family'] for c in cases}),'prior_inventory':inventory,'prior_corpus_file_count':sum(i['texts']==128 for i in inventory),'prior_text_count':prior_count,'prior_unique_exact_hashes':len(prior_exact),'prior_unique_normalized_hashes':len(prior_normal),'overlap_issues':collisions,'scalar_issues':scalar_issues,'row_hashes':row_hashes,'training_calibration_check':'pending_hash_only_denylist','no_score':True}
if len(sys.argv)>3:
    denyfile=Path(sys.argv[3]); deny=json.loads(denyfile.read_bytes())
    deny_exact={h for group in deny['groups'].values() for h in group['exact_sha256']}
    deny_normal={h for group in deny['groups'].values() for h in group['normalized_sha256']}
    for c in cases:
        for field,digest,hashes in [('exact',hashtext(c['text']),deny_exact),('normalized',hashtext(norm(c['text'])),deny_normal)]:
            if digest in hashes: report['overlap_issues'].append({'id':c['id'],'kind':'training_calibration_'+field})
    report['training_calibration_check']={'path':str(denyfile),'sha256':sha(denyfile.read_bytes()),'metadata':{k:v for k,v in deny.items() if k!='groups'},'groups':{k:{'text_count':g['count'],'exact_hash_count':len(g['exact_sha256']),'normalized_hash_count':len(g['normalized_sha256'])} for k,g in deny['groups'].items()},'exact_hash_count':len(deny_exact),'normalized_hash_count':len(deny_normal)}
devfile=PRIVATE/'development-denylist.json'
dev=json.loads(devfile.read_bytes())
for c in cases:
    for field,digest in [('exact_sha256',hashtext(c['text'])),('normalized_sha256',hashtext(norm(c['text'])))]:
        if digest in set(dev[field]): report['overlap_issues'].append({'id':c['id'],'kind':'development_'+field})
report['development_overlap_check']={'path':str(devfile),'sha256':sha(devfile.read_bytes()),'metadata':{k:v for k,v in dev.items() if k not in {'exact_sha256','normalized_sha256'}},'exact_hash_count':len(dev['exact_sha256']),'normalized_hash_count':len(dev['normalized_sha256'])}
save(PRIVATE/('validation-'+version+'.json'),report)
view=[{'id':c['id'],'text':c['text']} for c in cases]
if len(sys.argv)>4:
    before=json.loads(Path(sys.argv[4]).read_bytes())['cases']
    before={c['id']:c for c in before}
    changed=[c['id'] for c in cases if c!=before.get(c['id'])]
    view=[c for c in view if c['id'] in changed]
    save(PRIVATE/('changes-'+version+'.json'),{'changed_ids':changed,'unchanged_hashes':{c['id']:row_hashes[c['id']] for c in cases if c['id'] not in changed}})
save(PRIVATE/('blind-view-'+version+'.json'),view)
print(json.dumps({k:report[k] for k in ['corpus_sha256','strict_loader','counts','operations','family_count','prior_corpus_file_count','prior_text_count','overlap_issues','scalar_issues','training_calibration_check']}))
