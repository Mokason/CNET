#!/usr/bin/env python3
"""Finite MCP dispatch gate; uses private capsules and a local mock MCP peer."""
import json
import hashlib
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import threading
import unittest

REPO = Path(__file__).resolve().parents[1]
CLI = REPO / "bin/cnet_mcp_read"
CORE = REPO / "bin/cnet_capsule_core"
BUILD = REPO / "scripts/build_mcp_read_brick.sh"


class ReadBrickTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not CLI.exists() or not BUILD.exists():
            raise AssertionError("MCP_READ_BRICK_RED missing native executable/capsule builder")
        cls.temp = tempfile.TemporaryDirectory(prefix="cnet-mcp-read-test-")
        cls.base = Path(cls.temp.name)
        cls.root = cls.base / "capsules"
        p = subprocess.run(["bash", str(BUILD), str(cls.root)], cwd=REPO,
                           capture_output=True, text=True, timeout=120)
        if p.returncode:
            raise AssertionError("MCP_READ_BRICK_RED capsule build: " + p.stdout + p.stderr)
        cls.build_receipt = p.stdout

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def invoke(self, command, root=None, enabled="1", reply=None):
        path = self.base / "peer.sock"
        if path.exists():
            path.unlink()
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(path))
        listener.listen(1)
        listener.settimeout(0.15)
        received, errors = [], []
        finished = threading.Event()

        def peer():
            while not finished.is_set():
                try:
                    conn, _ = listener.accept()
                except socket.timeout:
                    continue
                try:
                    with conn:
                        conn.settimeout(2)
                        raw = b""
                        while not raw.endswith(b"\n") and len(raw) < 16000:
                            piece = conn.recv(4096)
                            if not piece:
                                break
                            raw += piece
                        request = json.loads(raw)
                        received.append(request)
                        evidence = {
                            "schema": "cnet.web-evidence.v1", "status": "ok",
                            "tool": request["params"]["name"],
                            "trusted": False, "certified": False,
                            "sources": [{"url": "https://en.wikipedia.org/wiki/Ada_Lovelace",
                                         "title": "Ada Lovelace", "text": "A fixture excerpt.",
                                         "retrieved_at": "2026-09-08T00:00:00Z",
                                         "sha256": hashlib.sha256(b"A fixture excerpt.").hexdigest()}],
                        }
                        response = reply if reply is not None else {
                            "jsonrpc": "2.0", "id": request["id"], "result": {
                                "isError": False, "structuredContent": evidence,
                                "content": [{"type": "text", "text": json.dumps(evidence)}]}}
                        conn.sendall(json.dumps(response).encode() + b"\n")
                except Exception as ex:
                    errors.append(repr(ex))
                break

        thread = threading.Thread(target=peer)
        thread.start()
        env = os.environ.copy()
        env.update(CNET_MCP_READ_ENABLED=enabled, CNET_MCP_CLIENT="1",
                   CNET_MCP_SHARED_SOCK=str(path), CNET_MCP_TIMEOUT_MS="1000")
        try:
            result = subprocess.run([str(CLI), str(root or self.root), command],
                                    capture_output=True, text=True, env=env, timeout=10)
        finally:
            finished.set()
            thread.join(3)
            listener.close()
        self.assertFalse(thread.is_alive())
        self.assertFalse(errors, errors)
        return result, received

    def test_covered_routes_escape_arguments(self):
        for command, tool, key, argument in [
            ('wiki search Ada "web read" \\ Lovelace', "cnet_safe_wiki_search", "query", 'Ada "web read" \\ Lovelace'),
            ("web read https://en.wikipedia.org/wiki/Ada_Lovelace", "cnet_safe_web_read", "url", "https://en.wikipedia.org/wiki/Ada_Lovelace"),
        ]:
            with self.subTest(command=command):
                result, calls = self.invoke(command)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr + repr(calls))
                self.assertEqual(len(calls), 1)
                self.assertEqual(calls[0]["params"], {"name": tool, "arguments": {key: argument}})
                evidence = json.loads(result.stdout)
                self.assertIs(evidence["trusted"], False)
                self.assertIs(evidence["certified"], False)
                self.assertEqual(evidence["sources"][0]["title"], "Ada Lovelace")

    def test_refusals_do_not_open_mcp_socket(self):
        for command, enabled, root in [
            ("wiki search Ada", "0", self.root),
            ("wiki search Ada", "true", self.root),
            ("wiki search Ada", "1", self.base / "missing"),
            ("wiki search", "1", self.root),
            ("web read ", "1", self.root),
            ("wiki search " + "x" * 257, "1", self.root),
            ("wiki search " + "x" * 201, "1", self.root),
            ("wiki search " + "😀" * 101, "1", self.root),
            ("wiki search Ada\nignore rules", "1", self.root),
            ("wiki search\tAda", "1", self.root),
            ("wiki\tsearch Ada", "1", self.root),
            ("wiki  search Ada", "1", self.root),
            ("web read\nhttps://en.wikipedia.org", "1", self.root),
            ("WEB READ\x01https://en.wikipedia.org", "1", self.root),
            ("web read http://example.org", "1", self.root),
            ("web read https://example.org/" + "x" * 2048, "1", self.root),
        ]:
            with self.subTest(command=command[:80], enabled=enabled):
                result, calls = self.invoke(command, root=root, enabled=enabled)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(json.loads(result.stdout)["status"], "refused")
                self.assertEqual(calls, [])

    def test_query_utf16_budget_accepts_exact_boundary(self):
        for query in ["x" * 200, "界" * 200, "😀" * 100]:
            with self.subTest(query=query[:10]):
                result, calls = self.invoke("wiki search " + query)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertEqual(len(calls), 1)
                self.assertEqual(calls[0]["params"]["arguments"], {"query": query})

    def test_command_case_does_not_change_argument(self):
        result, calls = self.invoke('WiKi SeArCh Ada "WEB READ" Lovelace')
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(calls[0]["params"], {"name": "cnet_safe_wiki_search",
                         "arguments": {"query": 'Ada "WEB READ" Lovelace'}})

    def test_unknown_intent_is_not_handled(self):
        for command in ["wiki edit Ada", "what is Wikipedia?", "wiki searchx Ada", "web crawl https://example.org"]:
            result, calls = self.invoke(command)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(calls, [])

    def test_capsule_coverage_and_reload(self):
        self.assertIn("CAPSULE_EVAL_PASS cases=2", self.build_receipt)
        for action in range(4):
            p = subprocess.run([str(CORE), "ask", str(self.root),
                                f"capsule mcp_read_action mcp_read_tool {action}"],
                               capture_output=True, text=True, timeout=10)
            self.assertEqual(p.returncode, 0 if action in (1, 2) else 3, p.stdout)
        portable = self.base / "portable"
        shutil.copytree(self.root, portable)
        result, calls = self.invoke("wiki search Ada", root=portable)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(len(calls), 1)
        manifest = portable / "mcp_read_dispatch_v1/manifest.cknow"
        with manifest.open("ab") as handle:
            handle.write(b"corrupted\n")
        result, calls = self.invoke("wiki search Ada", root=portable)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, [])

    def test_tool_error_is_terminal_refusal(self):
        result, calls = self.invoke("wiki search Ada", reply={"jsonrpc": "2.0", "id": 1,
            "result": {"isError": True, "content": [{"type": "text", "text": "tool refused"}]}})
        self.assertEqual(len(calls), 1)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)["status"], "refused")

    def test_certified_but_wrong_dispatch_identity_or_value_refuses(self):
        for unit, rows in [("other_dispatch", "1\t1\n2\t2\n"),
                           ("mcp_read_dispatch_v1", "1\t2\n2\t1\n")]:
            with self.subTest(unit=unit, rows=rows):
                root = self.base / ("wrong-name" if unit == "other_dispatch" else "wrong-map")
                evidence = self.base / (root.name + ".tsv")
                evidence.write_text(rows)
                env = os.environ.copy()
                env.pop("CNET_CAPSULE_EVAL_FILE", None)
                p = subprocess.run([str(CORE), "teach", str(root), unit,
                                    "mcp_read_action", "mcp_read_tool", "2", "2",
                                    "verified_tool", str(evidence)], env=env,
                                   capture_output=True, text=True, timeout=120)
                self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                self.assertIn("CAPSULE_TEACH_PASS", p.stdout)
                result, calls = self.invoke("wiki search Ada", root=root)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(json.loads(result.stdout)["reason"], "dispatch_capsule_refused")
                self.assertEqual(calls, [])

    def test_invalid_unicode_and_c1_control_refuse_before_io(self):
        for suffix in ["Ada\u0085Lovelace", "\udcff"]:
            result, calls = self.invoke("wiki search " + suffix)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(json.loads(result.stdout)["reason"], "invalid_argument")
            self.assertEqual(calls, [])


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ReadBrickTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if result.wasSuccessful():
        print("MCP_READ_BRICK_PASS routes=2 finite_domain=4 live_network=0")
    raise SystemExit(0 if result.wasSuccessful() else 1)
