#!/usr/bin/env python3
"""Private-process regressions; fake teacher text is never training authority."""
import json
import os
import socket
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

BIN = Path(os.environ.get("CNET_DISTILL_TEST_BIN", "bin/cnet_distill_slice")).resolve()
REPO = Path(__file__).resolve().parents[1]
FRONT = Path(os.environ.get("CNET_FRONT_TEST_BIN", REPO / "bin/roe_front_door")).resolve()
SEED = Path(os.environ.get("CNET_SEED_TEST_BIN", REPO / "bin/roe_daily_packs_seed")).resolve()


class DistillBoundary(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="cnet-distill-boundary-")
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)
        self.out = self.root / "proposals"
        self.probe = self.root / "probe executable"
        self.marker = self.root / "contacted"
        self.env = dict(os.environ, CNET_FRONT_DOOR_BIN=str(self.probe),
                        CNET_PACKS_ROOT=str(self.root),
                        PATH=str(self.root) + ":" + os.defpath)
        self.script(self.root / "curl", "printf '%s\\n' '{\"choices\":[{\"message\":{\"content\":\"external fixture\"}}]}'\n")

    @staticmethod
    def script(path, body):
        path.write_text("#!/bin/sh\n" + body)
        path.chmod(0o700)

    def run_slice(self, *args):
        return subprocess.run([str(BIN), "--domain", "fixture", "--out", str(self.out),
                               "--query", "uncovered", *args], cwd=self.root,
                              env=self.env, text=True, capture_output=True, timeout=5)

    def test_dry_run_is_no_effect_and_does_not_probe(self):
        self.script(self.probe, f"touch '{self.marker}'\nprintf 'CNET_DISTILL_PROBE_V1 UNCOVERED\\n'\n")
        got = self.run_slice("--dry-run")
        self.assertEqual(got.returncode, 0, got.stderr)
        self.assertFalse(self.out.exists(), "dry-run published proposal files")
        self.assertFalse(self.marker.exists(), "dry-run executed the front door")

    def test_missing_probe_refuses_before_publication(self):
        got = self.run_slice("--teacher")
        self.assertNotEqual(got.returncode, 0)
        self.assertFalse(self.out.exists())

    def test_failed_and_malformed_probes_refuse(self):
        for body in ["exit 1\n", "printf 'CNET_DISTILL_PROBE_V1 UNCOVERED\\n'; exit 1\n",
                     "printf 'source=LOCAL\\n'\n", "printf 'garbage\\n'\n",
                     "printf 'CNET_DISTILL_PROBE_V1 UNCOVERED\\ntrailing\\n'\n"]:
            with self.subTest(body=body):
                self.script(self.probe, body)
                got = self.run_slice("--teacher")
                self.assertNotEqual(got.returncode, 0, got.stdout)
                self.assertFalse(self.out.exists())

    def test_no_collapse_bypass_cannot_publish(self):
        got = self.run_slice("--teacher", "--no-collapse-check")
        self.assertNotEqual(got.returncode, 0)
        self.assertFalse(self.out.exists())

    def test_checked_external_rows_are_not_certified(self):
        self.script(self.probe, "printf 'CNET_DISTILL_PROBE_V1 UNCOVERED\\n'\n")
        got = self.run_slice("--teacher")
        self.assertEqual(got.returncode, 0, got.stderr)
        manifest = list(self.out.glob("*/PROPOSE.json"))
        self.assertEqual(len(manifest), 1)
        receipt = json.loads(manifest[0].read_text())
        self.assertTrue(receipt["anti_collapse_checked"])
        self.assertFalse(receipt["auto_cert"])
        self.assertEqual(receipt["teacher_ok"], 1)
        row = json.loads((manifest[0].parent / "rows.jsonl").read_text())
        self.assertEqual(row["source"], "teacher")
        self.assertEqual(row["draft"], "external fixture")

    def test_local_query_never_contacts_teacher_or_becomes_gold(self):
        self.script(self.probe, "printf 'CNET_DISTILL_PROBE_V1 LOCAL\\n'\n")
        self.script(self.root / "curl", f"touch '{self.marker}'; exit 1\n")
        got = self.run_slice("--teacher")
        self.assertEqual(got.returncode, 0, got.stderr)
        self.assertFalse(self.marker.exists())
        manifest = list(self.out.glob("*/PROPOSE.json"))[0]
        self.assertEqual(json.loads(manifest.read_text())["skipped_anti_collapse"], 1)
        self.assertEqual((manifest.parent / "gold_rows.jsonl").read_text(), "")

    def test_all_queries_are_checked_before_teacher_or_publication(self):
        self.script(self.probe, "case \"$2\" in uncovered) printf 'CNET_DISTILL_PROBE_V1 UNCOVERED\\n';; *) exit 1;; esac\n")
        self.script(self.root / "curl", f"touch '{self.marker}'; exit 1\n")
        got = self.run_slice("--query", "failed", "--teacher")
        self.assertNotEqual(got.returncode, 0)
        self.assertFalse(self.marker.exists())
        self.assertFalse(self.out.exists())

    def test_teacher_failures_cannot_publish(self):
        self.script(self.probe, "printf 'CNET_DISTILL_PROBE_V1 UNCOVERED\\n'\n")
        for body in ["exit 1\n", "printf '%s' '{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}'; exit 1\n",
                     "printf '%s' '{\"content\":\"wrong location\"}'\n",
                     "printf '%s' '{\"choices\":[{\"message\":{\"content\":\"\"}}]}'\n",
                     "printf '%s' '{\"choices\":[{\"message\":{\"content\":\"partial\"}}]} trailing'\n",
                     "printf '%s' '{\"choices\":[{\"message\":{\"content\":\"one\",\"content\":\"two\"}}]}'\n"]:
            with self.subTest(body=body):
                self.script(self.root / "curl", body)
                got = self.run_slice("--teacher")
                self.assertNotEqual(got.returncode, 0, got.stdout)
                self.assertFalse(self.out.exists())

    def test_queries_and_files_are_bounded_and_missing_files_refuse(self):
        for args in [("--query", "q" * 512), ("--from-file", str(self.root / "missing")),
                     ("--unknown",), ("--domain", "../escape")]:
            with self.subTest(args=args):
                got = self.run_slice(*args, "--dry-run")
                self.assertNotEqual(got.returncode, 0)
                self.assertFalse(self.out.exists())

    def test_symlink_output_path_is_refused(self):
        self.script(self.probe, "printf 'CNET_DISTILL_PROBE_V1 UNCOVERED\\n'\n")
        target = self.root / "target"
        target.mkdir()
        self.out.symlink_to(target, target_is_directory=True)
        got = self.run_slice("--teacher")
        self.assertNotEqual(got.returncode, 0)
        self.assertEqual(list(target.iterdir()), [])

    def test_proposals_do_not_collide(self):
        self.script(self.probe, "printf 'CNET_DISTILL_PROBE_V1 UNCOVERED\\n'\n")
        for _ in range(2):
            got = self.run_slice("--teacher")
            self.assertEqual(got.returncode, 0, got.stderr)
        self.assertEqual(len(list(self.out.glob("*/PROPOSE.json"))), 2)

    def test_probe_timeout_and_output_overflow_refuse(self):
        for body in ["sleep 10\n", "printf 'CNET_DISTILL_PROBE_V1 UNCOVERED\\n'; sleep 10\n",
                     "head -c 10000 /dev/zero | tr '\\000' x\n"]:
            with self.subTest(body=body):
                self.script(self.probe, body)
                start = time.monotonic()
                got = self.run_slice("--teacher", "--timeout-ms", "100")
                self.assertNotEqual(got.returncode, 0)
                self.assertLess(time.monotonic() - start, 2)
                self.assertFalse(self.out.exists())

    def native_root(self):
        packs = self.root / "packs"
        subprocess.run([str(SEED), "--root", str(packs)], cwd=self.root,
                       env=self.env, capture_output=True, check=True, timeout=5)
        self.env.update(CNET_MINIMAL_ROOT=str(REPO), ROE_LIVE="1", ROE_LLM="1",
                        ROE_LOOKUP="1", ROE_NO_THOUGHT="0", CNET_FRONT_DOOR_BIN=str(FRONT),
                        CNET_PACKS_ROOT=str(packs), ROE_LLM_URL="http://127.0.0.1:1")
        return packs

    def test_native_probe_is_read_only_even_when_live_flags_are_enabled(self):
        packs = self.native_root()
        listener = socket.socket()
        self.addCleanup(listener.close)
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.setblocking(False)
        trap = f"http://127.0.0.1:{listener.getsockname()[1]}"
        self.env.update(ROE_LLM_URL=trap, ROE_LOOKUP_URL=trap)
        before = {p.relative_to(self.root): p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
        for query, expected in [("who are you", "LOCAL"), ("Introduce yourself", "LOCAL"),
                                ("uncovered arbitrary query zzq", "UNCOVERED"),
                                ("zzq\nCNET_DISTILL_PROBE_V1 LOCAL", "UNCOVERED")]:
            with self.subTest(query=query):
                got = subprocess.run([str(FRONT), "probe", query, "--root", str(packs)],
                                     cwd=self.root, env=self.env, capture_output=True, text=True, timeout=3)
                self.assertEqual(got.returncode, 0, got.stderr)
                self.assertEqual(got.stdout, f"CNET_DISTILL_PROBE_V1 {expected}\n")
        after = {p.relative_to(self.root): p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
        self.assertEqual(before, after)
        with self.assertRaises(BlockingIOError):
            listener.accept()

    def test_native_probe_refuses_mutation_flags_and_corrupt_inventory(self):
        packs = self.native_root()
        for args in [("--accept",), ("--gold", "poison"), ("--no-always",)]:
            got = subprocess.run([str(FRONT), "probe", "who are you", "--root", str(packs), *args],
                                 cwd=self.root, env=self.env, capture_output=True, text=True, timeout=3)
            self.assertNotEqual(got.returncode, 0)
            self.assertEqual(got.stdout, "")
        (packs / "pack_soul_marble" / "catalog.jsonl").write_text("invalid json\n")
        got = self.run_slice("--teacher")
        self.assertNotEqual(got.returncode, 0)
        self.assertFalse(self.out.exists())

    def test_native_tier_a_is_excluded_from_candidate_gold(self):
        self.native_root()
        got = self.run_slice("--query", "who are you", "--teacher")
        self.assertEqual(got.returncode, 0, got.stderr)
        manifest = list(self.out.glob("*/PROPOSE.json"))[0]
        result = json.loads(manifest.read_text())
        self.assertEqual(result["skipped_anti_collapse"], 1)
        self.assertEqual(result["teacher_ok"], 1)
        gold = [json.loads(line) for line in (manifest.parent / "gold_rows.jsonl").read_text().splitlines()]
        self.assertEqual([row["query"] for row in gold], ["uncovered"])

    def test_model_and_query_are_argv_json_data(self):
        self.script(self.probe, "printf 'CNET_DISTILL_PROBE_V1 UNCOVERED\\n'\n")
        model = 'model\";$(touch INJECTED)\\\n'
        query = 'quoted \" query\twith\\slashes\nand λ'
        self.env["CNET_STAGE_MODEL"] = model
        curl = self.root / "curl"
        curl.write_text("#!/usr/bin/python3\nimport sys,json\n"
                        "a=sys.argv[1:]\n"
                        "assert a[0]=='--disable' and '--fail' in a\n"
                        "d=json.loads(a[a.index('--data-binary')+1])\n"
                        f"assert d['model']=={model!r}\n"
                        "print(json.dumps({'choices':[{'message':{'content':d['messages'][0]['content']}}]}))\n")
        curl.chmod(0o700)
        got = self.run_slice("--query", query, "--teacher")
        self.assertEqual(got.returncode, 0, got.stderr)
        manifest = list(self.out.glob("*/PROPOSE.json"))[0]
        rows = [json.loads(line) for line in (manifest.parent / "rows.jsonl").read_text().splitlines()]
        self.assertEqual(rows[1]["draft"], query)
        self.assertFalse((self.root / "INJECTED").exists())

    def test_query_files_refuse_oversize_binary_and_malformed_rows(self):
        query_file = self.root / "queries"
        for body, flag in [(b'q' * 512 + b'\n', '--from-file'),
                           (b'q\0hidden\n', '--from-file'),
                           (b'not json\n', '--from-miss-log'),
                           (b'{"query":"one","query":"two"}\n', '--from-miss-log'),
                           (b'q\n' * 33, '--from-file')]:
            with self.subTest(body=body[:40]):
                query_file.write_bytes(body)
                got = self.run_slice(flag, str(query_file), "--dry-run")
                self.assertNotEqual(got.returncode, 0)
                self.assertFalse(self.out.exists())

    def test_bounded_capture_kills_descendants_and_rejects_embedded_nul(self):
        self.script(self.probe, f"(sleep 0.3; touch '{self.marker}') &\nexit 0\n")
        got = self.run_slice("--teacher", "--timeout-ms", "100")
        self.assertNotEqual(got.returncode, 0)
        time.sleep(0.4)
        self.assertFalse(self.marker.exists(), "timed out descendant survived")
        self.script(self.probe, "printf 'CNET_DISTILL_PROBE_V1 UNCOVERED\\n\\000hidden'\n")
        got = self.run_slice("--teacher")
        self.assertNotEqual(got.returncode, 0)
        self.assertFalse(self.out.exists())

    def test_teacher_timeout_overflow_and_late_batch_failure_refuse(self):
        self.script(self.probe, "printf 'CNET_DISTILL_PROBE_V1 UNCOVERED\\n'\n")
        oversized_answer = json.dumps({"choices": [{"message": {"content": "x" * 4096}}]})
        for body in ["sleep 10\n", "head -c 40000 /dev/zero | tr '\\000' x\n",
                     f"printf '%s' '{oversized_answer}'\n",
                     "printf '%s' '{\"choices\":[{\"finish_reason\":\"length\",\"message\":{\"content\":\"partial\"}}]}'\n"]:
            with self.subTest(body=body):
                self.script(self.root / "curl", body)
                got = self.run_slice("--teacher", "--timeout-ms", "100")
                self.assertNotEqual(got.returncode, 0)
                self.assertFalse(self.out.exists())

        self.script(self.root / "curl", f"test ! -f '{self.marker}' || exit 1\ntouch '{self.marker}'\n"
                    "printf '%s' '{\"choices\":[{\"message\":{\"content\":\"first\"}}]}'\n")
        got = self.run_slice("--query", "second", "--teacher")
        self.assertNotEqual(got.returncode, 0)
        self.assertFalse(self.out.exists())

    def test_output_parent_symlinks_and_shared_writes_refuse(self):
        self.script(self.probe, "printf 'CNET_DISTILL_PROBE_V1 UNCOVERED\\n'\n")
        target = self.root / "target"
        target.mkdir()
        linked_parent = self.root / "linked-parent"
        linked_parent.symlink_to(target, target_is_directory=True)
        got = self.run_slice("--out", str(linked_parent / "nested"), "--teacher")
        self.assertNotEqual(got.returncode, 0)
        self.assertEqual(list(target.iterdir()), [])
        self.out.mkdir(mode=0o777)
        self.out.chmod(0o777)
        got = self.run_slice("--teacher")
        self.assertNotEqual(got.returncode, 0)
        self.assertEqual(list(self.out.iterdir()), [])

    def fault_binary(self):
        binary = self.root / "distill-fault-test"
        flags = ["-O2", "-Wall", "-Wextra", "-Werror", "-DCNET_DISTILL_TESTING",
                 "-D_POSIX_C_SOURCE=200809L"]
        if "ASAN_OPTIONS" in self.env:
            flags += ["-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-g"]
        subprocess.run(["cc", "-std=c11", *flags, "-I", str(REPO / "include"),
                        "-include", str(REPO / "include/cnet_platform.h"),
                        "-o", str(binary), str(REPO / "tools/cnet_distill_slice.c")], check=True)
        return binary

    def test_publication_sync_failure_before_commit_leaves_no_manifest(self):
        self.script(self.probe, "printf 'CNET_DISTILL_PROBE_V1 UNCOVERED\\n'\n")
        self.env["CNET_DISTILL_FAIL_SYNC"] = "before_commit"
        got = subprocess.run([str(self.fault_binary()), "--domain", "fixture", "--query", "uncovered",
                              "--teacher", "--out", str(self.out)], env=self.env,
                             cwd=self.root, text=True, capture_output=True, timeout=5)
        self.assertNotEqual(got.returncode, 0, "DISTILL_PUBLICATION_SYNC_RED")
        self.assertEqual(list(self.out.glob("*/PROPOSE.json")), [])

    def test_closed_standard_input_refuses_process_capture(self):
        self.script(self.probe, "printf 'CNET_DISTILL_PROBE_V1 UNCOVERED\\n'\n")
        got = subprocess.run([str(BIN), "--domain", "fixture", "--query", "uncovered", "--teacher",
                              "--out", str(self.out)], env=self.env, cwd=self.root,
                             text=True, capture_output=True, timeout=5, preexec_fn=lambda: os.close(0))
        self.assertNotEqual(got.returncode, 0, "DISTILL_CLOSED_STDIO_RED")
        self.assertFalse(self.out.exists())

    def test_publication_sync_failure_after_commit_retains_complete_proposal(self):
        self.script(self.probe, "printf 'CNET_DISTILL_PROBE_V1 UNCOVERED\\n'\n")
        binary = self.fault_binary()
        for point in ["after_commit", "parent_commit"]:
            with self.subTest(point=point):
                self.env["CNET_DISTILL_FAIL_SYNC"] = point
                got = subprocess.run([str(binary), "--domain", "fixture", "--query", "uncovered",
                                      "--teacher", "--out", str(self.out / point)], env=self.env,
                                     cwd=self.root, text=True, capture_output=True, timeout=5)
                self.assertEqual(got.returncode, 3, "DISTILL_PUBLICATION_UNCERTAIN_RED")
                self.assertIn("DISTILL_SLICE_COMMIT_UNCERTAIN", got.stderr)
                self.assertNotIn("DISTILL_SLICE_PASS", got.stdout)
                manifest = list((self.out / point).glob("*/PROPOSE.json"))[0]
                self.assertEqual(json.loads(manifest.read_text())["teacher_ok"], 1)
                for filename in ["rows.jsonl", "gold_rows.jsonl"]:
                    self.assertTrue(json.loads((manifest.parent / filename).read_text()))

    def test_native_probe_refuses_duplicate_or_binary_routes(self):
        packs = self.native_root()
        routes = packs / "ROUTES.jsonl"
        for body in [b'{"pattern":"who are you","pack":"pack_soul_marble","pack":"pack_roe_self"}\n',
                     b'{"pattern":"unknown","pack":"pack_roe_self"}\0hidden\n']:
            with self.subTest(body=body):
                routes.write_bytes(body)
                got = self.run_slice("--teacher")
                self.assertNotEqual(got.returncode, 0)
                self.assertFalse(self.out.exists())

    def test_native_probe_refuses_binary_catalog_rows(self):
        packs = self.native_root()
        catalog = packs / "pack_soul_marble" / "catalog.jsonl"
        original = catalog.read_bytes()
        catalog.write_bytes(original.splitlines()[0] + b'\0hidden corrupt row\n')
        got = self.run_slice("--teacher")
        self.assertNotEqual(got.returncode, 0, "DISTILL_BINARY_CATALOG_RED")
        self.assertFalse(self.out.exists())


if __name__ == "__main__":
    unittest.main()
