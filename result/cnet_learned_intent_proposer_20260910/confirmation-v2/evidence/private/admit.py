import hashlib
import json
from pathlib import Path
import shutil
import stat

WORK = Path('/tmp/cnet-confirmation-v2-qR7n165F')
META = Path('/home/marble/AI/CNET-worktrees/herdr-language-20260909/result/cnet_learned_intent_proposer_20260910/confirmation-v2/metadata')

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def read(relative):
    return json.loads((WORK/relative).read_bytes())

def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=True, indent=2) + '\n')

validation = read('validation/draft2.json')
audit = read('validation/draft2-keys-final.json')
review1 = read('validation/pass1-review-comparison.json')
review2 = read('validation/pass2-review-comparison.json')
assert validation['strict_schema_pass'] and validation['all_overlap_checks_pass']
assert audit['all_keys_match_unique_original'] and audit['all_ready_requests_within_length_and_control_contract']
assert review1['all_reviewed_rows_agree_unambiguously'] and review1['reviewed_count'] == 128
assert review2['all_reviewed_rows_agree_unambiguously'] and review2['reviewed_count'] == 13 and review2['unchanged_rows'] == 115
assert sha(WORK/'author/draft1.json') == '606071bcc3c8eef890dd9110607ae712843b0032179a2608754744e805acba7d'
assert sha(WORK/'author/draft2.json') == '10cdd9e0c4a5422fe08a105b6d0cabc72d092a76df1b8b70ab34a13812276e32'
assert validation['corpus_sha256'] == audit['source_sha256'] == review2['corpus_sha256'] == sha(WORK/'author/draft2.json')
old = {c['id']:c for c in read('author/draft1.json')['cases']}
new = read('author/draft2.json')['cases']
changed = [c['id'] for c in new if c != old[c['id']]]
assert set(changed) == set(read('repair-round1.json')['new_ids_requiring_replacement'])
assert all(all(c[k] == old[c['id']][k] for k in c if k != 'text') for c in new)
mapping = read('blind-mapping.json')
blind1 = {c['id']:c['text'] for c in read('reviewer/pass1-input.json')['rows']}
blind2 = read('reviewer/pass2-input.json')
assert len(blind2['unchanged']) == 115
assert all(hashlib.sha256(blind1[c['id']].encode('utf-8',errors='surrogatepass')).hexdigest() == c['text_sha256'] for c in blind2['unchanged'])
assert {c['id'] for c in blind2['rows']} == {mapping[c] for c in changed}
assert {c['id'] for c in blind2['unchanged']} | {c['id'] for c in blind2['rows']} == set(blind1)
assert sha(WORK/'protocol.json') == '4ec4d34ae51d436c61076adba62cf171eb6a5a8e12f22e7fd7c84d5538975f48'

dispositions = {'schema':1,'repair_rounds_used':1,'repair_round_limit':2,'blind_review_passes_used':2,'blind_review_pass_limit':3,'initial_review':{'count':128,'label_disagreements':0,'ambiguous':0},'round1':{'reason':'13 objective exact/normalized prior-corpus overlaps','changed_new_ids':changed,'preserved_rows':115,'labels_operations_keys_families_unchanged':True,'all_overlap_checks_after_repair':True},'second_review':{'changed_rows_reviewed':13,'unchanged_hashes_verified':115,'label_disagreements':0,'ambiguous':0},'final_review_coverage':128,'candidate_conditioned_feedback':False,'score_executed':False,'all_drafts_preserved':True}
save(WORK/'dispositions.json',dispositions)
corpus_path = WORK/'confirmation.json'
assert not corpus_path.exists(), 'refuse_repeat_admission_or_overwrite'
shutil.copyfile(WORK/'author/draft2.json',corpus_path)
corpus_path.chmod(0o444)
assert stat.S_IMODE(corpus_path.stat().st_mode) == 0o444
assert sha(corpus_path) == validation['corpus_sha256']

names = ['protocol.json','validate.py','audit_keys.py','make_blind.py','compare_review.py','admit.py','repair-round1.json','custodian-provenance.json','dispositions.json','blind-mapping.json','training-denylist.json','development-denylist.json','confirmation.json']
for directory in ['author','reviewer','review-input-provenance','validation']:
    names += [str(p.relative_to(WORK)) for p in sorted((WORK/directory).glob('*.json'))]
