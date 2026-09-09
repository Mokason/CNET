import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch, Mock

RUNNER = Path('/home/marble/AI/CNET-worktrees/herdr-language-20260909/result/cnet_learned_intent_proposer_20260910/confirmation-v2/score_once.py')


class OneShotIntegrity(unittest.TestCase):
    def setUp(self):
        self.assertTrue(RUNNER.is_file(), 'CONFIRMATION_RUNNER_RED missing one-shot runner')
        spec = importlib.util.spec_from_file_location('score_once', RUNNER)
        self.runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.runner)

    def test_existing_attempt_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'score.jsonl'
            output.write_text('original\n')
            with patch.object(self.runner, 'perform') as perform:
                with self.assertRaises(FileExistsError):
                    self.runner.run_once(Path(directory) / 'absent.json', output)
                perform.assert_not_called()
            self.assertEqual(output.read_text(), 'original\n')

    def test_failed_preflight_latches_attempt_without_invocation(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'score.jsonl'
            with patch.object(self.runner.subprocess, 'run') as invoke:
                with self.assertRaises(FileNotFoundError):
                    self.runner.run_once(Path(directory) / 'absent.json', output)
                invoke.assert_not_called()
            rows = [json.loads(line) for line in output.read_text().splitlines()]
            self.assertEqual([row['state'] for row in rows], ['started', 'failed'])
            with self.assertRaises(FileExistsError):
                self.runner.run_once(Path(directory) / 'absent.json', output)

    def test_quality_failure_is_retained_without_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'score.jsonl'
            with patch.object(self.runner, 'perform', return_value={'passed': False}) as perform:
                result = self.runner.run_once(Path(directory) / 'binding.json', output)
                self.assertFalse(result['passed'])
                perform.assert_called_once()
            rows = [json.loads(line) for line in output.read_text().splitlines()]
            self.assertEqual(rows[-1]['state'], 'complete')
            self.assertFalse(rows[-1]['result']['passed'])

    def test_identity_mismatch_refuses(self):
        with tempfile.TemporaryDirectory() as directory:
            value = Path(directory) / 'input'
            value.write_bytes(b'original')
            with self.assertRaisesRegex(ValueError, 'pin_mismatch'):
                self.runner.checked(value, '0' * 64)

    def test_probe_timeout_still_checks_all_post_run_pins(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binding_path = root / 'binding.json'
            binding = {'schema': 1, 'score_limit': 1, 'admission': 'admission',
                       'corpus': 'corpus', 'evaluator': 'evaluator', 'probe': 'probe',
                       'assembly': 'assembly', 'dotnet': '/test/pinned/dotnet',
                       'pins': {'admission': 'a', 'corpus': 'c', 'assembly': 'd'}}
            binding_path.write_text(json.dumps(binding))
            admission = {'admitted': True, 'score_executed': False, 'synthetic': True,
                         'training_eligible': False, 'corpus_path': 'corpus', 'corpus_sha256': 'c'}
            evaluator = Mock()
            evaluator.load_corpus.return_value = [{'text': 'fixture'}]
            evaluator.encode_requests.return_value = b'[]'
            with patch.object(self.runner, 'HERE', root), \
                 patch.object(self.runner, 'check_all') as verify, \
                 patch.object(self.runner, 'checked', return_value=json.dumps(admission).encode()), \
                 patch.object(self.runner.importlib.util, 'spec_from_file_location') as spec, \
                 patch.object(self.runner.importlib.util, 'module_from_spec', return_value=evaluator), \
                 patch.object(self.runner.subprocess, 'run', side_effect=self.runner.subprocess.TimeoutExpired('dotnet', 60)) as invoke:
                spec.return_value.loader.exec_module.return_value = None
                with self.assertRaises(self.runner.subprocess.TimeoutExpired):
                    self.runner.perform(binding_path)
                self.assertEqual(verify.call_count, 2, 'CONFIRMATION_RUNNER_RED timeout skipped post-run pins')
                self.assertEqual(invoke.call_args.args[0][0], '/test/pinned/dotnet',
                                 'CONFIRMATION_RUNNER_RED empty environment needs pinned absolute runtime')


if __name__ == '__main__':
    unittest.main(verbosity=2)
