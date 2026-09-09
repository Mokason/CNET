import hashlib,json
from pathlib import Path

root=Path('/home/marble/AI/CNET-worktrees/herdr-language-20260909')
paths=set()
for name in ('freeze.json','confirmation.json','qualification.json'):
    paths.update(root.glob('benchmarks/task_paraphrases*/'+name))
paths.update(root.glob('result/cnet_herdr_three_heads_20260909/exposed-corpora/*.json'))
for path in sorted(paths):
    raw=path.read_bytes()
    obj=json.loads(raw)
    cases=obj.get('cases',[]) if isinstance(obj,dict) else []
    print(json.dumps({'path':str(path.relative_to(root)),'sha256':hashlib.sha256(raw).hexdigest(),'case_count':len(cases),'text_count':sum(isinstance(c,dict) and isinstance(c.get('text'),str) for c in cases)}))
