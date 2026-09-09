"""Mechanically freeze validated synthetic data and emit metadata-only receipt."""
import hashlib,json,os,shutil,stat
from pathlib import Path

P=Path('/tmp/cnet-learned-confirmation-xAqJks4W')
D=Path('/home/marble/AI/CNET-worktrees/herdr-language-20260909/result/cnet_learned_intent_proposer_20260910/confirmation-v1')
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def read(name): return json.loads((P/name).read_bytes())
def artifact(path): return {'path':str(path),'sha256':sha(path),'bytes':path.stat().st_size}

v1=read('validation-v1.json'); v2=read('validation-v2.json')
r1=read('review-comparison-v1.json'); r2=read('review-comparison-v2.json')
assert r1['reviewed_count']==128 and r1['agreement_count']==128 and not r1['issues']
assert r2['reviewed_count']==26 and r2['agreement_count']==26 and not r2['issues']
assert not v2['overlap_issues'] and not v2['scalar_issues']
assert v2['strict_loader']=='pass'
assert v2['training_calibration_check']['sha256']=='ab6d6bf6a7d42c53ee8367647805736f30db268c8482f39d35e2068608b033df'
assert v2['development_overlap_check']['sha256']=='88102629048ef65a92acf7a464672b7bf802055038313893e35993d69bb137b6'
assert sha(P/'protocol.md')==sha(D/'protocol.md')=='cf90c87755262bfbe377fd0f12f02f92717fa4326b42a91817c91225b809f29c'
before={c['id']:c for c in read('author-v1.json')['cases']}
after={c['id']:c for c in read('author-v2.json')['cases']}
changes=read('changes-v2.json')
assert set(before)==set(after)
changed={i for i in before if before[i]!=after[i]}
assert changed==set(changes['changed_ids'])==set(read('repair-round-1.json')['ids'])
assert len(changed)==26 and len(changes['unchanged_hashes'])==102
for i in after:
    assert {k:v for k,v in before[i].items() if k!='text'}=={k:v for k,v in after[i].items() if k!='text'}
    if i not in changed:
        assert v1['row_hashes'][i]==v2['row_hashes'][i]==changes['unchanged_hashes'][i]
assert sha(P/'author-v1.json')==v1['corpus_sha256']==r1['corpus_sha256']
assert sha(P/'author-v2.json')==v2['corpus_sha256']==r2['corpus_sha256']
for version,report in [('v1',r1),('v2',r2)]:
    assert sha(P/('blind-review-'+version+'.json'))==report['review_sha256']
for item in v2['prior_inventory']:
    path=Path('/home/marble/AI/CNET-worktrees/herdr-language-20260909')/item['path']
    assert sha(path)==item['sha256']
assert sha(P/'training-denylist.json')==v2['training_calibration_check']['sha256']
assert sha(P/'development-denylist.json')==v2['development_overlap_check']['sha256']

