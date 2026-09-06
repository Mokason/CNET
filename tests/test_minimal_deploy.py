#!/usr/bin/env python3
"""Private artifact handoff tests. Never invoke the legacy helper's live path."""
import hashlib
import io
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import tarfile
import tempfile
import time
import unittest
from unittest import mock
import zlib

REPO = Path(__file__).resolve().parents[1]
HELPER = REPO / "scripts/deploy_cnet_minimal.sh"


def embedded():
    """Load only trusted helper definitions, without running its CLI."""
    source = HELPER.read_text().split("<<'PY'\n", 1)[1].rsplit("\nPY", 1)[0]
    source = source.split("\nsignal.signal(signal.SIGALRM", 1)[0]
    namespace = {"__name__": "deployment_test"}
    exec(compile(source, str(HELPER), "exec"), namespace)
    return namespace


class MinimalDeploy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = tempfile.TemporaryDirectory(prefix="cnet-deploy-fixture-")
        cls.fixture_root = Path(cls.fixture.name)
        cls.archive = cls.fixture_root / "package.tar.gz"
        # The repaired native packager only builds new private fixture outputs.
        run = subprocess.run(["bash", str(REPO / "scripts/package_cnet_minimal.sh")], cwd=REPO,
                             env={**os.environ, "CNET_MINIMAL_OUT": str(cls.fixture_root / "package"),
                                  "CNET_MINIMAL_TAR": str(cls.archive)},
                             capture_output=True, text=True, timeout=90)
        if run.returncode:
            raise AssertionError("trusted private package fixture failed: " + run.stderr + run.stdout[-1000:])
        cls.digest = hashlib.sha256(cls.archive.read_bytes()).hexdigest()

    @classmethod
    def tearDownClass(cls):
        cls.fixture.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="cnet-deploy-test-")
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def run_helper(self, destination, *, artifact=None, digest=None, dry=False, helper=HELPER):
        args = ["bash", str(helper), "--artifact", str(artifact or self.archive),
                "--destination", str(destination), "--sha256", digest or self.digest]
        if dry:
            args.append("--dry-run")
        return subprocess.run(args, cwd=self.root, capture_output=True, text=True, timeout=90)

    def test_dry_run_checks_without_writes_or_execution(self):
        # Safe actual RED on the old helper: copy it to a private fake repo and
        # stop its first legacy package call before ANY deployment mutation.
        # No broad/default legacy deployment path is ever exercised.
        scripts = self.root / "code/scripts"
        scripts.mkdir(parents=True)
        copied = scripts / "deploy_cnet_minimal.sh"
        shutil.copyfile(HELPER, copied)
        (scripts / "package_cnet_minimal.sh").write_text("#!/usr/bin/env bash\nexit 97\n")
        destination = self.root / "new-deployment"
        before = sorted(str(p.relative_to(self.root)) for p in self.root.rglob("*"))
        run = self.run_helper(destination, dry=True, helper=copied)
        self.assertEqual(run.returncode, 0, "DEPLOY_DRY_RUN_RED explicit validation-only request failed: " + run.stderr)
        self.assertIn("CNET_MINIMAL_DEPLOY_DRY_RUN_PASS", run.stdout)
        self.assertNotIn("CNET_RUNTIME_SMOKE_BEGIN", run.stdout)
        self.assertEqual(before, sorted(str(p.relative_to(self.root)) for p in self.root.rglob("*")))
        self.assertFalse(destination.exists())

    def test_real_private_handoff_keeps_previous_untouched(self):
        previous = self.root / "previous"
        previous.mkdir()
        sentinel = previous / "sentinel"
        sentinel.write_text("unchanged previous deployment")
        destination = self.root / "new-deployment"
        run = self.run_helper(destination)
        self.assertEqual(run.returncode, 0, run.stderr + run.stdout[-1000:])
        self.assertIn("CNET_MINIMAL_DEPLOY_PASS", run.stdout)
        self.assertIn("live_switch=0", run.stdout)
        self.assertEqual(destination.stat().st_mode & 0o777, 0o700)
        self.assertEqual(sentinel.read_text(), "unchanged previous deployment")
        checked = subprocess.run(["sha256sum", "-c", "MANIFEST.sha256"], cwd=destination,
                                 capture_output=True, text=True, timeout=10)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        again = self.run_helper(destination)
        self.assertNotEqual(again.returncode, 0)
        self.assertEqual(sentinel.read_text(), "unchanged previous deployment")

    def test_existing_and_broad_destinations_refuse(self):
        existing = self.root / "existing"
        existing.mkdir()
        (existing / "sentinel").write_text("preserve")
        for destination in (existing, Path("/"), Path.home(), REPO, REPO / "never-deploy-here"):
            with self.subTest(destination=destination):
                run = self.run_helper(destination, dry=True)
                self.assertNotEqual(run.returncode, 0)
                self.assertNotIn("_PASS", run.stdout)
        self.assertEqual((existing / "sentinel").read_text(), "preserve")
        self.assertFalse((REPO / "never-deploy-here").exists())

    def test_other_private_git_workspace_refuses_destination(self):
        checkout = self.root / "other-checkout"
        checkout.mkdir(mode=0o700)
        (checkout / ".git").mkdir()
        parent = checkout / "private"
        parent.mkdir(mode=0o700)
        run = self.run_helper(parent / "new", dry=True)
        self.assertNotEqual(run.returncode, 0, "DEPLOY_OTHER_WORKSPACE_RED accepted another checkout")
        self.assertFalse((parent / "new").exists())

    def test_foreign_owned_ancestor_refuses_without_chown(self):
        ns = embedded()
        ancestor = self.root / "foreign"
        ancestor.mkdir(mode=0o755)
        parent = ancestor / "private"
        parent.mkdir(mode=0o700)
        identity = ancestor.stat()
        real_stat = os.fstat
        def foreign(fd):
            info = real_stat(fd)
            if (info.st_dev, info.st_ino) == (identity.st_dev, identity.st_ino):
                fields = list(info)
                fields[stat.ST_UID] = os.geteuid() + 1
                return os.stat_result(fields)
            return info
        with mock.patch.object(ns["os"], "fstat", foreign):
            with self.assertRaises(ns["Refused"], msg="DEPLOY_ANCESTOR_OWNER_RED foreign owner can rename parent"):
                descriptor = ns["open_directory"](str(parent), private=True)
                os.close(descriptor)

    def test_digest_missing_arguments_and_symlink_boundaries(self):
        run = self.run_helper(self.root / "bad-digest", digest="0" * 64, dry=True)
        self.assertNotEqual(run.returncode, 0)
        self.assertFalse((self.root / "bad-digest").exists())
        missing = subprocess.run(["bash", str(HELPER), "--dry-run"], cwd=self.root,
                                 capture_output=True, text=True, timeout=10)
        self.assertNotEqual(missing.returncode, 0)
        link = self.root / "link"
        link.symlink_to(self.fixture_root, target_is_directory=True)
        for artifact, destination in ((link / "package.tar.gz", self.root / "new"),
                                      (self.archive, link / "new")):
            run = self.run_helper(destination, artifact=artifact, dry=True)
            self.assertNotEqual(run.returncode, 0)
        writable = self.root / "shared"
        writable.mkdir(mode=0o777)
        writable.chmod(0o777)
        self.assertNotEqual(self.run_helper(writable / "new", dry=True).returncode, 0)

    def mutate(self, name, change):
        output = self.root / (name + ".tar.gz")
        with tarfile.open(self.archive, "r:gz") as original:
            members = [(item, original.extractfile(item).read() if item.isfile() else b"") for item in original]
        change(members)
        with tarfile.open(output, "w:gz", format=tarfile.GNU_FORMAT) as archive:
            for item, contents in members:
                archive.addfile(item, io.BytesIO(contents) if item.isfile() else None)
        return output, hashlib.sha256(output.read_bytes()).hexdigest()

    @staticmethod
    def manifest_again(members):
        """A trusted digest cannot excuse an unsupported package shape."""
        manifest = next(item for item, _ in members if item.name == "package/MANIFEST.sha256")
        contents = b"".join((hashlib.sha256(data).hexdigest() + "  ./" + item.name[8:] + "\n").encode()
                            for item, data in members if item.isfile() and item != manifest)
        manifest.size = len(contents)
        members[:] = [(item, contents if item == manifest else data) for item, data in members]

    def test_required_executable_cannot_be_a_directory(self):
        def change(members):
            item = next(item for item, _ in members if item.name == "package/bin/roe_front_door")
            item.type, item.size = tarfile.DIRTYPE, 0
            members[:] = [(entry, b"" if entry == item else data) for entry, data in members]
            self.manifest_again(members)
        artifact, digest = self.mutate("directory-executable", change)
        run = self.run_helper(self.root / "new", artifact=artifact, digest=digest, dry=True)
        self.assertNotEqual(run.returncode, 0, "DEPLOY_REQUIRED_TYPE_RED dry run accepted directory executable")

    def test_smoke_kills_remaining_same_group_children(self):
        scripts = self.root / "scripts"
        scripts.mkdir()
        pid_path = self.root / "child.pid"
        (scripts / "cnet_runtime_smoke.sh").write_text(
            "#!/bin/bash\nsleep 30 >/dev/null 2>&1 &\necho $! > child.pid\n"
            "echo 'CNET_RUNTIME_SMOKE_PASS fixture_only=1 local=9 ood=4 native_dry_run=1 work=fixture'\n")
        ns = embedded()
        child = None
        try:
            ns["smoke"](str(self.root))
            child = int(pid_path.read_text())
            for _ in range(30):
                state = Path(f"/proc/{child}/stat")
                if not state.exists() or state.read_text().split()[2] == "Z":
                    break
                time.sleep(0.01)
            else:
                self.fail("DEPLOY_SMOKE_CHILD_RED completed smoke left its same-group child alive")
        finally:
            if child:
                try:
                    os.kill(child, 9)
                except ProcessLookupError:
                    pass

    def test_smoke_holds_child_identity_until_group_cleanup(self):
        scripts = self.root / "scripts"
        scripts.mkdir()
        (scripts / "cnet_runtime_smoke.sh").write_text(
            "#!/bin/bash\necho 'CNET_RUNTIME_SMOKE_PASS fixture_only=1 local=9 ood=4 native_dry_run=1 work=fixture'\n")
        ns = embedded()
        real_kill = os.killpg
        def identity_checked(pid, signum):
            self.assertTrue(Path(f"/proc/{pid}").exists(),
                            "DEPLOY_CHILD_IDENTITY_RED child was reaped before process-group signal")
            return real_kill(pid, signum)
        with mock.patch.object(ns["os"], "killpg", identity_checked):
            ns["smoke"](str(self.root))

    def test_stage_read_is_stable_and_execution_mode_is_bound(self):
        ns = embedded()
        artifact = self.root / "script"
        artifact.write_bytes(b"bound")
        artifact.chmod(0o600)
        directory = os.open(self.root, ns["DIRECTORY"])
        try:
            entries = {"": (True, b"", 0o700), "script": (False, b"bound", 0o700)}
            with self.assertRaises(ns["Refused"], msg="DEPLOY_STAGE_MODE_RED executable mode not bound"):
                ns["verify_stage"](directory, entries)
        finally:
            os.close(directory)

    def test_stage_root_must_remain_private(self):
        ns = embedded()
        directory = os.open(self.root, ns["DIRECTORY"])
        self.root.chmod(0o755)
        try:
            with self.assertRaises(ns["Refused"], msg="DEPLOY_ROOT_MODE_RED publication root no longer private"):
                ns["verify_stage"](directory, {"": (True, b"", 0o700)})
        finally:
            self.root.chmod(0o700)
            os.close(directory)

    def test_publication_failures_preserve_previous_and_classify_uncertainty(self):
        # Faults replace only trusted Python function references in this test
        # process. Production has no fault environment variables or flags.
        for phase in ("before", "after_io", "after_deadline"):
            with self.subTest(phase=phase):
                ns = embedded()
                destination = self.root / phase
                real_sync = os.fsync
                called = False
                def fault(fd):
                    nonlocal called
                    if not called and ((phase == "before") or destination.exists()):
                        called = True
                        if phase == "after_deadline":
                            raise ns["Refused"]("injected_deadline")
                        raise OSError("injected_fsync")
                    return real_sync(fd)
                ns["smoke"] = lambda _root: None
                args = ["deploy", str(REPO), "--artifact", str(self.archive), "--destination",
                        str(destination), "--sha256", self.digest]
                with mock.patch.object(ns["sys"], "argv", args), mock.patch.object(ns["os"], "fsync", fault):
                    if phase == "before":
                        with self.assertRaises(OSError):
                            ns["main"]()
                        self.assertFalse(destination.exists())
                    else:
                        self.assertEqual(ns["main"](), 3, "DEPLOY_COMMIT_UNCERTAIN_RED")
                        self.assertTrue((destination / "MANIFEST.sha256").is_file())
                self.assertTrue(called)
                self.assertFalse(list(self.root.glob(".cnet-deploy-*")))

    def test_publication_race_does_not_replace_new_target(self):
        ns = embedded()
        destination = self.root / "race"
        library = ns["ctypes"].CDLL(None, use_errno=True)
        real_rename = library.renameat2
        def raced(*args):
            destination.mkdir(mode=0o700)
            (destination / "sentinel").write_text("concurrent owner target")
            return real_rename(*args)
        library.renameat2 = raced
        ns["smoke"] = lambda _root: None
        args = ["deploy", str(REPO), "--artifact", str(self.archive), "--destination",
                str(destination), "--sha256", self.digest]
        with mock.patch.object(ns["sys"], "argv", args), mock.patch.object(ns["ctypes"], "CDLL", return_value=library):
            with self.assertRaises(ns["Refused"]):
                ns["main"]()
        self.assertEqual((destination / "sentinel").read_text(), "concurrent owner target")
        self.assertEqual(sorted(p.name for p in destination.iterdir()), ["sentinel"])
        self.assertFalse(list(self.root.glob(".cnet-deploy-*")))

    def test_interrupt_at_rename_return_or_sync_retains_complete_deployment(self):
        for phase, signum in (("rename", signal.SIGINT), ("sync", signal.SIGINT),
                              ("rename", signal.SIGTERM), ("sync", signal.SIGTERM)):
            with self.subTest(phase=phase, signal=signum):
                ns = embedded()
                destination = self.root / ("interrupt-" + phase + "-" + str(signum))
                library = ns["ctypes"].CDLL(None, use_errno=True)
                real_rename, real_sync = library.renameat2, os.fsync
                interrupted = False
                def rename(*args):
                    result = real_rename(*args)
                    if phase == "rename" and result == 0:
                        os.kill(os.getpid(), signum)
                    return result
                def sync(fd):
                    nonlocal interrupted
                    if phase == "sync" and destination.exists() and not interrupted:
                        interrupted = True
                        os.kill(os.getpid(), signum)
                    return real_sync(fd)
                library.renameat2 = rename
                ns["smoke"] = lambda _root: None
                args = ["deploy", str(REPO), "--artifact", str(self.archive), "--destination",
                        str(destination), "--sha256", self.digest]
                # Keep the default Python SIGINT path covered as well as the
                # actual CLI's handled SIGTERM path; signal only this test.
                previous_handler = signal.getsignal(signum)
                if signum == signal.SIGTERM:
                    signal.signal(signum, ns["interrupted"])
                try:
                    with mock.patch.object(ns["sys"], "argv", args), \
                            mock.patch.object(ns["ctypes"], "CDLL", return_value=library), \
                            mock.patch.object(ns["os"], "fsync", sync):
                        try:
                            outcome = ns["main"]()
                        except BaseException as error:
                            outcome = type(error).__name__
                finally:
                    signal.signal(signum, previous_handler)
                self.assertEqual(outcome, 3, "DEPLOY_INTERRUPT_RED completed rename not classified as retained uncertainty")
                self.assertTrue((destination / "MANIFEST.sha256").is_file())
                checked = subprocess.run(["sha256sum", "-c", "MANIFEST.sha256"], cwd=destination,
                                         capture_output=True, text=True, timeout=10)
                self.assertEqual(checked.returncode, 0, checked.stderr)

    def test_smoke_exit_receipt_output_and_timeout_refuse(self):
        ns = embedded()
        scripts = self.root / "scripts"
        scripts.mkdir()
        script = scripts / "cnet_runtime_smoke.sh"
        receipt = "echo 'CNET_RUNTIME_SMOKE_PASS fixture_only=1 local=9 ood=4 native_dry_run=1 work=fixture'\n"
        cases = {"nonzero": receipt + "exit 7\n", "missing": "echo incomplete\n",
                 "output": "head -c 262145 /dev/zero\n", "timeout": "sleep 30\n"}
        for name, body in cases.items():
            with self.subTest(name=name):
                script.write_text("#!/bin/bash\n" + body)
                ns["SMOKE_SECONDS"] = 0.15 if name == "timeout" else 60
                with self.assertRaises(ns["Refused"]):
                    ns["smoke"](str(self.root))

    def test_smoke_failure_prevents_any_publication(self):
        def change(members):
            item = next(item for item, _ in members if item.name == "package/scripts/cnet_runtime_smoke.sh")
            script = b"#!/bin/bash\necho 'CNET_RUNTIME_SMOKE_PASS fixture_only=1 local=9 ood=4 native_dry_run=1 work=fixture'\nexit 7\n"
            item.size = len(script)
            members[:] = [(entry, script if entry == item else data) for entry, data in members]
            self.manifest_again(members)
        artifact, digest = self.mutate("smoke-nonzero", change)
        destination = self.root / "new"
        run = self.run_helper(destination, artifact=artifact, digest=digest)
        self.assertNotEqual(run.returncode, 0)
        self.assertFalse(destination.exists())
        self.assertFalse(list(self.root.glob(".cnet-deploy-*")))

    def test_compression_metadata_and_size_bounds_refuse(self):
        ns = embedded()
        compressed = self.archive.read_bytes()
        for payload in (compressed[:-1], compressed + b"trailing", compressed + compressed):
            with self.assertRaises(ns["Refused"]):
                ns["unpack_checked"](payload)
        # Exercise the real 128 MiB decompression cap, using repeated compressed
        # chunks so the test producer does not allocate the expanded bomb.
        compressor = zlib.compressobj(wbits=31)
        bomb = b"".join(compressor.compress(bytes(1024 * 1024)) for _ in range(129)) + compressor.flush()
        with self.assertRaises(ns["Refused"]):
            ns["unpack_checked"](bomb)
        for kind in (tarfile.XHDTYPE, tarfile.XGLTYPE, tarfile.GNUTYPE_SPARSE, tarfile.GNUTYPE_LONGLINK):
            header = tarfile.TarInfo("unsupported")
            header.type = kind
            raw = header.tobuf(format=tarfile.GNU_FORMAT) + bytes(1024)
            compressor = zlib.compressobj(wbits=31)
            with self.assertRaises(ns["Refused"]):
                ns["unpack_checked"](compressor.compress(raw) + compressor.flush())
        # Archive acquisition is bounded before allocating its contents.
        oversize = self.root / "oversize.tar.gz"
        with oversize.open("wb") as stream:
            stream.truncate(ns["MAX_ARCHIVE"] + 1)
        with self.assertRaises(ns["Refused"]):
            ns["read_artifact"](str(oversize))

    def test_archive_types_paths_duplicates_and_manifest_refuse(self):
        def extra(members, path, kind=tarfile.REGTYPE, link=""):
            entry = tarfile.TarInfo(path)
            entry.type = kind
            entry.linkname = link
            members.append((entry, b""))
        cases = {
            "traversal": lambda m: extra(m, "package/../escaped"),
            "absolute": lambda m: extra(m, "/absolute-escape"),
            "symlink": lambda m: extra(m, "package/data/link", tarfile.SYMTYPE, "/"),
            "hardlink": lambda m: extra(m, "package/data/link", tarfile.LNKTYPE, "package/VERSION"),
            "fifo": lambda m: extra(m, "package/data/fifo", tarfile.FIFOTYPE),
            "duplicate": lambda m: m.append(next(x for x in m if x[0].name.endswith("/VERSION"))),
            "unmanifested": lambda m: extra(m, "package/data/unlisted"),
            "missing": lambda m: m.__setitem__(slice(None), [x for x in m if not x[0].name.endswith("/BUILD_STATUS")]),
            "second-root": lambda m: extra(m, "another-root/file"),
            "digest": lambda m: m.__setitem__(next(i for i, x in enumerate(m) if x[0].name.endswith("/VERSION")),
                                                   (next(x[0] for x in m if x[0].name.endswith("/VERSION")), b"x" * next(x[0].size for x in m if x[0].name.endswith("/VERSION")))),
        }
        for name, change in cases.items():
            with self.subTest(name=name):
                artifact, digest = self.mutate(name, change)
                destination = self.root / ("out-" + name)
                run = self.run_helper(destination, artifact=artifact, digest=digest, dry=True)
                self.assertNotEqual(run.returncode, 0, "DEPLOY_ARCHIVE_RED accepted " + name)
                self.assertFalse(destination.exists())
        self.assertFalse((self.root / "escaped").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
