"""Private daemon fixture: 25 raw tables, no model, Discord or live writes."""
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest


class DaemonCapacityTests(unittest.TestCase):
    def test_native_process_reports_loaded_capacity_and_answers_ping(self):
        binary = Path(os.environ.get("CNETD_BIN", "bin/cnetd")).absolute()
        with tempfile.TemporaryDirectory(prefix="cnet-brick-daemon-") as directory:
            root = Path(directory)
            (root / "ROUTES.jsonl").write_text('{"pattern":"fixture","pack":"fixture"}\n')
            bank = root / "bricks"
            bank.mkdir()
            for index in range(25):
                (bank / f"fixture_{index}.lut").write_text(
                    f"tag=fixture_{index}\nlut=" + ",".join(str(k) for k in range(16)) + "\n")
            sock = root / "front.sock"
            env = {"PATH": "/usr/bin:/bin", "LC_ALL": "C", "CNET_PACKS_ROOT": str(root),
                   "CNET_SOCK": str(sock), "CNET_CORE_BUS_BRICKS_DIR": str(bank)}
            process = subprocess.Popen([str(binary)], cwd=root, env=env,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 10
                with socket.socket(socket.AF_UNIX) as client:
                    client.settimeout(2)
                    while True:
                        try:
                            client.connect(str(sock))
                            break
                        except (FileNotFoundError, ConnectionRefusedError):
                            if process.poll() is not None or time.monotonic() >= deadline:
                                self.fail("daemon never became ready")
                            time.sleep(0.02)
                    client.sendall(b"PING\n")
                    reply = b""
                    while b"\n" not in reply and len(reply) < 32:
                        chunk = client.recv(32 - len(reply))
                        if not chunk:
                            break
                        reply += chunk
                    self.assertEqual(reply, b"PONG\n")
            finally:
                if process.poll() is None:
                    process.terminate()
                try:
                    _, errors = process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=5)
                    raise
            events = [json.loads(line) for line in errors.decode().splitlines() if line.startswith('{"event":"brick_bank_loaded"')]
            self.assertEqual(events, [dict(event="brick_bank_loaded", count=25, capacity=256, certified=0)])


if __name__ == "__main__":
    unittest.main()
