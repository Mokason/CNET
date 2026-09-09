import hashlib
import json
from pathlib import Path
import sys

WORK = Path('/tmp/cnet-confirmation-v2-qR7n165F')
source = Path(sys.argv[1])
pass_number = int(sys.argv[2])
review_path = WORK/'reviewer'/f'pass{pass_number}-review.json'
input_path = WORK/'reviewer'/f'pass{pass_number}-input.json'
cases = json.loads(source.read_bytes())['cases']
mapping = json.loads((WORK/'blind-mapping.json').read_bytes())
lookup = {mapping[c['id']]:c for c in cases}
input_value = json.loads(input_path.read_bytes())
value = json.loads(review_path.read_bytes())
reviews = value['reviews']
assert len(reviews) == len(input_value['rows'])
assert len({r['id'] for r in reviews}) == len(reviews)
assert {r['id'] for r in reviews} == {r['id'] for r in input_value['rows']}
issues = []
for review in reviews:
    row = lookup[review['id']]
    assert review['status'] in {'ready','clarify','abstain'}
    assert type(review['ambiguous']) is bool
    if review['status'] == 'ready':
        assert review['operation'] in {'upper','lower'} and type(review['key']) is int and 0 <= review['key'] <= 255
    else:
        assert review['operation'] is None and review['key'] is None
    fields = [f for f in ['status','operation','key'] if row[f] != review[f]]
    if fields or review['ambiguous']:
        issues.append({'id':row['id'],'neutral_id':review['id'],'different_fields':fields,'ambiguous':review['ambiguous'],'review_status':review['status'],'review_operation':review['operation'],'review_key':review['key'],'note':review['note']})
report = {'schema':1,'pass':pass_number,'corpus_path':str(source),'corpus_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'review_path':str(review_path),'review_sha256':hashlib.sha256(review_path.read_bytes()).hexdigest(),'input_path':str(input_path),'input_sha256':hashlib.sha256(input_path.read_bytes()).hexdigest(),'reviewed_count':len(reviews),'unchanged_rows':len(input_value['unchanged']),'issues':issues,'all_reviewed_rows_agree_unambiguously':not issues,'score_executed':False}
destination = WORK/'validation'/f'pass{pass_number}-review-comparison.json'
destination.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
