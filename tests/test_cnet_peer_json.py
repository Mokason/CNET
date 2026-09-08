"""Native JSON client wire tests against a private synthetic UNIX endpoint."""
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import unittest


class PeerJsonTests(unittest.TestCase):
    def exchange(self, query, response):
        binary = Path(os.environ.get("CNET_PEER_BIN", "bin/cnet_peer")).absolute()
        requests, errors = [], []
        with tempfile.TemporaryDirectory(prefix="cnet-peer-json-") as tmp:
            path = str(Path(tmp) / "peer.sock")
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(path)
                server.listen(1)
                server.settimeout(3)
                def respond():
                    try:
                        with server.accept()[0] as client:
                            client.settimeout(3)
                            with client.makefile("rb") as stream:
                                line = stream.readline(65536)
                                requests.append(json.loads(line) if line.startswith(b"{") else line.decode().strip())
                            client.sendall(response)
                    except Exception as exc:
                        errors.append(str(exc))
                worker = threading.Thread(target=respond)
                worker.start()
                result = subprocess.run([str(binary), "--sock", path, "--json", "--peer", 'fixture_"peer', query],
                                        capture_output=True, timeout=5)
                worker.join(4)
            self.assertFalse(worker.is_alive())
            self.assertFalse(errors, errors)
        return requests[0], result

    def test_peer_and_control_characters_roundtrip(self):
        query = 'quoted " text\\with\ttab\nnew line'
        request, result = self.exchange(query, b'{"ok":true}\n')
        self.assertEqual(request, dict(op="ask", q=query, peer='fixture_"peer'), "PEER_JSON_RED")
        self.assertEqual(result.returncode, 0)

    def test_escaped_query_over_wire_limit_refuses_before_connect(self):
        binary = Path(os.environ.get("CNET_PEER_BIN", "bin/cnet_peer")).absolute()
        with tempfile.TemporaryDirectory(prefix="cnet-peer-limit-") as tmp:
            result = subprocess.run([str(binary), "--json", "--sock", str(Path(tmp) / "absent.sock"),
                                     '"\\' * 4000], capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 2, "PEER_JSON_RED")
        self.assertEqual(result.stdout, b"")

    def test_large_reply_not_truncated(self):
        reply = json.dumps(dict(answer="x" * 12000)).encode() + b"\n"
        _, result = self.exchange("fixture query", reply)
        self.assertEqual(result.stdout, reply, "PEER_JSON_RED")

    def test_incomplete_reply_fails_loud(self):
        _, result = self.exchange("fixture query", b'{"ok":true')
        self.assertNotEqual(result.returncode, 0, "PEER_JSON_RED")
        self.assertEqual(result.stdout, b"")

    def test_json_flag_keeps_ping_control_compatible(self):
        request, result = self.exchange("PING", b"PONG\n")
        self.assertEqual(request, "PING")
        self.assertEqual(result.returncode, 0, "PEER_CONTROL_RED")
        self.assertEqual(result.stdout, b"PONG\n")


if __name__ == "__main__":
    unittest.main()
