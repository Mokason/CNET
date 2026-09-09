"""One frozen grammar-on confirmation. Reuses the unchanged exact scorer."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
from datetime import datetime, timezone

ROOT = Path('/home/marble/AI/CNET-worktrees/herdr-language-20260909')
HERE = Path(__file__).resolve().parent


def checked(path, expected):
    with open(path, 'rb') as stream:
        raw = stream.read(32 * 1024 * 1024 + 1)
    if len(raw) > 32 * 1024 * 1024 or hashlib.sha256(raw).hexdigest() != expected:
        raise ValueError('pin_mismatch:' + str(path))
    return raw


def check_all(binding):
    for role in ('candidate_freeze', 'admission', 'corpus', 'evaluator', 'probe', 'assembly', 'dotnet'):
        path = binding.get(role)
        if not isinstance(path, str) or not Path(path).is_absolute() or path not in binding['pins']:
            raise ValueError('missing_role_pin:' + role)
    required_assets = [Path(__file__).resolve()]
    for role in ('probe', 'assembly'):
        required_assets.extend(Path(binding[role]).with_suffix(suffix)
                               for suffix in ('.deps.json', '.runtimeconfig.json'))
    if any(str(path) not in binding['pins'] for path in required_assets):
        raise ValueError('missing_harness_asset_pin')
    for path, expected in binding['pins'].items():
        checked(path, expected)
    freeze = json.loads(checked(binding['candidate_freeze'], binding['pins'][binding['candidate_freeze']]))
    for path, expected in freeze['pins'].items():
        checked(ROOT / path, expected)
    if freeze['confirmation_scoring_limit'] != 1 or freeze['grammar_off'] or freeze['deployed']:
        raise ValueError('frozen_mode')
    if freeze['floors'] != {'wrong_ready': 0, 'ready': '72/80', 'ready_per_operation': '34/40',
                            'clarify': '22/24', 'abstain': '23/24'}:
        raise ValueError('frozen_floors')
    if binding['pins'][binding['assembly']] != freeze['pins'][str(Path(binding['assembly']).relative_to(ROOT))]:
        raise ValueError('assembly_binding')


def perform(binding_path):
    binding = json.loads(binding_path.read_text())
    if binding['schema'] != 1 or binding['score_limit'] != 1:
        raise ValueError('binding_schema')
    binding_hash = hashlib.sha256(binding_path.read_bytes()).hexdigest()
    check_all(binding)
    admission = json.loads(checked(binding['admission'], binding['pins'][binding['admission']]))
    if (admission['admitted'] is not True or admission['score_executed'] is not False
            or admission['synthetic'] is not True or admission['training_eligible'] is not False
            or admission['corpus_path'] != binding['corpus']
            or admission['corpus_sha256'] != binding['pins'][binding['corpus']]):
        raise ValueError('admission_binding')
    spec = importlib.util.spec_from_file_location('unchanged_evaluator', binding['evaluator'])
    evaluator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(evaluator)
    cases = evaluator.load_corpus(
        checked(binding['corpus'], binding['pins'][binding['corpus']]),
        binding['pins'][binding['corpus']], 'confirmation')
    try:
        # Persist byte-for-byte transport evidence, even when the probe refuses.
        with open(HERE / 'probe-response.json', 'xb') as stdout_file, open(HERE / 'probe-stderr.txt', 'xb') as stderr_file:
            try:
                child = subprocess.run(
                    [binding['dotnet'], binding['probe'], binding['assembly'], binding['pins'][binding['assembly']]],
                    input=evaluator.encode_requests([case['text'] for case in cases]),
                    stdout=stdout_file, stderr=stderr_file, timeout=60, env={})
            finally:
                stdout_file.flush()
                stderr_file.flush()
                os.fsync(stdout_file.fileno())
                os.fsync(stderr_file.fileno())
        response_bytes = evaluator.read_bounded(HERE / 'probe-response.json')
        if child.returncode or (HERE / 'probe-stderr.txt').stat().st_size:
            raise ValueError('managed_probe_refused')
        response = evaluator.decode(response_bytes)
        if (set(response) != {'schema', 'assembly_sha256', 'proposals'}
                or type(response['schema']) is not int or response['schema'] != 1
                or response['assembly_sha256'] != binding['pins'][binding['assembly']]):
            raise ValueError('managed_probe_identity')
        report = evaluator.score(cases, response['proposals'])
    finally:
        check_all(binding)
        checked(binding_path, binding_hash)
    return {'schema': 1, 'synthetic': True, 'training_eligible': False,
            'grammar_off': False, 'deployed': False, 'native_actions': 0,
            'binding_sha256': binding_hash, 'pins': binding['pins'],
            'probe_response_sha256': hashlib.sha256(response_bytes).hexdigest(), **report}


def run_once(binding_path, output_path):
    fd = os.open(output_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w', encoding='utf-8') as stream:
        def record(value):
            stream.write(json.dumps(value, sort_keys=True) + '\n')
            stream.flush()
            os.fsync(stream.fileno())
        record({'state': 'started', 'at_utc': datetime.now(timezone.utc).isoformat(),
                'binding_path': str(binding_path), 'score_limit': 1})
        try:
            result = perform(binding_path)
        except Exception as error:
            chain = []
            current = error
            while current is not None:
                chain.append({'error_type': type(current).__name__, 'detail': str(current)})
                current = current.__cause__ or current.__context__
            record({'state': 'failed', 'errors': chain})
            raise
        record({'state': 'complete', 'at_utc': datetime.now(timezone.utc).isoformat(), 'result': result})
        return result


if __name__ == '__main__':
    if len(sys.argv) != 1:
        raise SystemExit('No arguments: this runner has one fixed attempt path.')
    outcome = run_once(HERE / 'score-binding.json', HERE / 'score-once.jsonl')
    print(json.dumps({key: outcome[key] for key in
                     ('passed', 'total', 'exact', 'wrong_ready', 'gates', 'by_status', 'by_operation')}, sort_keys=True))
    raise SystemExit(0 if outcome['passed'] else 1)
