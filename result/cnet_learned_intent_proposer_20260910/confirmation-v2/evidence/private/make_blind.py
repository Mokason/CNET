import hashlib
import json
from pathlib import Path
import sys

WORK = Path('/tmp/cnet-confirmation-v2-qR7n165F')

def digest(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(',', ':')).encode()).hexdigest()

def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=True, indent=2) + '\n')

source = Path(sys.argv[1])
pass_number = int(sys.argv[2])
cases = json.loads(source.read_bytes())['cases']
assert 1 <= pass_number <= 3
if pass_number == 1:
    # Neutral identifiers and text-derived deterministic ordering expose no labels/families/keys.
    ordered = sorted(cases, key=lambda c: hashlib.sha256(('blind-v2:' + c['text']).encode('utf-8', errors='surrogatepass')).hexdigest())
    mapping = {c['id']:f'r{i:03d}' for i,c in enumerate(ordered, 1)}
    save(WORK/'blind-mapping.json',mapping)
    selected = ordered
    unchanged = []
else:
    previous = json.loads(Path(sys.argv[3]).read_bytes())['cases']
    old = {c['id']:c for c in previous}
    assert set(old) == {c['id'] for c in cases}
    mapping = json.loads((WORK/'blind-mapping.json').read_bytes())
    selected = [c for c in cases if c != old[c['id']]]
    unchanged = [{'id':mapping[c['id']], 'text_sha256':hashlib.sha256(c['text'].encode('utf-8', errors='surrogatepass')).hexdigest()} for c in cases if c == old[c['id']]]
    # A changed hidden label also requires blind reassessment even if its text was unchanged.
rows = sorted([{'id':mapping[c['id']], 'text':c['text']} for c in selected],key=lambda c:c['id'])
save(WORK/'reviewer'/f'pass{pass_number}-input.json',{'schema':1,'pass':pass_number,'rows':rows,'unchanged':unchanged})
save(WORK/'review-input-provenance'/f'pass{pass_number}.json',{'schema':1,'source_path':str(source),'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'blind_input_path':str(WORK/'reviewer'/f'pass{pass_number}-input.json'),'rows':len(rows),'unchanged_rows':len(unchanged),'score_executed':False})
print(json.dumps({'pass':pass_number,'rows':len(rows),'unchanged_rows':len(unchanged)}))