assert len(names) == len(set(names))
entries = [{'source_path':str(WORK/n),'source_relative':n,'archive_relative':'evidence/private/'+n,'sha256':sha(WORK/n),'bytes':(WORK/n).stat().st_size} for n in sorted(names)]
manifest = {'schema':1,'metadata_only':True,'private_workspace':str(WORK),'copy_raw_evidence_only_after':'root confirms the single score completed','contains_root_build_assets':False,'entries':entries}
manifest_path = META/'archive-manifest.json'
save(manifest_path,manifest)
shutil.copyfile(WORK/'custodian-provenance.json',META/'custodian-provenance.json')
shutil.copyfile(WORK/'dispositions.json',META/'corpus-dispositions.json')
receipt = {'schema':1,'admitted':True,'corpus_path':str(corpus_path),'corpus_sha256':sha(corpus_path),'protocol_path':str(META/'corpus-protocol.json'),'protocol_sha256':sha(WORK/'protocol.json'),'synthetic':True,'training_eligible':False,'score_executed':False,'corpus_mode':'0444','quotas':{'total':128,'ready':80,'upper':40,'lower':40,'clarify':24,'abstain':24,'min_named_families':8},'families':validation['families'],'family_count':len(validation['families']),'strict_validator':{'pass':True,'load_corpus_source_sha256':validation['validator_source_sha256']},'independent_original_scalar_audit':{'pass':True,'checked_ready_rows':80,'candidate_used':False,'raw_path':str(WORK/'validation/draft2-keys-final.json'),'raw_sha256':sha(WORK/'validation/draft2-keys-final.json')},'reviews_and_dispositions':dispositions,'overlap':{'pass':True,'normalization':validation['normalization'],'internal_exact_collisions':0,'internal_normalized_collisions':0,'prior_exact_collisions':0,'prior_normalized_collisions':0,'prior_collection_count':16,'prior_text_count':2048,'prior_unique_exact':validation['prior_unique_exact'],'prior_unique_normalized':validation['prior_unique_normalized'],'prior_inventory':[{k:v for k,v in x.items() if k not in {'exact_hashes','normalized_hashes'}} for x in validation['prior_inventory']],'prior_freeze_metadata':validation['freeze_metadata'],'denylists':validation['denylists'],'training_calibration_development_collisions':0,'raw_path':str(WORK/'validation/draft2.json'),'raw_sha256':sha(WORK/'validation/draft2.json')},'provenance':{'custodian_path':str(META/'custodian-provenance.json'),'custodian_sha256':sha(WORK/'custodian-provenance.json'),'author_path':str(WORK/'author/provenance.json'),'author_sha256':sha(WORK/'author/provenance.json'),'author_repair_path':str(WORK/'author/provenance-round1.json'),'author_repair_sha256':sha(WORK/'author/provenance-round1.json'),'reviewer_paths':[str(WORK/'reviewer/pass1-review.json'),str(WORK/'reviewer/pass2-review.json')],'isolation':'Fresh-context author and separate fresh-context blind reviewer, same model/tools; procedural, not OS or third-party/human isolation.'},'source_access_and_no_score_attestation':read('custodian-provenance.json'),'archive_manifest_path':str(manifest_path),'archive_manifest_sha256':sha(manifest_path),'raw_evidence':entries,'candidate_freeze_binding':'Performed separately by root; custodian did not modify candidate-freeze or build assets.','floors_unchanged':read('protocol.json')['floors']}
receipt_path = META/'admission.json'
save(receipt_path,receipt)
print(json.dumps({'admitted':True,'admission_path':str(receipt_path),'admission_sha256':sha(receipt_path),'corpus_path':str(corpus_path),'corpus_sha256':sha(corpus_path),'protocol_path':receipt['protocol_path'],'protocol_sha256':receipt['protocol_sha256'],'archive_manifest_path':str(manifest_path),'archive_manifest_sha256':sha(manifest_path),'score_executed':False}))
