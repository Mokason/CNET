import ast
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys
import unicodedata

ROOT = Path('/home/marble/AI/CNET-worktrees/herdr-language-20260909')
WORK = Path('/tmp/cnet-confirmation-v2-qR7n165F')

def sha(raw):
    return hashlib.sha256(raw).hexdigest()

def norm(text):
    return ''.join(c for c in text.casefold() if not c.isspace() and not unicodedata.category(c).startswith('P'))

def text_hash(text):
    return sha(text.encode('utf-8', errors='surrogatepass'))

def pairs(items):
    value = {}
    for key, item in items:
        if key in value:
            raise ValueError('duplicate_json_key')
        value[key] = item
    return value

def decode(raw):
    return json.loads(raw.decode('utf-8'), object_pairs_hook=pairs, parse_constant=lambda value: (_ for _ in ()).throw(ValueError('nonfinite')))

def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=True, indent=2) + '\n')

def run(corpus_path, tag):
    # Mechanically hash prior text; never print, copy, or reveal its content.
    prior_paths = sorted(ROOT.glob('benchmarks/task_paraphrases*/qualification.json')) + sorted(ROOT.glob('benchmarks/task_paraphrases*/confirmation.json')) + sorted(ROOT.glob('result/cnet_herdr_three_heads_20260909/exposed-corpora/*.json')) + [ROOT/'result/cnet_learned_intent_proposer_20260910/confirmation-v1/evidence/private/confirmation.json']
    assert len(prior_paths) == 16
    exact, normalized, inventory = set(), set(), []
    for path in prior_paths:
        raw = path.read_bytes()
        if 'confirmation-v1' in str(path):
            assert sha(raw) == '806c0c2d38eeda7422f0cc03e36e50ff5698687513df4b50a90d4abe8c5b5f53'
        rows = decode(raw)['cases']
        assert len(rows) == 128
        ex = sorted(text_hash(c['text']) for c in rows)
        no = sorted(text_hash(norm(c['text'])) for c in rows)
        exact.update(ex)
        normalized.update(no)
        inventory.append({'path': str(path), 'sha256': sha(raw), 'count': len(rows), 'exact_hashes': ex, 'normalized_hashes': no})
    freeze_metadata = [{'path':str(p), 'sha256':sha(p.read_bytes())} for p in sorted(ROOT.glob('benchmarks/task_paraphrases*/freeze.json'))]
    deny_inventories = []
    deny_sets = {}
    for name, expected in [('training-denylist.json','58d7d2690343f6a49d7d62a9ddf3e185a255045200ebe73a44cab3ef9c2d1b19'), ('development-denylist.json','fea685d0354d625075f6c45941a7f0eda7ce26e21830ae1542929eba6f443a8a')]:
        p = WORK/name
        raw = p.read_bytes()
        assert sha(raw) == expected
        v = decode(raw)
        groups = v.get('groups', {'development': v})
        for group, d in groups.items():
            deny_sets[group] = (set(d['exact_sha256']), set(d['normalized_sha256']))
            deny_inventories.append({'path':str(p),'sha256':sha(raw),'group':group,'count':d['count'],'unique_exact':len(set(d['exact_sha256'])),'unique_normalized':len(set(d['normalized_sha256']))})
    raw = corpus_path.read_bytes()
    # Only the authorized load_corpus function is extracted, never importing or executing the scoring module.
    lines = (ROOT/'tools/task_paraphrase_eval/evaluate.py').read_text().splitlines(keepends=True)
    validator_source = ''.join(lines[46:74])
    assert validator_source.startswith('def load_corpus(')
    tree = ast.parse(validator_source)
    assert len(tree.body) == 1 and isinstance(tree.body[0], ast.FunctionDef) and tree.body[0].name == 'load_corpus'
    context = {'re':re,'hashlib':hashlib,'decode':decode,'ATOM':re.compile(r'[A-Za-z0-9_-]{1,48}'),'STATUSES':{'ready','clarify','abstain'},'Counter':Counter}
    exec(compile(tree, '<authorized strict load_corpus>', 'exec'), context)
    cases = context['load_corpus'](raw, sha(raw), 'confirmation')
    issues = []
    seen_exact, seen_normalized = {}, {}
    for row in cases:
        ex, no = text_hash(row['text']), text_hash(norm(row['text']))
        for label, item, seen in [('internal_exact',ex,seen_exact),('internal_normalized',no,seen_normalized)]:
            if item in seen:
                issues.append({'id':row['id'],'kind':label,'new_peer_id':seen[item]})
            else:
                seen[item] = row['id']
        if ex in exact: issues.append({'id':row['id'],'kind':'prior_exact'})
        if no in normalized: issues.append({'id':row['id'],'kind':'prior_normalized'})
        for group,(dex,dno) in deny_sets.items():
            if ex in dex: issues.append({'id':row['id'],'kind':group+'_exact'})
            if no in dno: issues.append({'id':row['id'],'kind':group+'_normalized'})
    report = {'schema':1,'corpus_path':str(corpus_path),'corpus_sha256':sha(raw),'strict_schema_pass':True,'validator_source_sha256':sha(validator_source.encode()),'count':len(cases),'statuses':dict(Counter(c['status'] for c in cases)),'operations':dict(Counter(str(c['operation']) for c in cases)),'families':dict(Counter(c['family'] for c in cases)),'normalization':'Unicode casefold then remove Unicode P* punctuation and all whitespace','prior_collection_count':len(inventory),'prior_text_count':sum(i['count'] for i in inventory),'prior_unique_exact':len(exact),'prior_unique_normalized':len(normalized),'prior_inventory':inventory,'freeze_metadata':freeze_metadata,'denylists':deny_inventories,'overlap_issues':issues,'all_overlap_checks_pass':not issues,'score_executed':False}
    save(WORK/'validation'/f'{tag}.json',report)
    print(json.dumps({k:v for k,v in report.items() if k not in {'prior_inventory','freeze_metadata'}},ensure_ascii=True))

if __name__ == '__main__':
    run(Path(sys.argv[1]),sys.argv[2])
