"""Private fake installation checks only; dummy artifacts are never executed."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import learning_bridge as lb
from unicode_queries import DATASETS, Reference

SOURCE = Path(__file__).resolve().parents[2] / "data/unicode17/UnicodeData-Latin1.txt"


class InstallationTests(unittest.TestCase):
    def setUp(self):
        # /tmp's writable ancestor deliberately fails the production contract.
        self.tmp = tempfile.TemporaryDirectory(prefix="cnet-bridge-boundary-fixture-", dir=Path.home())
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        for name in ("managed/runtimes/linux-x64/native", "native", "work/data"):
            path = self.root / name
            path.mkdir(parents=True, mode=0o700)
            for parent in (path, *path.parents):
                if parent == self.root:
                    break
                parent.chmod(0o700)
        self.put("dotnet", b"fixture-never-execute")
        (self.root / "dotnet").chmod(0o500)
        self.put("UnicodeData-Latin1.txt", SOURCE.read_bytes())
        self.reference = Reference(self.root / "UnicodeData-Latin1.txt")
        inventories = []
        for directory, names in (("managed", lb.MANAGED), ("native", lb.NATIVE)):
            files = {}
            for name in names:
                self.put(directory + "/" + name, b"fixture")
                files[name] = self.sha(b"fixture")
            inventories.append(files)
        self.put("managed.json", self.encode(dict(schema_version=1, target="linux-x64", files=inventories[0])))
        self.put("runtime.json", self.encode(dict(schema_version=1, files=inventories[1])))
        datasets = [dict(id=name, authority="verified_tool", **(dict(symbol_vocabulary_sha256=self.reference.vocabulary_sha256)
                     if name in DATASETS[2:] else {})) for name in DATASETS]
        self.put("policy.json", self.encode(dict(enabled=True, allocator_enabled=False, datasets=datasets,
                                               max_run_seconds=604800, max_probe_gap_seconds=120)))
        for name in DATASETS:
            self.put("work/data/" + name + ".tsv", b"fixture-source")
        config = dict(schema_version=1, dotnet=str(self.root / "dotnet"),
                      managed_sha256=self.sha((self.root / "managed.json").read_bytes()),
                      native_sha256=self.sha((self.root / "runtime.json").read_bytes()),
                      policy_sha256=self.sha((self.root / "policy.json").read_bytes()),
                      sources={name: self.sha(b"fixture-source") for name in DATASETS})
        self.config = self.encode(config)
        self.put("bridge.json", self.config)

    @staticmethod
    def sha(raw):
        return hashlib.sha256(raw).hexdigest()

    @staticmethod
    def encode(value):
        return (json.dumps(value) + "\n").encode()

    def put(self, name, raw):
        path = self.root / name
        path.write_bytes(raw)
        path.chmod(0o600)

    def test_valid_inventory_and_each_mutable_boundary_refuses_drift(self):
        bridge = lb.Bridge(self.root, self.sha(self.config))
        for name in ("bridge.json", "managed.json", "runtime.json", "policy.json", "native/cnetd",
                     "managed/cnet-control.dll", "work/data/ascii_category.tsv", "UnicodeData-Latin1.txt"):
            path = self.root / name
            original = path.read_bytes()
            path.write_bytes(b"changed")
            with self.subTest(file=name), self.assertRaises(Exception):
                bridge.check_installation()
            path.write_bytes(original)
        bridge.check_installation()

    def test_relative_root_wrong_pin_and_shared_file_refuse(self):
        with patch("os.getcwd", return_value=str(self.root.parent)):
            with self.assertRaises(Exception):
                lb.Bridge(self.root.name, self.sha(self.config))
        with self.assertRaises(Exception):
            lb.Bridge(self.root, "0" * 64)
        path = self.root / "native/cnetd"
        path.chmod(0o644)
        with self.assertRaises(Exception):
            lb.Bridge(self.root, self.sha(self.config))

    def test_symlink_and_hardlink_code_refuse(self):
        path = self.root / "native/cnetd"
        path.rename(self.root / "saved")
        path.symlink_to(self.root / "saved")
        with self.assertRaises(Exception):
            lb.Bridge(self.root, self.sha(self.config))
        path.unlink()
        os.link(self.root / "saved", path)
        with self.assertRaises(Exception):
            lb.Bridge(self.root, self.sha(self.config))

    def test_owner_budget_pause_pin_and_stale_heartbeat_refuse(self):
        bridge = lb.Bridge(self.root, self.sha(self.config))
        status = dict(event="learning_status", paused=False, run_state="running",
                      **{k: bridge.config[k] for k in ("managed_sha256", "native_sha256", "policy_sha256")},
                      run=dict(State="running", Boot="fixture-boot", StartNanoseconds=0, LastNanoseconds=10**9))
        with patch.object(bridge, "command", return_value=status), \
                patch("learning_bridge.time.clock_gettime_ns", return_value=2*10**9), \
                patch("learning_bridge.Path.read_text", return_value="fixture-boot\n"):
            bridge.require_owner()
            for change in (dict(paused=True), dict(run_state="budget_complete"), dict(native_sha256="0"*64),
                           dict(run=dict(State="running", Boot="old-boot", LastNanoseconds=10**9))):
                with patch.object(bridge, "command", return_value=dict(status, **change)), self.assertRaises(lb.BridgeError):
                    bridge.require_owner()
            with patch("learning_bridge.time.clock_gettime_ns", return_value=122*10**9), self.assertRaises(lb.BridgeError):
                bridge.require_owner()

    def test_original_budget_expiry_and_shorter_policy_heartbeat_refuse_before_owner_finishes_cleanup(self):
        bridge = lb.Bridge(self.root, self.sha(self.config))
        bridge.max_run_seconds = 1
        bridge.max_probe_gap_seconds = 120
        status = dict(event="learning_status", paused=False, run_state="running",
                      **{k: bridge.config[k] for k in ("managed_sha256", "native_sha256", "policy_sha256")},
                      run=dict(State="running", Boot="fixture-boot", StartNanoseconds=0, LastNanoseconds=2*10**9))
        with patch.object(bridge, "command", return_value=status), \
                patch("learning_bridge.time.clock_gettime_ns", return_value=2*10**9), \
                patch("learning_bridge.Path.read_text", return_value="fixture-boot\n"):
            with self.assertRaises(lb.BridgeError):
                bridge.require_owner()

    def test_final_budget_sample_follows_boot_identity_io(self):
        bridge = lb.Bridge(self.root, self.sha(self.config))
        bridge.max_run_seconds = 1
        status = dict(event="learning_status", paused=False, run_state="running",
                      **{k: bridge.config[k] for k in ("managed_sha256", "native_sha256", "policy_sha256")},
                      run=dict(State="running", Boot="fixture-boot", StartNanoseconds=0, LastNanoseconds=0))
        now = [10**9-1]
        def delayed_boot_read():
            now[0] = 2*10**9
            return "fixture-boot"
        with patch.object(bridge, "command", return_value=status), \
                patch("learning_bridge.time.clock_gettime_ns", side_effect=lambda *_: now[0]), \
                patch("learning_bridge.Path.read_text", side_effect=delayed_boot_read):
            with self.assertRaises(lb.BridgeError):
                bridge.require_owner()
            bridge.max_run_seconds = 100
            bridge.max_probe_gap_seconds = 1
            status["run"]["LastNanoseconds"] = 0
            with self.assertRaises(lb.BridgeError):
                bridge.require_owner()


if __name__ == "__main__":
    unittest.main()
