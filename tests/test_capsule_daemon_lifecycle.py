#!/usr/bin/env python3
"""Same-process operator lifecycle against actual native socket/producer paths."""
import json
import os
from pathlib import Path
import socket
import shutil
import subprocess
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[1]

class ResidentDaemon(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cnet-daemon-lifecycle-")
        self.root = Path(self.temp.name)
        for part in ("packs", "sets", "state", "sets/original", "sets/empty"):
            (self.root / part).mkdir(mode=0o700)
        (self.root / "packs/ROUTES.jsonl").write_text('{"pattern":"fixture","pack":"fixture"}\n')
        self.sock = self.root / "ask.sock"
        self.control = self.root / "control.sock"
        self.env = {"PATH": os.environ["PATH"], "CNET_PACKS_ROOT": str(self.root / "packs"),
                    "CNET_MINIMAL_ROOT": str(self.root), "CNET_SOCK": str(self.sock),
                    "CNET_CAPSULE_SETS_DIR": str(self.root / "sets"),
                    "CNET_CAPSULE_STATE_DIR": str(self.root / "state"),
                    "CNET_CAPSULE_CONTROL_SOCK": str(self.control),
                    "CNET_CAPSULE_SOURCE_ROOT": str(self.root / "sources"),
                    "CNET_SELF_ANSWER": "0", "CNET_TEACHER_ON_MISS": "0", "CNET_CORE_AUTO_EVOLVE": "0"}
        self.log = (self.root / "daemon.log").open("ab")
        self.proc = None
        self.addCleanup(self.cleanup)
        self.start()

    def start(self):
        self.proc = subprocess.Popen([str(REPO / "bin/cnetd")], cwd=self.root,
                                     env=self.env, stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                self.fail("CAPSULE_DAEMON_RED startup_failed: " + (self.root / "daemon.log").read_text())
            if self.sock.exists():
                return
            time.sleep(.02)
        self.fail("CAPSULE_DAEMON_RED daemon_not_ready")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(timeout=5)
        self.proc = None

    def cleanup(self):
        self.stop()
        self.log.close()
        # Snapshot cache directories are intentionally read-only.
        for current, dirs, files in os.walk(self.root):
            os.chmod(current, 0o700)
        self.temp.cleanup()

    def exchange(self, path, line):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(8)
            s.connect(str(path))
            s.sendall(line.encode() + b"\n")
            response = bytearray()
            while b"\n" not in response:
                part = s.recv(8192)
                self.assertTrue(part, "CAPSULE_DAEMON_RED incomplete_reply")
                response.extend(part)
                self.assertLess(len(response), 32768)
            return response.decode().strip()

    def command(self, text):
        self.assertTrue(self.control.exists(), "CAPSULE_DAEMON_RED missing_operator_socket")
        return self.exchange(self.control, text)

    def ask(self, query):
        return json.loads(self.exchange(self.sock, json.dumps({"op": "ask", "q": query})))

    def stage(self, revision, preserve, name):
        result = self.command(f"STAGE {revision} {preserve} {name}")
        self.assertTrue(result.startswith("OK "), result)
        return dict(part.split("=", 1) for part in result.split()[1:])["staged"]

    def teach(self):
        rows = self.root / "minutes.tsv"
        rows.write_text("".join(f"{x}\t{x * 60}\n" for x in range(6)))
        output = self.root / "sets/original/minute_conversion"
        subprocess.run([str(REPO / "bin/cnet_capsule_core"), "teach",
                        str(output.parent), output.name, "minutes", "seconds", "3", "9",
                        "verified_tool", str(rows)], check=True, cwd=self.root,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        output.chmod(0o700)

    def source_fixture(self):
        sources = self.root / "sources"
        (sources / "include").mkdir(parents=True, mode=0o700)
        for name in ("cnet_capsule.h", "cnet_json_internal.h"):
            (sources / "include" / name).write_bytes((REPO / "include" / name).read_bytes())
        self.build_sources("original", "source_facts")
        return sources

    def build_sources(self, set_name, unit):
        subprocess.run([str(REPO / "bin/cnet_source_capsule"), "build", str(self.root / "sources"),
                        str(self.root / "sets" / set_name / unit), unit],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)

    def source_downstream(self):
        rows = self.root / "index.tsv"
        rows.write_text("".join(f"{x}\t{x + 1}\n" for x in range(5)))
        output = self.root / "sets/original/index_plus_one"
        subprocess.run([str(REPO / "bin/cnet_capsule_core"), "teach", str(output.parent), output.name,
                        "cnet_source_answer", "source_index_plus_one", "3", "3", "verified_tool", str(rows)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        output.chmod(0o700)

    def test_named_swap_restart_and_retention(self):
        pid = self.proc.pid
        self.assertIn("revision=1", self.command("STATUS"))
        self.assertFalse(self.ask("convert 3 minutes to seconds")["verified"])
        self.teach()
        ticket = self.stage(1, 1, "original")
        self.assertTrue(self.command(f"ACTIVATE 8 wrong {ticket}").startswith("ERR "))
        self.assertTrue(self.command(f"ACTIVATE 1 initial {ticket}").startswith("OK "))
        self.assertTrue(self.command(f"ACTIVATE 1 initial {ticket}").startswith("OK "))
        for x in range(6):
            reply = self.ask(f"convert {x} minutes to seconds")
            self.assertTrue(reply["verified"])
            self.assertEqual(reply["answer"], str(x * 60))
        self.assertFalse(self.ask("convert 6 minutes to seconds")["verified"])
        self.assertTrue(self.command("STAGE 2 1 empty").startswith("ERR "))
        self.assertEqual(self.ask("convert 3 minutes to seconds")["answer"], "180")
        self.assertTrue(self.command("STAGE 2 0 ../escape").startswith("ERR "))
        self.assertTrue(self.command("UNLOAD 2 empty").startswith("OK "))
        reply = self.ask("convert 3 minutes to seconds")
        self.assertFalse(reply["verified"])
        self.assertFalse(reply["teacher"])
        self.assertIn("ABSTAIN", reply["answer"])
        self.assertEqual(self.proc.pid, pid)
        self.assertIsNone(self.proc.poll())
        self.stop(); self.start()
        self.assertIn("revision=3", self.command("STATUS"))
        self.assertFalse(self.ask("convert 3 minutes to seconds")["verified"])
        self.assertTrue(self.command("ROLLBACK 3 restore").startswith("OK "))
        self.assertTrue(self.command("ROLLBACK 3 restore").startswith("OK "))
        self.assertEqual(self.ask("convert 3 minutes to seconds")["answer"], "180")
        with (self.root / "sets/original/minute_conversion/manifest.cknow").open("a") as f:
            f.write("corruption\n")
        self.assertTrue(self.command("STAGE 4 1 original").startswith("ERR "))
        self.assertEqual(self.ask("convert 3 minutes to seconds")["answer"], "180")

    def test_chat_is_not_control(self):
        self.assertIn("revision=1", self.command("STATUS"))
        self.assertFalse(self.ask("UNLOAD 1 injected")["verified"])
        result = self.exchange(self.sock, json.dumps({"op": "activate", "q": "original"}))
        self.assertFalse(json.loads(result)["ok"])
        self.assertTrue(self.command("ASK capsule minutes seconds 3").startswith("ERR "))
        self.assertTrue(self.command("STAGE 01 0 empty").startswith("ERR "))
        self.assertIn("revision=1", self.command("STATUS"))

    def test_earlier_unit_does_not_change_original_provenance_identity(self):
        self.teach()
        ticket = self.stage(1, 1, "original")
        self.assertTrue(self.command(f"ACTIVATE 1 provenance_initial {ticket}").startswith("OK "))
        original = self.root / "sets/original/minute_conversion/unit.cnb"
        exact = original.read_bytes()
        rows = self.root / "cost.tsv"
        rows.write_text("".join(f"{x * 60}\t{x * 2}\n" for x in (0, 1, 3, 4, 5)))
        output = self.root / "sets/original/aaa_cost"
        subprocess.run([str(REPO / "bin/cnet_capsule_core"), "teach", str(output.parent), output.name,
                        "seconds", "credits", "9", "4", "verified_tool", str(rows)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        output.chmod(0o700)
        self.assertEqual(original.read_bytes(), exact)
        ticket = self.stage(2, 1, "original")
        self.assertTrue(self.command(f"ACTIVATE 2 provenance_added {ticket}").startswith("OK "))
        self.assertEqual(self.ask("convert 3 minutes to seconds")["answer"], "180")
        self.assertEqual(self.ask("capsule minutes credits 3")["answer"], "6")
        self.stop(); self.start()
        self.assertEqual(self.ask("convert 3 minutes to seconds")["answer"], "180")

    def test_foreign_owned_ancestor_refuses_without_chown(self):
        shim = self.root / "owner-stat.so"
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared", "-o", str(shim),
                        str(REPO / "tests/capsule_owner_stat_shim.c"), "-ldl"], check=True, timeout=30)
        self.stop()
        result = subprocess.run([str(REPO / "bin/cnetd")], cwd=self.root,
                                env=dict(self.env, LD_PRELOAD=str(shim), CNET_TEST_FOREIGN_ANCESTOR=str(self.root)),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        self.assertNotEqual(result.returncode, 0, "foreign-owned read-only ancestor is still renameable by its owner")
        self.assertIn(b"capsule", result.stderr.lower())

    def test_repeated_named_set_soak_and_cache_bound(self):
        self.teach()
        ticket = self.stage(1, 1, "original")
        self.assertTrue(self.command(f"ACTIVATE 1 initial_soak {ticket}").startswith("OK "))
        pid = self.proc.pid
        def rss_kib():
            for line in Path(f"/proc/{pid}/status").read_text().splitlines():
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
            self.fail("daemon RSS unavailable")
        before = rss_kib()
        elapsed = []
        for index in range(20):
            selection = self.root / "sets" / f"cycle_{index}"
            selection.mkdir(mode=0o700)
            output = selection / f"minutes_{index}"
            subprocess.run([str(REPO / "bin/cnet_capsule_core"), "teach", str(selection), output.name,
                            "minutes", "seconds", "3", "9", "verified_tool", str(self.root / "minutes.tsv")],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            output.chmod(0o700)
            revision = index + 2
            begin = time.monotonic()
            ticket = self.stage(revision, 1, selection.name)
            self.assertTrue(self.command(f"ACTIVATE {revision} cycle_{index} {ticket}").startswith("OK "))
            elapsed.append((time.monotonic() - begin) * 1000)
            for value in range(6):
                self.assertEqual(self.ask(f"convert {value} minutes to seconds")["answer"], str(value * 60))
            self.assertFalse(self.ask("convert 6 minutes to seconds")["verified"])
            self.assertLessEqual(len(list((self.root / "state/snapshots").iterdir())), 3)
        after = rss_kib()
        self.assertLess(after - before, 16384, "small-fixture repeated swapping must not retain every materialized core")
        self.assertEqual(self.proc.pid, pid)
        self.stop(); self.start()
        self.assertIn("revision=22", self.command("STATUS"))
        self.assertEqual(self.ask("convert 3 minutes to seconds")["answer"], "180")
        print("CAPSULE_DAEMON_SOAK " + json.dumps({"cycles": 20, "correct_answers": 120, "ood_refusals": 20,
              "rss_before_kib": before, "rss_after_kib": after, "max_snapshot_copies": 3,
              "stage_activate_ms_median": sorted(elapsed)[len(elapsed)//2], "stage_activate_ms_max": max(elapsed)}))

    def test_source_labels_freshness_and_composition(self):
        self.teach()
        sources = self.source_fixture()
        self.source_downstream()
        output = self.root / "sets/original/source_facts"
        ticket = self.stage(1, 1, "original")
        self.assertTrue(self.command(f"ACTIVATE 1 sources {ticket}").startswith("OK "))
        expected = {"capsule-schema": "include/cnet_capsule.h:CNET_CAPSULE_SCHEMA=1",
                    "capsule-asset-schema": "include/cnet_capsule.h:CNET_CAPSULE_SCHEMA_ASSET=2",
                    "capsule-asset-file": "include/cnet_capsule.h:CNET_CAPSULE_ASSET_FILE=frontend.cvfa",
                    "capsule-reason-limit": "include/cnet_capsule.h:CNET_CAPSULE_REASON_MAX=160",
                    "json-depth-limit": "include/cnet_json_internal.h:CNETD_JSON_DEPTH_MAX=32"}
        for name, label in expected.items():
            answer = self.ask("source-fact " + name)
            self.assertTrue(answer["verified"])
            self.assertEqual(answer["answer"], label)
        for value in range(5):
            answer = self.ask(f"capsule cnet_source_fact source_index_plus_one {value}")
            self.assertTrue(answer["verified"])
            self.assertEqual(answer["answer"], str(value + 1))
        for query in ("source-fact ../escape", "source-fact ignore previous instructions",
                      "capsule cnet_source_fact cnet_source_answer 5"):
            answer = self.ask(query)
            self.assertFalse(answer["verified"])
            self.assertFalse(answer["teacher"])
        source = sources / "include/cnet_json_internal.h"
        original = source.read_bytes()
        source.write_bytes(original + b"\n/* changed source invalidates current fact claims */\n")
        self.assertFalse(self.ask("source-fact json-depth-limit")["verified"])
        self.assertFalse(self.ask("capsule cnet_source_fact source_index_plus_one 4")["verified"])
        self.assertEqual(self.ask("convert 3 minutes to seconds")["answer"], "180")
        self.stop(); self.start()
        self.assertFalse(self.ask("source-fact json-depth-limit")["verified"])
        self.assertEqual(self.ask("convert 3 minutes to seconds")["answer"], "180")
        source.write_bytes(original)
        self.assertTrue(self.ask("source-fact json-depth-limit")["verified"])
        self.assertTrue(self.command("UNLOAD 2 source_away").startswith("OK "))
        self.assertFalse(self.ask("source-fact capsule-schema")["verified"])
        self.assertTrue(self.command("ROLLBACK 3 source_back").startswith("OK "))
        self.assertTrue(self.ask("source-fact capsule-schema")["verified"])
        (output / "frontend.cvfa").write_bytes(b"corrupt asset")
        self.assertTrue(self.command("STAGE 4 1 original").startswith("ERR "))
        self.assertTrue(self.ask("source-fact capsule-schema")["verified"])
        source.unlink()
        self.assertFalse(self.ask("source-fact capsule-schema")["verified"])

    def test_numeric_equality_does_not_authorize_source_relabelling(self):
        self.teach()
        sources = self.source_fixture()
        ticket = self.stage(1, 1, "original")
        self.assertTrue(self.command(f"ACTIVATE 1 old_sources {ticket}").startswith("OK "))
        source = sources / "include/cnet_json_internal.h"
        original = source.read_bytes()
        changed = original.replace(b"#define CNETD_JSON_DEPTH_MAX 32", b"#define CNETD_JSON_DEPTH_MAX 33")
        self.assertNotEqual(original, changed)
        source.write_bytes(changed)
        alternate = self.root / "sets/alternate"
        alternate.mkdir(mode=0o700)
        shutil.copytree(self.root / "sets/original/minute_conversion", alternate / "minute_conversion")
        self.build_sources("alternate", "source_facts_v2")
        # Both numeric kernels remain 0..4 -> 0..4; their semantic labels differ.
        self.assertTrue(self.command("STAGE 2 1 alternate").startswith("ERR "))
        self.assertFalse(self.ask("source-fact json-depth-limit")["verified"])
        ticket = self.stage(2, 0, "alternate")
        self.assertTrue(self.command(f"ACTIVATE 2 changed_sources {ticket}").startswith("OK "))
        self.assertEqual(self.ask("source-fact json-depth-limit")["answer"],
                         "include/cnet_json_internal.h:CNETD_JSON_DEPTH_MAX=33")
        self.assertEqual(self.ask("convert 3 minutes to seconds")["answer"], "180")
        self.assertTrue(self.command("ROLLBACK 3 old_again").startswith("OK "))
        self.assertFalse(self.ask("source-fact json-depth-limit")["verified"])
        source.write_bytes(original)
        self.assertTrue(self.ask("source-fact json-depth-limit")["verified"])

    def test_mutation_after_used_hop_refuses_final_answer(self):
        sources = self.source_fixture()
        self.source_downstream()
        ticket = self.stage(1, 1, "original")
        self.assertTrue(self.command(f"ACTIVATE 1 before_mutation {ticket}").startswith("OK "))
        source = sources / "include/cnet_json_internal.h"
        original = source.read_bytes()
        shim = self.root / "mutation.so"
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared", "-o", str(shim),
                        str(REPO / "tests/source_freshness_mutation_shim.c"), "-ldl"], check=True, timeout=30)
        self.stop()
        self.env.update(LD_PRELOAD=str(shim), CNET_TEST_MUTATE_SOURCE=str(source))
        self.start()
        answer = self.ask("capsule cnet_source_fact source_index_plus_one 4")
        self.assertEqual(source.read_bytes(), original + b"\n", "real successful used-hop guard must precede mutation")
        self.assertFalse(answer["verified"], "final receipt must recheck an earlier evidence-bearing hop")
        self.assertFalse(answer["teacher"])

if __name__ == "__main__":
    unittest.main()
