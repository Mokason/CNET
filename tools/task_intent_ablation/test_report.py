import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('diagnostic', Path(__file__).with_name('run_diagnostic.py'))
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)


class ReportTests(unittest.TestCase):
    def test_changed_pin_never_publishes_success(self):
        root = diagnostic.ROOT
        corpus = root / 'result/cnet_learned_intent_proposer_20260910/confirmation-v2/evidence/private/confirmation.json'
        real_digest = diagnostic.digest
        probe_reads = 0

        def changing_digest(path):
            nonlocal probe_reads
            if path == diagnostic.PROBE:
                probe_reads += 1
                if probe_reads > 1:
                    return '0' * 64  # Simulated post-run identity drift, no artifact mutation.
            return real_digest(path)

        with tempfile.TemporaryDirectory(prefix='cnet-ablation-guard-') as temporary:
            output = Path(temporary) / 'attempt'
            args = ['diagnostic', '--assembly', str(root / '.artifacts/usability-20260910/bin/CnetControlPlane/debug/cnet-control.dll'),
                    '--corpus', str(corpus), '--corpus-sha256', real_digest(corpus), '--output', str(output)]
            with patch('sys.argv', args), patch.object(diagnostic, 'digest', side_effect=changing_digest):
                with self.assertRaisesRegex(ValueError, 'diagnostic_pin_changed'):
                    diagnostic.main()
            self.assertTrue((output / 'response.json').exists())
            self.assertFalse((output / 'result.json').exists(), 'ABLATION_REPORT_RED invalidated result published')


if __name__ == '__main__':
    unittest.main()
