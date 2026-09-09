"""Integration guards for the offline diagnostic; never runs a confirmation runner."""
import hashlib
import json
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
PROBE = ROOT / '.artifacts/intent-ablation/bin/TaskIntentAblation/debug/cnet-task-intent-ablation.dll'
ASSEMBLY = ROOT / '.artifacts/usability-20260910/bin/CnetControlPlane/debug/cnet-control.dll'
DOTNET = '/home/marble/dotnet/dotnet'


def wire(texts):
    return json.dumps([[int.from_bytes(raw[i:i+2], 'little') for i in range(0, len(raw), 2)]
                       for raw in (text.encode('utf-16-le', errors='surrogatepass') for text in texts)]).encode()


class ProbeTests(unittest.TestCase):
    def call(self, payload, digest=None):
        self.assertTrue(PROBE.is_file(), 'INTENT_ABLATION_RED diagnostic missing')
        return subprocess.run([DOTNET, str(PROBE), str(ASSEMBLY), digest or
                               hashlib.sha256(ASSEMBLY.read_bytes()).hexdigest()],
                              input=payload, capture_output=True, timeout=30, env={})

    def test_existing_fallback_is_the_only_changed_path(self):
        r = self.call(wire(["Kindly recode the glyph 'P' toward small letters.", 'uppercase U+0050']))
        self.assertEqual(0, r.returncode, r.stderr)
        arms = json.loads(r.stdout)['arms']
        self.assertEqual({'hybrid', 'grammar', 'guarded_learned_raw'}, set(arms))
        self.assertEqual('abstain', arms['grammar'][0]['Status'])
        self.assertEqual(('ready', 'unicode17_lower_latin1', 80),
                         tuple(arms['hybrid'][0][k] for k in ('Status', 'Dataset', 'Key')))
        self.assertEqual(arms['grammar'][1], arms['hybrid'][1])

    def test_raw_bounds_and_quoted_domain_are_retained_for_every_arm(self):
        rows = ['x' * 257, "uppercase 'a'\n", "uppercase '\ud800'", "uppercase 'hello'",
                "Kindly recode the glyph 'P' toward capitals; erase it.",
                "Kindly recode the glyph 'P' or U+000A toward capitals."]
        r = self.call(wire(rows))
        self.assertEqual(0, r.returncode, r.stderr)
        for arm, proposals in json.loads(r.stdout)['arms'].items():
            for p in proposals:
                self.assertEqual('abstain', p['Status'], (arm, p))
                self.assertIsNone(p['Dataset'])
                self.assertIsNone(p['Key'])

    def test_bad_hash_refuses_without_predictions(self):
        r = self.call(wire(['uppercase a']), '0' * 64)
        self.assertEqual(2, r.returncode)
        self.assertEqual(b'', r.stdout)
        self.assertEqual(b'INTENT_ABLATION_REFUSED\n', r.stderr)

    def test_malformed_transport_is_bounded_and_refused(self):
        for value in ([], [[65536]], [[True]], [[65]] * 129, [[65] * 4097], {'mode': 'live'}):
            r = self.call(json.dumps(value).encode())
            self.assertEqual(2, r.returncode)
            self.assertEqual(b'', r.stdout)


if __name__ == '__main__':
    unittest.main()
