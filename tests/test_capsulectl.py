#!/usr/bin/env python3
"""Bounded owner-control client checks against independently scripted sockets."""
from contextlib import contextmanager
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import unittest

REPO = Path(__file__).resolve().parents[1]
CLI = Path(os.environ.get("CNET_CAPSULECTL_TEST_BIN", REPO / "bin/cnet_capsulectl"))
HASH = "a" * 64
OK = f"OK revision=1 active={HASH} rollback=- staged=- durable=1 reason=ok\n".encode()
ERR = f"ERR revision=1 active={HASH} rollback=- staged=- durable=1 reason=refused\n".encode()


class CapsuleControlCli(unittest.TestCase):
    def setUp(self):
        self.assertTrue(CLI.is_file(), "CAPSULECTL_RED native owner-control client missing")
        self.temp = tempfile.TemporaryDirectory(prefix="cnet-control-cli-")
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)

    def run_cli(self, path, *arguments, binary=CLI, timeout=3, env=None):
        return subprocess.run([str(binary), str(path), *arguments], cwd=REPO,
                              capture_output=True, timeout=timeout, env=env)

    @contextmanager
    def endpoint(self, response=OK, *, permissions=0o600, drip=False):
        path = self.root / "control.sock"
        captured = []
        stopped = threading.Event()
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(str(path))
        path.chmod(permissions)
        server.listen(1)
        server.settimeout(.5)

        def serve():
            try:
                client, _ = server.accept()
                with client:
                    client.settimeout(1)
                    request = bytearray()
                    while b"\n" not in request and len(request) < 512:
                        part = client.recv(512)
                        if not part:
                            break
                        request.extend(part)
                    captured.append(bytes(request))
                    if drip:
                        for byte in response:
                            if stopped.wait(.1):
                                break
                            client.sendall(bytes([byte]))
                    else:
                        client.sendall(response)
            except (OSError, TimeoutError):
                pass

        thread = threading.Thread(target=serve)
        thread.start()
        try:
            yield path, captured
        finally:
            stopped.set()
            thread.join(timeout=2)
            server.close()
            if path.exists() or path.is_symlink():
                path.unlink()
            self.assertFalse(thread.is_alive(), "test socket thread not joined")

    def test_status_and_exact_request(self):
        with self.endpoint() as (path, captured):
            run = self.run_cli(path, "STATUS")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, OK)
        self.assertEqual(captured, [b"STATUS\n"])

    def test_all_named_operations_have_fixed_argument_shapes(self):
        cases = [("STAGE", "1", "1", "known_set"),
                 ("ACTIVATE", "1", "request-1", HASH), ("UNLOAD", "1", "request-1"),
                 ("ROLLBACK", "1", "request-1"), ("DISCARD", "1", HASH)]
        for args in cases:
            with self.subTest(args=args), self.endpoint() as (path, captured):
                run = self.run_cli(path, *args)
                self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(captured, [(" ".join(args) + "\n").encode()])

    def test_server_refusal_is_not_reported_as_success(self):
        with self.endpoint(ERR) as (path, _):
            run = self.run_cli(path, "UNLOAD", "1", "request")
        self.assertEqual(run.returncode, 1)
        self.assertEqual(run.stdout, ERR)

    def test_uncertain_publication_and_volatile_status_are_distinct(self):
        uncertain = ERR.replace(b"durable=1 reason=refused", b"durable=0 reason=durability_uncertain")
        with self.endpoint(uncertain) as (path, _):
            run = self.run_cli(path, "UNLOAD", "1", "same-request")
        self.assertEqual(run.returncode, 1)
        self.assertEqual(run.stdout, uncertain)
        volatile = OK.replace(b"durable=1", b"durable=0")
        with self.endpoint(volatile) as (path, _):
            run = self.run_cli(path, "STATUS")
        self.assertEqual(run.returncode, 0)
        self.assertEqual(run.stdout, volatile)

    def test_invalid_arguments_never_reach_socket(self):
        bad = [("STATUS", "extra"), ("ASK",), ("STAGE", "1", "2", "set"),
               ("STAGE", "1", "0", "../escape"), ("STAGE", "1", "0", "a\nSTATUS"),
               ("STAGE", "01", "0", "set"), ("UNLOAD", "0", "x"),
               ("UNLOAD", "18446744073709551616", "x"), ("UNLOAD", "1", "x" * 64),
               ("ACTIVATE", "1", "id", HASH.upper()), ("DISCARD", "1", "short")]
        with self.endpoint() as (path, captured):
            for args in bad:
                with self.subTest(args=args):
                    run = self.run_cli(path, *args)
                    self.assertEqual(run.returncode, 2)
                    self.assertEqual(run.stdout, b"")
        self.assertEqual(captured, [])

    def test_socket_and_parent_paths_must_be_private_and_nofollow(self):
        with self.endpoint() as (path, captured):
            link = self.root / "alias.sock"
            link.symlink_to(path)
            parent_link = self.root / "parent"
            parent_link.symlink_to(self.root, target_is_directory=True)
            for bad in (link, parent_link / path.name, Path("relative.sock")):
                self.assertEqual(self.run_cli(bad, "STATUS").returncode, 2)
            self.root.chmod(0o755)
            self.assertEqual(self.run_cli(path, "STATUS").returncode, 2)
            self.root.chmod(0o700)
        self.assertEqual(captured, [])
        with self.endpoint(permissions=0o666) as (path, captured):
            self.assertEqual(self.run_cli(path, "STATUS").returncode, 2)
        self.assertEqual(captured, [])

    def test_foreign_peer_refuses_before_request_bytes(self):
        # Safe syscall-result fault injection: no privilege or other user's
        # process is needed to exercise the actual pre-send peer check.
        source = self.root / "foreign-peer.c"
        library = self.root / "foreign-peer.so"
        source.write_text(r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/socket.h>
int getsockopt(int fd, int level, int option, void *value, socklen_t *size) {
    int (*real_get)(int,int,int,void *,socklen_t *) = dlsym(RTLD_NEXT,"getsockopt");
    if (!real_get) return -1;
    int rc = real_get(fd,level,option,value,size);
    if (!rc && level == SOL_SOCKET && option == SO_PEERCRED && *size == sizeof(struct ucred))
        ((struct ucred *)value)->uid ^= 1;
    return rc;
}
''')
        build = subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                                str(source), "-ldl", "-o", str(library)],
                               capture_output=True, timeout=30)
        self.assertEqual(build.returncode, 0, build.stderr)
        with self.endpoint() as (path, captured):
            run = self.run_cli(path, "UNLOAD", "1", "request",
                               env={**os.environ, "LD_PRELOAD": str(library)})
        self.assertEqual(run.returncode, 2)
        self.assertEqual(run.stdout, b"")
        self.assertEqual(captured, [b""])
        self.assertIn(b"peer identity", run.stderr)
        self.assertNotIn(b"outcome unknown", run.stderr)

    def test_malformed_oversized_trailing_and_incomplete_responses_refuse(self):
        for response in (b"OK\n", OK + b"extra\n", OK[:-1], b"x" * 4096,
                         OK.replace(b"durable=1", b"durable=2"),
                         OK.replace(b"active=", b"active=\x00")):
            with self.subTest(response=response[:30]), self.endpoint(response) as (path, _):
                run = self.run_cli(path, "ACTIVATE", "1", "same-request", HASH)
            self.assertEqual(run.returncode, 2)
            self.assertEqual(run.stdout, b"")
            self.assertIn(b"outcome unknown", run.stderr)

    def test_one_deadline_bounds_slow_response(self):
        binary = self.root / "short-deadline-client"
        compile_run = subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
            "-DCNET_CAPSULECTL_TESTING", "-Iinclude", "-Isrc/serve",
            "tools/cnet_capsulectl.c", "-Lbin", "-lcnet_capsule_core",
            f"-Wl,-rpath,{REPO / 'bin'}", "-o", str(binary)],
            cwd=REPO, capture_output=True, timeout=30)
        self.assertEqual(compile_run.returncode, 0, compile_run.stderr)
        with self.endpoint(OK, drip=True) as (path, _):
            start = time.monotonic()
            run = self.run_cli(path, "UNLOAD", "1", "same-request", binary=binary)
            elapsed = time.monotonic() - start
        self.assertEqual(run.returncode, 2)
        self.assertLess(elapsed, 1)
        self.assertIn(b"outcome unknown", run.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
