"""Paired exposed-data diagnostic, not a new confirmation or production mode."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[2]
EVALUATOR = ROOT / 'tools/task_paraphrase_eval/evaluate.py'
DOTNET = Path('/home/marble/dotnet/dotnet')
PROBE = ROOT / '.artifacts/intent-ablation/bin/TaskIntentAblation/debug/cnet-task-intent-ablation.dll'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    with path.open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write('\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assembly', required=True, type=Path)
    parser.add_argument('--corpus', required=True, type=Path)
    parser.add_argument('--corpus-sha256', required=True)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    assembly, corpus, output = args.assembly.resolve(), args.corpus.resolve(), args.output.resolve()
    spec = importlib.util.spec_from_file_location('exact_scorer', EVALUATOR)
    evaluator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(evaluator)
    cases = evaluator.load_corpus(evaluator.read_bounded(corpus), args.corpus_sha256, 'confirmation')
    assets = [assembly, corpus, EVALUATOR, DOTNET, PROBE, Path(__file__).resolve(),
              Path(__file__).with_name('Program.cs'), Path(__file__).with_name('test_probe.py')]
    for asset in (assembly, PROBE):
        assets.extend(asset.with_suffix(s) for s in ('.deps.json', '.runtimeconfig.json'))
    # Snapshot all production proposer sources, not only the top-level parser.
    assets.extend(sorted((ROOT / 'dotnet/CnetControlPlane/Learning').glob('Learning*Intent*.cs')))
    assets.extend(ROOT / 'dotnet/CnetControlPlane/Learning' / name
                  for name in ('LearningTaskProposal.cs', 'LearningCaseRequestSyntax.cs'))
    pins = {str(p): digest(p) for p in assets}
    output.mkdir(parents=True, exist_ok=False)
    save(output / 'binding.json', {'schema': 1, 'at_utc': datetime.now(timezone.utc).isoformat(),
         'mode': 'exposed_paired_diagnostic', 'fresh_confirmation': False,
         'synthetic': True, 'training_eligible': False, 'pins': pins})
    try:
        with (output / 'response.json').open('xb') as stdout, (output / 'stderr.txt').open('xb') as stderr:
            child = subprocess.run([str(DOTNET), str(PROBE), str(assembly), pins[str(assembly)]],
                                   input=evaluator.encode_requests([c['text'] for c in cases]),
                                   stdout=stdout, stderr=stderr, timeout=60, env={})
        if child.returncode or (output / 'stderr.txt').stat().st_size:
            raise ValueError('diagnostic_probe_refused')
        response = evaluator.decode(evaluator.read_bounded(output / 'response.json'))
        if (set(response) != {'schema', 'assembly_sha256', 'arms'} or response['schema'] != 1
                or response['assembly_sha256'] != pins[str(assembly)]
                or set(response['arms']) != {'hybrid', 'grammar', 'guarded_learned_raw'}):
            raise ValueError('diagnostic_identity')
        reports = {arm: evaluator.score(cases, proposals) for arm, proposals in response['arms'].items()}
        changed = []
        for grammar, hybrid in zip(reports['grammar']['cases'], reports['hybrid']['cases'], strict=True):
            if grammar['proposal'] != hybrid['proposal']:
                changed.append({'id': grammar['id'], 'grammar': grammar, 'hybrid': hybrid})
        for report in reports.values():
            del report['cases']  # All raw arm predictions remain in response.json.
        result = {'schema': 1, 'fresh_confirmation': False,
             'synthetic': True, 'training_eligible': False, 'native_actions': 0,
             'arms': reports, 'paired_changed_rows': changed,
             'paired_exact_delta': reports['hybrid']['exact'] - reports['grammar']['exact'],
             'response_sha256': digest(output / 'response.json'),
             'limitation': 'Guarded learned raw retains handwritten input/domain/operand guards and differs in preprocessing; equal outputs do not prove fallback was unused.'}
    finally:
        for path, expected in pins.items():
            if digest(Path(path)) != expected:
                raise ValueError('diagnostic_pin_changed:' + path)
    save(output / 'result.json', result)
    print(json.dumps({arm: {key: report[key] for key in ('exact', 'wrong_ready', 'by_status')}
                      for arm, report in reports.items()}, sort_keys=True))


if __name__ == '__main__':
    main()
