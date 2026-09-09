"""Second candidate orchestration; original evaluation floors are unchanged."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path('/home/marble/AI/CNET-worktrees/herdr-language-20260909')
RUN = Path('/tmp/cnet-herdr-three-heads-PK7L3c')
ASSEMBLY = ROOT / '.artifacts/herdr-vocabulary/bin/CnetControlPlane/debug/cnet-control.dll'
PROBE = ROOT / '.artifacts/herdr-probe/bin/TaskParaphraseProbe/debug/cnet-task-paraphrase-probe.dll'
SOURCES = [ROOT / 'dotnet/CnetControlPlane/Learning/LearningTaskProposal.cs',
    ROOT / 'dotnet/CnetControlPlane/Learning/LearningCaseRequestSyntax.cs',
    ROOT / 'tools/task_paraphrase_eval/evaluate.py', PROBE, ASSEMBLY, Path(__file__)]
spec = importlib.util.spec_from_file_location('unchanged_evaluator', SOURCES[2])
evaluator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evaluator)

def identity(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def pins():
    return {str(path): identity(path) for path in SOURCES}

def score_file(path, name, assembly_sha):
    raw = evaluator.read_bounded(path)
    sha = evaluator.digest(raw)
    cases = evaluator.load_corpus(raw, sha, name)
    child = subprocess.run(['dotnet', str(PROBE), str(ASSEMBLY), assembly_sha],
        input=evaluator.encode_requests([case['text'] for case in cases]),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60, check=True)
    result = evaluator.decode(child.stdout)
    if set(result) != {'schema', 'assembly_sha256', 'proposals'} or result['schema'] != 1 or result['assembly_sha256'] != assembly_sha:
        raise ValueError('probe_identity')
    return {'corpus_path': str(path), 'corpus_sha256': sha,
        'score': evaluator.score(cases, result['proposals'])}

def main():
    mode, output = sys.argv[1:]
    frozen_pins = pins()
    sha = identity(ASSEMBLY)
    with open(output, 'x', encoding='utf-8') as stream:
        stream.write(json.dumps({'state': 'started', 'mode': mode, 'pins': frozen_pins}) + '\n')
        stream.flush()
        os.fsync(stream.fileno())
        if mode == 'development':
            population = [(path, name) for directory in sorted((ROOT / 'benchmarks').glob('task_paraphrases*_20260909'))
                for name in ('qualification', 'confirmation') if (path := directory / (name + '.json')).is_file()]
            population.append((RUN / 'evaluator/confirmation-frozen.json', 'confirmation'))
        elif mode == 'confirmation':
            freeze = json.loads((RUN / 'vocabulary-freeze.json').read_text())
            if freeze['pins'] != frozen_pins:
                raise ValueError('candidate_freeze_mismatch')
            path = Path(freeze['corpus_path'])
            if identity(path) != freeze['corpus_sha256'] or identity(Path(freeze['admission_path'])) != freeze['admission_sha256']:
                raise ValueError('confirmation_admission_mismatch')
            population = [(path, 'confirmation')]
        else:
            raise ValueError('mode')
        collections = [score_file(path, name, sha) for path, name in population]
        if pins() != frozen_pins:
            raise ValueError('source_changed_during_run')
        report = {'schema': 1, 'state': 'complete', 'mode': mode, 'pins': frozen_pins,
            'synthetic': True, 'training_eligible': False, 'native_actions': 0, 'collections': collections}
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
