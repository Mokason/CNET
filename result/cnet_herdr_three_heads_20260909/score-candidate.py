"""Pin the existing scorer and actual parser probe; never call a native action."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path('/home/marble/AI/CNET-worktrees/herdr-language-20260909')
spec = importlib.util.spec_from_file_location('frozen_score', ROOT / 'tools/task_paraphrase_eval/evaluate.py')
evaluator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evaluator)
assembly = ROOT / '.artifacts/herdr-task/bin/CnetControlPlane/debug/cnet-control.dll'
probe = ROOT / '.artifacts/herdr-probe/bin/TaskParaphraseProbe/debug/cnet-task-paraphrase-probe.dll'

def identity(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def score_file(path, name, assembly_sha):
    raw = evaluator.read_bounded(path)
    sha = evaluator.digest(raw)
    cases = evaluator.load_corpus(raw, sha, name)
    child = subprocess.run(['dotnet', str(probe), str(assembly), assembly_sha],
        input=evaluator.encode_requests([c['text'] for c in cases]), stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, timeout=60, check=True)
    result = evaluator.decode(child.stdout)
    if set(result) != {'schema', 'assembly_sha256', 'proposals'} or result['schema'] != 1 or result['assembly_sha256'] != assembly_sha:
        raise ValueError('probe_identity')
    scored = evaluator.score(cases, result['proposals'])
    return {'corpus_path': str(path), 'corpus_sha256': sha, 'score': scored}

def main():
    mode, output = sys.argv[1:]
    sha = identity(assembly)
    sources = [ROOT / 'dotnet/CnetControlPlane/Learning/LearningTaskProposal.cs',
        ROOT / 'dotnet/CnetControlPlane/Learning/LearningCaseRequestSyntax.cs',
        ROOT / 'tools/task_paraphrase_eval/evaluate.py', probe, assembly]
    pins = {str(path.relative_to(ROOT)): identity(path) for path in sources}
    with open(output, 'x', encoding='utf-8') as stream:
        # Reserve one output before any request; a failed/partial run is not
        # silently overwritten with a successful repeat of confirmation.
        stream.write(json.dumps({'state': 'started', 'mode': mode, 'pins': pins}) + '\n')
        stream.flush()
        if mode == 'development':
            collections = []
            for directory in sorted((ROOT / 'benchmarks').glob('task_paraphrases*_20260909')):
                for name in ('qualification', 'confirmation'):
                    path = directory / (name + '.json')
                    if path.is_file():
                        collections.append(score_file(path, name, sha))
        elif mode == 'confirmation':
            freeze = json.loads(Path('/tmp/cnet-herdr-three-heads-PK7L3c/candidate-freeze.json').read_text())
            if freeze['pins'] != pins or freeze['corpus_sha256'] != 'f47c14e717f414984fcbb1f472ddbf39460b5ef301a805f902af91bb74b59a57':
                raise ValueError('candidate_freeze_mismatch')
            path = Path('/tmp/cnet-herdr-three-heads-PK7L3c/evaluator/confirmation-frozen.json')
            if identity(path) != freeze['corpus_sha256']:
                raise ValueError('confirmation_hash_mismatch')
            collections = [score_file(path, 'confirmation', sha)]
        else:
            raise ValueError('mode')
        if pins != {str(path.relative_to(ROOT)): identity(path) for path in sources}:
            raise ValueError('source_changed_during_run')
        report = {'schema': 1, 'state': 'complete', 'mode': mode, 'pins': pins,
            'synthetic': True, 'training_eligible': False, 'native_actions': 0,
            'collections': collections}
        stream.write(json.dumps(report, sort_keys=True) + '\n')
        stream.flush()
        os.fsync(stream.fileno())
    for entry in collections:
        score = entry['score']
        print(json.dumps({'collection': entry['corpus_path'], 'corpus_sha256': entry['corpus_sha256'],
            **{key: score[key] for key in ('passed', 'total', 'exact', 'wrong_ready', 'gates', 'by_status', 'by_operation')},
            'failed_ids': [row['id'] for row in score['cases'] if not row['exact']]}))
    return 0 if all(entry['score']['passed'] for entry in collections) else 1

if __name__ == '__main__':
    sys.exit(main())
