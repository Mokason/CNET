import hashlib
import json
from pathlib import Path
import re
import sys
import unicodedata

source = Path(sys.argv[1])
destination = Path(sys.argv[2])
rows = json.loads(source.read_bytes())['cases']
audit = []
for row in rows:
    if row['status'] != 'ready':
        continue
    text = row['text']
    candidates = []
    for match in re.finditer(r'\bU\+([0-9A-Fa-f]{2,6})\b',text):
        candidates.append(('hexadecimal',int(match[1],16)))
    for match in re.finditer(r'\bcode\s*point\s*(?:number\s*)?(?:[:=#]\s*)?([0-9]+)\b',text,re.I):
        candidates.append(('decimal',int(match[1],10)))
    for match in re.finditer(r'([\'\"])(.)\1|‘(.)’|“(.)”',text,re.S):
        char = next(x for x in match.groups()[1:] if x is not None)
        candidates.append(('quoted_literal',ord(char)))
    if not candidates:
        singles = re.findall(r"(?<![\w'’])([^\W\d_])(?![\w'’])",text,re.UNICODE)
        # Lowercase article a and pronoun I may be grammatical; preserve ambiguity for manual audit.
        candidates = [('bare_single_letter',ord(char)) for char in singles]
        if len(candidates) > 1:
            narrowed = [item for item in candidates if item[1] not in (97,73)]
            if len(narrowed) == 1:
                candidates = narrowed
    unique = sorted({value for _,value in candidates})
    audit.append({'id':row['id'],'declared_original_key':row['key'],'independent_candidates':unique,'methods':sorted({method for method,_ in candidates}),'key_matches_unique_original':unique==[row['key']],'control_free':not any(unicodedata.category(c)=='Cc' for c in text),'original_scalar_control_free':unicodedata.category(chr(row['key']))!='Cc','utf16_units':len(text.encode('utf-16-le',errors='surrogatepass'))//2})
report = {'schema':1,'source_path':str(source),'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'ready_count':len(audit),'all_keys_match_unique_original':all(a['key_matches_unique_original'] for a in audit),'all_ready_requests_within_length_and_control_contract':all(a['control_free'] and a['original_scalar_control_free'] and a['utf16_units']<=256 for a in audit),'rows':audit,'candidate_used':False,'score_executed':False}
destination.parent.mkdir(parents=True,exist_ok=True)
destination.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k!='rows'}))
print(json.dumps({'needs_manual_audit':[a for a in audit if not a['key_matches_unique_original']]}))