frozen=P/'confirmation.json'
assert not frozen.exists()
shutil.copyfile(P/'author-v2.json',frozen)
os.chmod(frozen,0o444)
assert sha(frozen)==v2['corpus_sha256']
assert stat.S_IMODE(frozen.stat().st_mode)==0o444
evidence_names=['protocol.md','author-v1-draft.json','author-v1-neutral-draft.json','author-v1.json','author-v2.json','blind-view-v1.json','blind-view-v2.json','blind-review-v1.json','blind-review-v2.json','validation-v1.json','validation-v2.json','review-comparison-v1.json','review-comparison-v2.json','changes-v2.json','repair-round-1.json','custodian-v1-observations.json','custodian-v2-observations.json','validate.py','compare_review.py','inspect_prior.py','finalize_admission.py']
evidence_names.append('role-provenance.json')
evidence=[artifact(P/name) for name in evidence_names]
evidence.extend(artifact(P/name) for name in ['training-denylist.json','development-denylist.json'])
receipt={
    'schema':1,'admitted':True,'corpus_path':str(frozen),'corpus_sha256':sha(frozen),'corpus_permissions':'0444',
    'protocol_path':str(D/'protocol.md'),'protocol_sha256':sha(D/'protocol.md'),
    'synthetic':True,'training_eligible':False,'score_executed':False,
    'candidate_execution_by_custodian_author_reviewer':False,'candidate_source_access_by_custodian_author_reviewer':False,
    'source_binding':'Root binds existing candidate-freeze separately; custodian did not change it.',
    'corpus_schema':{'top_level':['schema','name','cases'],'schema':1,'name':'confirmation','case_fields':['id','family','text','status','operation','key'],'id_family_pattern':'[A-Za-z0-9_-]{1,48}','distinct_texts':128,'distinct_ids':128,'rows':128,'statuses':v2['counts'],'operations':v2['operations'],'families':v2['family_count'],'nonready_executable_fields':0,'strict_loader':'pass','strict_loader_source_prefix_sha256':v2['strict_loader_source_prefix_sha256']},
    'independent_review':{'blind_passes_used':2,'maximum_blind_passes':3,'initial_reviewed':128,'initial_agreed':128,'changed_rows_reviewed':26,'changed_rows_agreed':26,'unchanged_rows_verified_by_sha256':102,'final_unique_rows_reviewed':128,'unresolved_label_issues':0,'uncertain_rows':0,'reviewer_input':'Mechanically generated id/text only; neutral IDs. No author labels/families/keys supplied.','scalar_logic':'Strict typed input constraints, explicit operand checks, controls/surrogates/UTF-16 bounds and custodian inspection passed.'},
    'repairs':{'custodian_directed_rounds_used':1,'maximum_rounds':2,'cause':'Objective historical/development overlap only','changed_rows':26,'label_operation_key_family_changes':0,'output_conditioned':False,'originals_preserved':True,'initial_author_drafting':'Two pre-submission drafts preserved: neutralized IDs and seven wording changes for internal normalized duplicates; no custodian repair rounds or candidate feedback during drafting.'},
    'overlap':{'normalization':'Unicode casefold then remove every Unicode P* punctuation codepoint and all Unicode whitespace','internal_exact_issues':0,'internal_normalized_issues':0,'external_exact_issues':0,'external_normalized_issues':0,'prior_corpus_file_count':v2['prior_corpus_file_count'],'prior_text_count':v2['prior_text_count'],'prior_unique_exact_hashes':v2['prior_unique_exact_hashes'],'prior_unique_normalized_hashes':v2['prior_unique_normalized_hashes'],'prior_inventory':v2['prior_inventory'],'training_calibration':v2['training_calibration_check'],'development':v2['development_overlap_check'],'scope_addendum':artifact(D/'overlap-scope-addendum.md')},
    'role_provenance':{'author':'/root/confirmation_custodian_v1/author; fork_turns=none; inherited model; contract-only task; author reported no non-owned file access','custodian':'/root/confirmation_custodian_v1; no authorship, CNET execution or score; prior texts accessed only through mechanical hashes; approved strict validator/contract access only','reviewer':'/root/confirmation_custodian_v1/blind_reviewer; fork_turns=none; inherited model; contract plus label-blind id/text views','scorer':'/root; one authorized score only after admission','independence_limit':'Procedural fresh contexts on the same model/tools; not enforced OS isolation, independent organizations or third-party human benchmark.'},
    'evidence':evidence,
    'preservation':'Corpus drafts and reviews remain private until root scores. All evidence is synthetic and training-ineligible; no output-conditioned modification or additional score is authorized.'
}
receipt_path=D/'admission.json'
with receipt_path.open('x') as out:
    json.dump(receipt,out,indent=2);out.write('\n')
for name in evidence_names:
    os.chmod(P/name,0o444)
os.chmod(receipt_path,0o444)
os.chmod(D/'protocol.md',0o444)
manifest={'schema':1,'purpose':'Preserve only after root confirms the single score completed; metadata-only manifest may be read beforehand','exclude':'All root-owned spec-hashes build artifacts and unrelated files','synthetic':True,'training_eligible':False,'artifacts':evidence+[artifact(frozen),artifact(receipt_path),artifact(D/'protocol.md'),artifact(D/'overlap-scope-addendum.md')]}
manifest_path=D/'archive-manifest.json'
with manifest_path.open('x') as out:
    json.dump(manifest,out,indent=2);out.write('\n')
os.chmod(manifest_path,0o444)
print(json.dumps({'admitted':True,'corpus_path':str(frozen),'corpus_sha256':sha(frozen),'admission_path':str(receipt_path),'admission_sha256':sha(receipt_path),'protocol_sha256':sha(D/'protocol.md'),'archive_manifest_path':str(manifest_path),'archive_manifest_sha256':sha(manifest_path),'score_executed':False}))
