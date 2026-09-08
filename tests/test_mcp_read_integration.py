"""Real native capsule -> UNIX transport -> real stdio MCP server.

Default cases never access the public network. CNET_MCP_LIVE_TEST=1 adds one
explicit Wikipedia smoke; it never changes the shared MCP/daemon deployment.
"""
import hashlib
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "dotnet/CnetMcpServer/bin/Debug/net10.0/CnetMcpServer.dll"


class IntegrationTests(unittest.TestCase):
    def exercise(self, query, enabled):
        self.assertTrue(SERVER.is_file(), "build the managed MCP server first")
        with tempfile.TemporaryDirectory(prefix="cnet-read-e2e-") as directory:
            root = Path(directory)
            capsule_root = root / "capsules"
            env = {"PATH": os.environ["PATH"], "LC_ALL": "C.UTF-8",
                   "CNET_BASE_PATH": str(root / "missing.cnb"), "CNET_HEALTH_TICK_SECONDS": "0",
                   "CNET_MCP_READ_ENABLED": enabled, "CNET_MCP_READ_HOSTS": "en.wikipedia.org"}
            build = subprocess.run(["bash", "scripts/build_mcp_read_brick.sh", str(capsule_root)],
                                   cwd=ROOT, env=env, capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            with (root / "mcp.stderr").open("wb") as err:
                server = subprocess.Popen(["dotnet", str(SERVER)], stdin=subprocess.PIPE,
                                          stdout=subprocess.PIPE, stderr=err, env=env)
                peer_errors, observed = [], []

                def frame(request):
                    server.stdin.write((json.dumps(request) + "\n").encode())
                    server.stdin.flush()
                    self.assertTrue(select.select([server.stdout], [], [], 15)[0], "MCP reply deadline")
                    line = server.stdout.readline(262144)
                    self.assertTrue(line.endswith(b"\n"), "bounded complete MCP frame")
                    return json.loads(line)

                try:
                    init = frame({"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {
                        "protocolVersion": "2025-11-25", "capabilities": {},
                        "clientInfo": {"name": "CNET-read-integration", "version": "1"}}})
                    self.assertIn("result", init)
                    server.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
                    server.stdin.flush()
                    path = root / "peer.sock"
                    with socket.socket(socket.AF_UNIX) as listener:
                        listener.bind(str(path)); listener.listen(1); listener.settimeout(3)

                        def bridge():
                            try:
                                conn, _ = listener.accept()
                                with conn:
                                    conn.settimeout(15)
                                    line = b""
                                    while not line.endswith(b"\n"):
                                        chunk = conn.recv(16384)
                                        if not chunk: break
                                        line += chunk
                                        if len(line) > 16384: raise ValueError("request budget")
                                    request = json.loads(line)
                                    response = frame(request)
                                    observed.append(response)
                                    conn.sendall((json.dumps(response) + "\n").encode())
                            except Exception as exc:
                                peer_errors.append(repr(exc))

                        thread = threading.Thread(target=bridge)
                        thread.start()
                        native_env = dict(env, CNET_MCP_READ_ENABLED="1", CNET_MCP_CLIENT="1",
                                          CNET_MCP_SHARED_SOCK=str(path), CNET_MCP_TIMEOUT_MS="12000")
                        native = subprocess.run([str(ROOT / "bin/cnet_mcp_read"), str(capsule_root), query],
                                                env=native_env, capture_output=True, text=True, timeout=20)
                        thread.join(16)
                        self.assertFalse(thread.is_alive())
                        self.assertFalse(peer_errors, peer_errors)
                    self.assertEqual(len(observed), 1)
                    return native, observed[0]
                finally:
                    server.stdin.close()
                    try: server.wait(5)
                    except subprocess.TimeoutExpired:
                        server.kill(); server.wait(5)
                    server.stdout.close()

    def test_real_server_disabled_policy_refuses_native_call(self):
        native, rpc = self.exercise("wiki search Quasicrystal", "0")
        self.assertNotEqual(native.returncode, 0)
        self.assertEqual(json.loads(native.stdout)["status"], "refused")
        self.assertTrue(rpc["result"]["isError"])
        self.assertEqual(rpc["result"]["structuredContent"]["status"], "disabled")

    def test_real_server_blocks_private_url_with_policy_enabled(self):
        native, rpc = self.exercise("web read https://127.0.0.1/secret", "1")
        self.assertNotEqual(native.returncode, 0)
        self.assertEqual(rpc["result"]["structuredContent"]["status"], "url_refused")

    @unittest.skipUnless(os.environ.get("CNET_MCP_LIVE_TEST") == "1", "explicit public-network opt-in")
    def test_live_wikipedia_through_certified_native_brick(self):
        native, rpc = self.exercise("wiki search Quasicrystal", "1")
        self.assertEqual(native.returncode, 0, native.stdout + native.stderr + json.dumps(rpc))
        evidence = json.loads(native.stdout)
        self.assertEqual(evidence, rpc["result"]["structuredContent"])
        self.assertFalse(evidence["certified"])
        self.assertFalse(evidence["trusted"])
        self.assertEqual(evidence["status"], "ok")
        self.assertGreater(len(evidence["sources"]), 0)
        for source in evidence["sources"]:
            self.assertEqual(source["sha256"], hashlib.sha256(source["text"].encode()).hexdigest())
        print("MCP_READ_LIVE_PASS " + json.dumps({"sources": [
            {key: source[key] for key in ("url", "title", "sha256", "retrieved_at")}
            for source in evidence["sources"]], "certified": False, "trusted": False}))


if __name__ == "__main__":
    unittest.main()
