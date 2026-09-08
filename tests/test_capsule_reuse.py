"""Actual-native reuse with arithmetic labels, never CNET-generated truth."""
import ctypes
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

CLI = os.environ.get("CNET_REUSE_CLI", "bin/cnet_capsule_core")
LIB = os.environ.get("CNET_TEST_CORE_LIBRARY", "bin/libcnet_capsule_core.so")
if not hasattr(ctypes.CDLL(LIB), "cnet_capsule_core_reuse"):
    raise SystemExit("CAPSULE_REUSE_RED missing_verified_subset_export")


def run(*args):
    return subprocess.run([CLI, *map(str, args)], text=True, capture_output=True, timeout=30)


class CompositionReuse(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="cnet-reuse-")
        cls.root = Path(cls.temporary.name)
        cls.source = cls.root / "source"
        for name, pin, pout, ib, ob, values in (
            ("hours_minutes", "hours", "minutes", 5, 10, [(h, h * 60) for h in range(18)]),
            ("minutes_seconds", "minutes", "seconds", 10, 16, [(h * 60, h * 3600) for h in range(17)]),
            ("unrelated_counter", "sample", "doubled", 8, 8, [(x, x * 2) for x in range(17)]),
        ):
            rows = cls.root / (name + ".tsv")
            rows.write_text("".join(f"{x} {y}\n" for x, y in values))
            result = run("teach", cls.source, name, pin, pout, ib, ob, "verified_tool", rows)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
        for unit, directory in (("hours_minutes", "a-entry"), ("minutes_seconds", "z-tail"), ("unrelated_counter", "m-unused")):
            (cls.source / unit).rename(cls.source / directory)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        self.destination = Path(tempfile.mkdtemp(dir=self.root, prefix="export-"))

    def export(self, source=None, request="capsule hours seconds 2", destination=None):
        return run("reuse", source or self.source, destination or self.destination, request)

    def exported(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        match = re.fullmatch(r"REUSE verified=1 snapshot_sha256=([a-f0-9]{64}) bytes=([1-9][0-9]*) value=7200 hops=2 units=hours_minutes,minutes_seconds\n", result.stdout)
        self.assertIsNotNone(match, result.stdout)
        return self.destination / match[1]

    def refused(self, result):
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("verified=1", result.stdout)
        self.assertEqual(list(self.destination.iterdir()), [])

    def test_exact_subset_offline_replans_covered_values(self):
        exported = self.exported(self.export())
        self.assertEqual({p.name for p in exported.iterdir()}, {"a-entry", "z-tail"})
        for directory in exported.iterdir():
            self.assertEqual({p.name for p in directory.iterdir()}, {"unit.cnb", "manifest.cknow"})
            for artifact in directory.iterdir():
                self.assertEqual(artifact.read_bytes(), (self.source / directory.name / artifact.name).read_bytes())
        for h in range(17):
            result = run("ask", exported, f"capsule hours seconds {h}")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(f"value={h * 3600} hops=2", result.stdout)
        for h in (17, 18, 31):
            self.assertNotEqual(run("ask", exported, f"capsule hours seconds {h}").returncode, 0)
        self.assertEqual(run("ask", self.source, "capsule sample doubled 4").returncode, 0)
        self.assertNotEqual(run("ask", exported, "capsule sample doubled 4").returncode, 0)
        self.assertEqual(self.exported(self.export()), exported)

    def test_uncovered_or_malformed_requests_never_publish(self):
        for request in ("capsule hours seconds 17", "capsule hours seconds 18", "capsule hours seconds -1", "capsule hours missing 2", "capsule hours seconds 2 extra", "", "capsule hours seconds 999999"):
            with self.subTest(request=request):
                self.refused(self.export(request=request))

    def test_source_is_not_required_for_arithmetic_reuse(self):
        source = self.root / self.id().split(".")[-1]
        shutil.copytree(self.source, source)
        exported = self.exported(self.export(source=source))
        shutil.rmtree(source)
        self.assertIn("value=10800 hops=2", run("ask", exported, "capsule hours seconds 3").stdout)

    def test_file_boundary_refusals(self):
        for attack in ("extra", "symlink", "hardlink", "fifo", "writable"):
            with self.subTest(attack=attack):
                source = self.root / (self.id().split(".")[-1] + attack)
                shutil.copytree(self.source, source)
                payload = source / "a-entry" / "unit.cnb"
                if attack == "extra":
                    (payload.parent / "extra").write_text("unexpected")
                elif attack == "symlink":
                    outside = self.root / "symlink-target"
                    payload.rename(outside)
                    payload.symlink_to(outside)
                elif attack == "hardlink":
                    os.link(payload, self.root / "hardlink")
                elif attack == "fifo":
                    payload.unlink()
                    os.mkfifo(payload)
                else:
                    payload.chmod(0o666)
                self.refused(self.export(source=source))

    def test_source_and_destination_path_boundaries(self):
        link = self.destination / "source-link"
        link.symlink_to(self.source, target_is_directory=True)
        result = self.export(source=link)
        self.assertNotEqual(result.returncode, 0)
        link.unlink()
        self.refused(self.export(source=str(self.source) + "/../source"))
        self.refused(self.export(destination=self.root / "missing"))
        self.destination.chmod(0o755)
        self.refused(self.export())
        self.destination.chmod(0o700)

    def test_unselected_or_hidden_malformed_content_is_not_ignored(self):
        for attack in ("unselected-extra", "unselected-link", "hidden", "lock"):
            with self.subTest(attack=attack):
                source = self.root / (self.id().split(".")[-1] + attack)
                shutil.copytree(self.source, source)
                if attack == "unselected-extra":
                    (source / "m-unused" / "extra").write_text("unexpected")
                elif attack == "unselected-link":
                    shutil.rmtree(source / "m-unused")
                    (source / "m-unused").symlink_to(self.source / "m-unused")
                elif attack == "hidden":
                    (source / ".unexpected").mkdir()
                else:
                    (source / ".publish.lock").write_text("not a lock")
                self.refused(self.export(source=source))

    def test_asset_bytes_and_source_freshness_survive_export(self):
        fixture = self.root / self.id().split(".")[-1]
        fixture.mkdir(mode=0o700)
        includes = fixture / "include"
        includes.mkdir(mode=0o700)
        header = includes / "cnet_capsule.h"
        original = ('#define CNET_CAPSULE_SCHEMA 1\n#define CNET_CAPSULE_SCHEMA_ASSET 2\n'
                    '#define CNET_CAPSULE_ASSET_FILE "frontend.cvfa"\n#define CNET_CAPSULE_REASON_MAX 160\n')
        header.write_text(original)
        (includes / "cnet_json_internal.h").write_text('#define CNETD_JSON_DEPTH_MAX 32u\n')
        source = fixture / "capsules"
        source.mkdir(mode=0o700)
        result = subprocess.run(["bin/cnet_source_capsule", "build", str(fixture), str(source / "facts"), "source_facts"],
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        request = "capsule cnet_source_fact cnet_source_answer 0"
        with patch.dict(os.environ, {"CNET_CAPSULE_SOURCE_ROOT": str(fixture)}):
            result = self.export(source=source, request=request)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            digest = re.search(r"snapshot_sha256=([a-f0-9]{64})", result.stdout)[1]
            exported = self.destination / digest
            self.assertEqual((exported / "facts" / "frontend.cvfa").read_bytes(),
                             (source / "facts" / "frontend.cvfa").read_bytes())
            self.assertEqual(run("ask", exported, request).returncode, 0)
            header.write_text(header.read_text().replace("SCHEMA 1", "SCHEMA 3"))
            self.assertNotEqual(run("ask", exported, request).returncode, 0)
            self.assertNotEqual(self.export(source=source, request=request).returncode, 0)
            self.assertEqual(list(self.destination.iterdir()), [exported])
            header.unlink()
            self.assertNotEqual(run("ask", exported, request).returncode, 0)
            self.assertNotEqual(self.export(source=source, request=request).returncode, 0)
            header.write_text(original)
            self.assertEqual(self.export(source=source, request=request).returncode, 0)
            asset = source / "facts" / "frontend.cvfa"
            asset.unlink()
            self.assertNotEqual(self.export(source=source, request=request).returncode, 0)
        self.assertEqual(list(self.destination.iterdir()), [exported])

    def test_changed_existing_digest_is_not_overwritten(self):
        exported = self.exported(self.export())
        payload = exported / "a-entry" / "unit.cnb"
        payload.chmod(0o600)
        payload.write_bytes(b"broken")
        before = hashlib.sha256(payload.read_bytes()).hexdigest()
        self.assertNotEqual(self.export().returncode, 0)
        self.assertEqual(hashlib.sha256(payload.read_bytes()).hexdigest(), before)
        self.assertNotEqual(run("ask", exported, "capsule hours seconds 2").returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
