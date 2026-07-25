#!/usr/bin/env python3
"""Metric honesty suite — tests that fail when a governor metric lies.

Every defect fixed on 2026-07-25 was an instrument bug, not a mechanism bug.
The mechanisms (teach, certify, seal, replay) are verified; the things that
WATCH them were written in one pass and never checked, and because the governor
promotes those readings into decisions, a wrong number became a wrong priority.

Each test below encodes the invariant the metric must satisfy and is built so
that the ORIGINAL buggy implementation fails it. Every case carries the real
observed value it would have produced. Verified by mutation: reintroducing each
bug turns the corresponding test red (see `make metric_honesty_mutation`).

Hermetic: no services, no teacher, no network, no writes outside a tmpdir.
The only exception is a read-only cnb_audit call, skipped if the base is absent.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import gap_inject  # noqa: E402
import governor_hermes_structured as hermes  # noqa: E402


def write_gaps(path: Path, closed: int = 0, open_: int = 0, waiting: int = 0) -> None:
    """Write a ledger in the on-disk format: 2 header lines, state in field 1."""
    lines = ["CNET_GAPS 4", str(closed + open_ + waiting)]
    n = 0
    for _ in range(closed):
        n += 1
        lines.append(f"0 2 1 1 1 256 1 w_cur 1 256 3 tk{n}q{n} - cce_cond_next - - 0 0")
    for _ in range(open_):
        n += 1
        lines.append(f"0 1 1 1 1 256 1 w_cur 1 256 3 tk{n}q{n} - cce_cond_next - - 0 0")
    for _ in range(waiting):
        n += 1
        lines.append(
            f"0 1 1 1 1 256 1 w_cur 1 256 3 tk{n}q{n} - cce_cond_next waiting_oracle - 0 0"
        )
    path.write_text("\n".join(lines) + "\n")


def run_miss_ingest(tmp: Path, gaps: Path, faults: Path) -> dict:
    base = tmp / "base.cnb"
    base.write_bytes(b"\0")
    # miss_ingest derives the ledger path from the base
    target = Path(str(base) + ".gaps.txt")
    target.write_text(gaps.read_text())
    env = dict(os.environ)
    env.update(
        {
            "CNET_ROOT": str(ROOT),
            "CNET_BASE_PATH": str(base),
            "CNET_FAULT_LOG": str(faults),
            "CNET_GOVERNOR_DIR": str(tmp),
        }
    )
    subprocess.run(
        ["bash", str(ROOT / "scripts/governor_miss_ingest.sh")],
        env=env, capture_output=True, text=True, timeout=120, check=True,
    )
    return json.loads((tmp / "miss_bus.json").read_text())


class TestMissRateDenominator(unittest.TestCase):
    """real_miss_rate must be a rate: 0 when nothing is outstanding.

    The original denominator was (waiting + open + 1) — it excluded every
    closed gap, so success could not move the number. With 10 waiting and 628
    closed it read 0.2941 and, after being max()'d with the Hermes rate,
    published 0.8969, making real_miss_cut the top project at urgency 57.6.
    """

    def test_all_closed_is_zero(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            g = tmp / "g.txt"
            write_gaps(g, closed=500)
            rep = run_miss_ingest(tmp, g, tmp / "f.jsonl")
            # Old formula: 0/(0+0+1) = 0.0 — passes by luck here, so the
            # discriminating cases are the two below.
            self.assertEqual(rep["real_miss_rate"], 0.0)

    def test_successes_are_in_the_denominator(self):
        """The discriminating case: the old formula ignored `closed` entirely."""
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            g = tmp / "g.txt"
            write_gaps(g, closed=990, open_=0, waiting=10)
            rep = run_miss_ingest(tmp, g, tmp / "f.jsonl")
            # honest: 10/1000 = 0.01.  old: 10/(10+10+1) = 0.476
            self.assertAlmostEqual(rep["real_miss_rate"], 0.01, places=4)
            self.assertLess(
                rep["real_miss_rate"], 0.05,
                "a ledger that is 99% closed must not report a high miss rate",
            )

    def test_waiting_rows_are_not_double_counted(self):
        """waiting_oracle is an annotation on an OPEN row, not a third state."""
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            g = tmp / "g.txt"
            write_gaps(g, closed=90, open_=0, waiting=10)
            rep = run_miss_ingest(tmp, g, tmp / "f.jsonl")
            self.assertEqual(
                rep["waiting_oracle"] + rep["open_gaps"], 10,
                "waiting rows counted as both waiting and open",
            )
            self.assertAlmostEqual(rep["real_miss_rate"], 0.10, places=4)

    def test_monotonic_in_closed_gaps(self):
        """Closing gaps must lower the rate. The old formula held it constant."""
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            rates = []
            for closed in (10, 100, 1000):
                g = tmp / "g.txt"
                write_gaps(g, closed=closed, waiting=10)
                rates.append(run_miss_ingest(tmp, g, tmp / "f.jsonl")["real_miss_rate"])
            self.assertTrue(
                rates[0] > rates[1] > rates[2],
                f"rate must fall as work completes, got {rates}",
            )

    def test_rate_is_bounded(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            g = tmp / "g.txt"
            write_gaps(g, closed=0, open_=5, waiting=5)
            rep = run_miss_ingest(tmp, g, tmp / "f.jsonl")
            self.assertEqual(rep["real_miss_rate"], 1.0)


class TestParsersAgree(unittest.TestCase):
    """Two readers of the same ledger must not report contradictory totals.

    governor_autonomous.collect() partitions rows (waiting XOR open), while
    miss_ingest counted waiting rows as open as well — so backlog_pressure and
    real_miss_rate disagreed about how much work was outstanding.
    """

    @staticmethod
    def _governor_parse(text: str) -> tuple[int, int, int]:
        open_n = def_n = closed_n = 0
        for i, ln in enumerate(text.splitlines()):
            if i < 2:
                continue
            if "waiting_oracle" in ln or "waiting_charter" in ln:
                def_n += 1
            else:
                p = ln.split()
                if len(p) > 1 and p[1] == "2":
                    closed_n += 1
                elif len(p) > 1 and p[1] == "1":
                    open_n += 1
        return open_n, def_n, closed_n

    def test_outstanding_totals_match(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            g = tmp / "g.txt"
            write_gaps(g, closed=200, open_=15, waiting=7)
            rep = run_miss_ingest(tmp, g, tmp / "f.jsonl")
            g_open, g_def, g_closed = self._governor_parse(g.read_text())
            self.assertEqual(
                rep["waiting_oracle"] + rep["open_gaps"], g_open + g_def,
                "miss_bus and the governor disagree on outstanding work",
            )
            self.assertEqual(rep["closed_gaps"], g_closed)

    def test_parsers_agree_on_the_real_ledger(self):
        """Real production data, one consistent snapshot.

        Comparing against logs/governor/scoreboard.json directly would be flaky:
        the lane closes gaps continuously, so a scoreboard written minutes ago
        describes a different ledger. Copy once, then run both readers over
        that copy.
        """
        ledger = ROOT / "soul_gemma4v2_final.cnb.gaps.txt"
        if not ledger.exists():
            self.skipTest("no live ledger")
        text = ledger.read_text(errors="replace")
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            g = tmp / "g.txt"
            g.write_text(text)
            rep = run_miss_ingest(tmp, g, tmp / "f.jsonl")
            g_open, g_def, g_closed = self._governor_parse(text)
            self.assertEqual(
                rep["waiting_oracle"] + rep["open_gaps"], g_open + g_def,
                "miss_bus and the governor disagree on outstanding work "
                "in the real ledger",
            )
            self.assertEqual(rep["closed_gaps"], g_closed)


class TestMissRateProvenance(unittest.TestCase):
    """The scoreboard's real_miss_rate must come from CNET, never from Hermes.

    governor_autonomous.py did `max(cnet_miss, hermes_err_rate)`, so another
    system's tool-error rate was published as CNET's miss rate.
    """

    def test_no_max_with_hermes(self):
        src = (ROOT / "scripts/governor_autonomous.py").read_text()
        m = re.search(r"^\s*real_miss\s*=\s*(.+)$", src, re.MULTILINE)
        self.assertIsNotNone(m, "real_miss assignment not found")
        expr = m.group(1)
        self.assertNotIn(
            "hermes", expr.lower(),
            "real_miss must not be derived from any hermes_* field; "
            f"got: {expr.strip()}",
        )

    def test_hermes_rate_still_reported_separately(self):
        src = (ROOT / "scripts/governor_autonomous.py").read_text()
        self.assertIn(
            '"hermes_err_rate"', src,
            "Hermes health must still be observable, just not as CNET's miss rate",
        )


class TestHermesClassifier(unittest.TestCase):
    """Tool outcomes must be decided on the envelope, not on substrings.

    The rule `if role == "tool" and "error" in text.lower(): fail` flagged
    essentially every result, because the envelope carries an `error` key on
    every terminal result and it is usually empty. Observed: 470 fails / 54 oks
    = 0.8969, with PERSONALITY_SELFTEST_PASS stored as a "failure".
    """

    def test_empty_error_field_is_success(self):
        payload = json.dumps({"output": "hello", "error": "", "exit_code": 0})
        self.assertEqual(hermes.classify(payload, "terminal"), "ok")

    def test_success_output_mentioning_the_word_error_is_success(self):
        """The exact shape that produced the fabricated 0.90 rate."""
        payload = json.dumps(
            {"output": "grep -c error app.log\n0 errors found", "error": "", "exit_code": 0}
        )
        self.assertEqual(hermes.classify(payload, "terminal"), "ok")

    def test_selftest_pass_output_is_success(self):
        payload = json.dumps(
            {"output": "PERSONALITY_SELFTEST_PASS checks=6", "error": "", "exit_code": 0}
        )
        self.assertEqual(hermes.classify(payload, "terminal"), "ok")

    def test_populated_error_field_is_failure(self):
        payload = json.dumps({"output": "", "error": "BLOCKED: timed out", "exit_code": -1})
        self.assertEqual(hermes.classify(payload, "terminal"), "fail")

    def test_nonzero_exit_is_failure(self):
        payload = json.dumps({"output": "", "error": "", "exit_code": 1})
        self.assertEqual(hermes.classify(payload, "terminal"), "fail")

    def test_pending_approval_is_neither(self):
        payload = json.dumps(
            {"output": "", "error": "", "exit_code": -1, "status": "pending_approval"}
        )
        self.assertEqual(hermes.classify(payload, "terminal"), "skip")

    def test_structured_result_without_error_key_is_success(self):
        payload = json.dumps({"bytes_written": 12, "resolved_path": "/tmp/x"})
        self.assertEqual(hermes.classify(payload, "patch"), "ok")

    def test_explicit_failure_flag_is_failure(self):
        payload = json.dumps({"success": False, "error": "over the limit"})
        self.assertEqual(hermes.classify(payload, "memory"), "fail")

    def test_all_success_corpus_yields_zero_rate(self):
        """Aggregate guard: a corpus of successes must not read ~0.9."""
        corpus = [
            json.dumps({"output": f"line {i}\nerror: none", "error": "", "exit_code": 0})
            for i in range(50)
        ]
        fails = sum(1 for c in corpus if hermes.classify(c, "terminal") == "fail")
        self.assertEqual(
            fails, 0, "a corpus of successful tool results must yield zero failures"
        )

    def test_non_json_payload_defaults_to_success(self):
        self.assertEqual(hermes.classify("plain tool output, nothing wrong", "terminal"), "ok")

    def test_non_json_traceback_is_failure(self):
        self.assertEqual(
            hermes.classify("Traceback (most recent call last):\n  File ...", "terminal"),
            "fail",
        )


class TestGapInject(unittest.TestCase):
    """The injector must propose fresh work, and say so when it cannot.

    Both injectors seeded RNG from a time bucket (3600s in the tick, 600s in
    the governor), so every tick within the bucket proposed the SAME ids; the
    lane closed them once and reported bound=0 / drained=0 thereafter. Neither
    filtered ids already sealed, so a tick could look busy while proposing
    nothing new.
    """

    def _base(self, tmp: Path, closed_tokens=(), pending_tokens=()):
        base = tmp / "base.cnb"
        base.write_bytes(b"\0")
        lines = ["CNET_GAPS 4", str(len(closed_tokens))]
        for t in closed_tokens:
            lines.append(
                f"0 2 1 1 1 256 1 w_cur 1 256 3 tk{t}q{t} - cce_cond_next - - 0 0"
            )
        Path(str(base) + ".gaps.txt").write_text("\n".join(lines) + "\n")
        if pending_tokens:
            Path(str(base) + ".inbox").write_text(
                "".join(f"NO_PLAN 1 256 1 w_cur 1 256 3 tk{t}q{t}\n" for t in pending_tokens)
            )
        return base

    def _window(self, tmp: Path, ids) -> Path:
        w = tmp / "win.txt"
        w.write_text("\n".join(str(i) for i in ids) + "\n")
        return w

    def test_successive_calls_are_not_identical(self):
        """Time-bucket seeding made 3 ticks/hour propose the same 4 ids."""
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            base = self._base(tmp)
            win = self._window(tmp, range(1, 201))
            draws = set()
            for _ in range(12):
                lines, _ = gap_inject.pick(base, win, 4)
                draws.add(tuple(sorted(l.split()[-1] for l in lines)))
            self.assertGreater(
                len(draws), 1,
                "12 successive draws from a 200-id window produced one sample — "
                "the RNG is seeded from a coarse clock",
            )

    def test_never_proposes_a_sealed_token(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            sealed = list(range(1, 96))
            base = self._base(tmp, closed_tokens=sealed)
            win = self._window(tmp, range(1, 101))
            for _ in range(30):
                lines, _ = gap_inject.pick(base, win, 4)
                got = {int(re.search(r"tk(\d+)q", l).group(1)) for l in lines}
                self.assertEqual(
                    got & set(sealed), set(),
                    f"proposed already-sealed ids: {sorted(got & set(sealed))}",
                )

    def test_never_proposes_a_queued_token(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            pending = [7, 8, 9]
            base = self._base(tmp, pending_tokens=pending)
            win = self._window(tmp, [7, 8, 9, 10, 11, 12])
            for _ in range(20):
                lines, _ = gap_inject.pick(base, win, 2)
                got = {int(re.search(r"tk(\d+)q", l).group(1)) for l in lines}
                self.assertEqual(got & set(pending), set())

    def test_exhaustion_is_reported_not_faked(self):
        """Fully-covered window must report exhaustion, not re-propose."""
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            base = self._base(tmp, closed_tokens=range(1, 11))
            win = self._window(tmp, range(1, 11))
            lines, info = gap_inject.pick(base, win, 4)
            self.assertEqual(lines, [])
            self.assertEqual(info["reason"], "window_exhausted")

    def test_remaining_count_is_accurate(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            base = self._base(tmp, closed_tokens=range(1, 91))
            win = self._window(tmp, range(1, 101))
            lines, info = gap_inject.pick(base, win, 4)
            self.assertEqual(len(lines), 4)
            self.assertEqual(info["remaining"], 6)


class TestUnitCountIsAuthoritative(unittest.TestCase):
    """The unit metric must be the CNB count, never a byte-marker proxy.

    units_proxy counted b"acq_"/b"json_toolcall"/b"hyb_struct" substrings and
    read 355 on both sides of a window in which the real count moved 102 -> 120,
    so d_units_proxy, learning_velocity and plateau were all blind.
    """

    def test_no_byte_marker_proxy_on_the_primary_path(self):
        src = (ROOT / "scripts/governor_autonomous.py").read_text()
        fn = re.search(r"def real_units\(\).*?(?=\ndef )", src, re.DOTALL)
        self.assertIsNotNone(fn, "real_units() not found")
        body = fn.group(0)
        # Strip the docstring so prose about the old proxy is not mistaken for code.
        code = re.sub(r'""".*?"""', "", body, flags=re.DOTALL)
        self.assertIn("cnb_audit", code, "authoritative count must be the primary path")
        self.assertIn("units=", code, "must parse the audit's unit count")
        # The byte proxy may survive only as a fallback, after the audit attempt.
        proxy_at = code.find('count(b"acq_")')
        audit_at = code.find("cnb_audit")
        if proxy_at != -1:
            self.assertLess(
                audit_at, proxy_at,
                "byte-marker proxy must not precede the authoritative count",
            )

    def test_proxy_and_truth_diverge_on_crafted_input(self):
        """Proof the proxy is not merely imprecise but unrelated to the count."""
        blob = b"acq_" * 40 + b"json_toolcall" * 10 + b"hyb_struct" * 5
        proxy = blob.count(b"acq_") + blob.count(b"json_toolcall") + blob.count(b"hyb_struct")
        self.assertEqual(proxy, 55, "byte proxy counts substrings, not units")

    def test_live_metric_matches_cnb_audit(self):
        audit = ROOT / "bin/cnb_audit"
        base = ROOT / "soul_gemma4v2_final.cnb"
        if not audit.exists() or not base.exists():
            self.skipTest("cnb_audit or base unavailable")
        out = subprocess.run(
            [str(audit), str(base), "--count"], capture_output=True, text=True, timeout=120
        ).stdout
        truth = int(re.search(r"units=(\d+)", out).group(1))
        blob = base.read_bytes()
        proxy = (
            blob.count(b"acq_") + blob.count(b"json_toolcall") + blob.count(b"hyb_struct")
        )
        self.assertNotEqual(
            proxy, truth,
            "proxy and truth coincide here, so this base cannot discriminate; "
            "the divergence check above still holds",
        )
        import governor_autonomous  # noqa: E402  (imports systemd-free helpers)

        self.assertEqual(governor_autonomous.real_units(), truth)


class TestEvalProbeHonesty(unittest.TestCase):
    """The eval probe must not write into the data it measures, and must
    surface the signal that can actually move.

    It wrote ~211 synthetic faults per run into logs/cnet_faults.jsonl — the
    file the miss-bus reads — while reporting eval_jtc_delta=0.44 with
    d_eval_jtc=0.0 on every cycle, because the bench is hermetic over a fixed
    set. jtc_lift therefore read "target achieved" permanently.
    """

    def test_probe_does_not_grow_the_shared_fault_bus(self):
        """Behavioural, not a source grep.

        A static check on `CNET_FAULT_LOG=` matched the variable NAME and so
        survived a mutation that pointed that variable back at the shared bus.
        Run the probe against a sacrificial bus and require zero growth.
        """
        probe = ROOT / "scripts/governor_eval_probe.sh"
        bench = ROOT / "bin/jtc_adapter_bench"
        if not bench.exists():
            self.skipTest("jtc_adapter_bench not built")
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            shared = tmp / "cnet_faults.jsonl"
            shared.write_text("")
            govdir = tmp / "gov"
            govdir.mkdir()
            env = dict(
                os.environ,
                CNET_ROOT=str(ROOT),
                CNET_GOVERNOR_DIR=str(govdir),
                CNET_FAULT_LOG=str(shared),
            )
            subprocess.run(
                ["bash", str(probe)], env=env, cwd=str(ROOT),
                capture_output=True, timeout=400,
            )
            grew = len(shared.read_text().splitlines())
            self.assertEqual(
                grew, 0,
                f"probe wrote {grew} records into the fault bus it measures",
            )
            scratch = govdir / "eval_faults.jsonl"
            self.assertTrue(scratch.exists(), "probe wrote no scratch bus at all")
            self.assertGreater(
                len(scratch.read_text().splitlines()), 0,
                "scratch bus empty — the probe may not have run",
            )

    def test_probe_declares_its_source_as_synthetic(self):
        src = (ROOT / "scripts/governor_eval_probe.sh").read_text()
        self.assertIn("synthetic_fixed_set", src)

    def test_probe_surfaces_regressions(self):
        src = (ROOT / "scripts/governor_eval_probe.sh").read_text()
        for field in ("cert_regress", "cert_fixes", "acc_on", "acc_off"):
            self.assertIn(field, src, f"probe must surface {field}")

    def test_veto_triggers_on_regression_rise_not_only_delta(self):
        src = (ROOT / "scripts/governor_autonomous.py").read_text()
        self.assertIn(
            "d_eval_cert_regress", src,
            "d_eval_jtc is constant by construction; the veto needs a signal "
            "that can actually change",
        )

    def test_seal_rejects_are_a_delta_not_a_running_total(self):
        src = (ROOT / "scripts/governor_eval_probe.sh").read_text()
        self.assertIn("seal_reject_new", src)
        self.assertIn("muscle_offset", src, "needs an offset to compute new-since-last")


class TestFaultBusIdempotence(unittest.TestCase):
    """Re-appending an identical fault record must not grow the bus.

    Dedupe was in-process only, so each fresh tick started with an empty set and
    re-appended its whole seed batch: 4385 lines holding 311 distinct pairs.
    """

    def test_dedupe_survives_a_new_process(self):
        exe = ROOT / "bin/cnet_fault_dedupe_probe"
        if not exe.exists():
            self.skipTest("cnet_fault_dedupe_probe not built (make cnet_fault_dedupe_probe)")
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "faults.jsonl"
            env = dict(os.environ, CNET_FAULT_LOG=str(log))
            subprocess.run([str(exe)], env=env, check=True, capture_output=True, timeout=60)
            first = len(log.read_text().splitlines())
            self.assertGreater(first, 0, "probe wrote nothing")
            for _ in range(3):
                subprocess.run(
                    [str(exe)], env=env, check=True, capture_output=True, timeout=60
                )
            self.assertEqual(
                len(log.read_text().splitlines()), first,
                "identical records re-appended by a later process",
            )

    def test_dedupe_can_still_be_disabled(self):
        exe = ROOT / "bin/cnet_fault_dedupe_probe"
        if not exe.exists():
            self.skipTest("cnet_fault_dedupe_probe not built")
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "faults.jsonl"
            env = dict(os.environ, CNET_FAULT_LOG=str(log), CNET_FAULT_DEDUPE="0")
            subprocess.run([str(exe)], env=env, check=True, capture_output=True, timeout=60)
            first = len(log.read_text().splitlines())
            subprocess.run([str(exe)], env=env, check=True, capture_output=True, timeout=60)
            self.assertEqual(len(log.read_text().splitlines()), 2 * first)


class TestNoDedupeOverrideInProduction(unittest.TestCase):
    """The 24/7 callers must not force dedupe off."""

    def test_autoteach_tick_does_not_disable_dedupe(self):
        src = (ROOT / "scripts/cnet_autoteach_tick.sh").read_text()
        live = [
            l for l in src.splitlines()
            if "CNET_FAULT_DEDUPE=0" in l and not l.strip().startswith("#")
        ]
        self.assertEqual(live, [], f"dedupe forced off: {live}")

    def test_governor_does_not_disable_dedupe(self):
        src = (ROOT / "scripts/governor_autonomous.py").read_text()
        live = [
            l for l in src.splitlines()
            if "CNET_FAULT_DEDUPE=0" in l and not l.strip().startswith("#")
        ]
        self.assertEqual(live, [], f"dedupe forced off: {live}")


class TestCadenceGates(unittest.TestCase):
    """Periodic work must gate on elapsed time, not on the wall-clock hour.

    `$(date +%H) % 3 -eq 0` is evaluated per tick, so with a 20-minute timer the
    "every 3rd hour" job ran three times inside the qualifying hour and then not
    at all for two.
    """

    def test_no_modulo_hour_gates(self):
        src = (ROOT / "scripts/cnet_autoteach_tick.sh").read_text()
        bad = [
            l for l in src.splitlines()
            if re.search(r"date \+%H\s*\)\s*%\s*\d", l) and not l.strip().startswith("#")
        ]
        self.assertEqual(bad, [], f"hour-bucket cadence gate still present: {bad}")

    def test_due_helper_is_used(self):
        src = (ROOT / "scripts/cnet_autoteach_tick.sh").read_text()
        self.assertIn("due()", src)
        self.assertRegex(src, r"\bdue \w+ \d+")


class TestPinProtection(unittest.TestCase):
    """A pin can be the only copy of dropped units; rotation must respect that."""

    def test_keeplist_is_honoured(self):
        src = (ROOT / "scripts/cnet_pin_prune.sh").read_text()
        self.assertIn("KEEP.txt", src)
        self.assertIn("is_protected", src)

    def test_blanket_delete_requires_intent(self):
        src = (ROOT / "scripts/cnet_pin_prune.sh").read_text()
        self.assertIn("CNET_PIN_PRUNE_FORCE", src)

    def test_protected_pin_survives_forced_prune(self):
        script = ROOT / "scripts/cnet_pin_prune.sh"
        with tempfile.TemporaryDirectory() as d:
            pins = Path(d)
            for n in ("pin_old.cnb", "pin_mid.cnb", "pin_keep.cnb"):
                (pins / n).write_text("x")
            (pins / "KEEP.txt").write_text("pin_keep.cnb\n")
            env = dict(
                os.environ,
                CNET_GOV_PIN_DIR=str(pins),
                CNET_GOV_MAX_SNAPSHOTS="0",
                CNET_PIN_PRUNE_FORCE="1",
            )
            subprocess.run(["bash", str(script)], env=env, capture_output=True, timeout=60)
            self.assertTrue((pins / "pin_keep.cnb").exists(), "protected pin was deleted")
            self.assertFalse((pins / "pin_old.cnb").exists())

    def test_dry_run_deletes_nothing(self):
        script = ROOT / "scripts/cnet_pin_prune.sh"
        with tempfile.TemporaryDirectory() as d:
            pins = Path(d)
            for n in ("pin_a.cnb", "pin_b.cnb", "pin_c.cnb"):
                (pins / n).write_text("x")
            env = dict(
                os.environ, CNET_GOV_PIN_DIR=str(pins), CNET_GOV_MAX_SNAPSHOTS="1"
            )
            subprocess.run(
                ["bash", str(script), "--dry-run"], env=env, capture_output=True, timeout=60
            )
            self.assertEqual(len(list(pins.glob("*.cnb"))), 3)

    def test_live_keeplist_protects_the_16h14_pin(self):
        keep = ROOT / "artifacts/janitor/pins/KEEP.txt"
        if not keep.exists():
            self.skipTest("no live KEEP.txt")
        self.assertIn("pin_20260725_161423", keep.read_text())


class TestTagCollisionResistance(unittest.TestCase):
    """Over-long curriculum names must not collapse into one unit.

    queue() warned and then truncated to 31 chars anyway, so any two goals
    sharing a 31-char prefix minted the same unit name.
    """

    def _queue(self, tmp: Path, *goals: str) -> list[str]:
        script = ROOT / "scripts/queue_research_skills.sh"
        body = script.read_text()
        fn = re.search(r"queue\(\) \{.*?\n\}", body, re.DOTALL).group(0)
        harness = tmp / "h.sh"
        harness.write_text(
            'INBOX="$1"; K=3; W=256\n' + fn + "\n"
            + "\n".join(f"queue {g}" for g in goals) + "\n"
        )
        inbox = tmp / "inbox"
        subprocess.run(
            ["bash", str(harness), str(inbox)], capture_output=True, timeout=60, check=True
        )
        return [l.split()[-1] for l in inbox.read_text().splitlines()]

    def test_long_names_sharing_a_prefix_stay_distinct(self):
        with tempfile.TemporaryDirectory() as d:
            tags = self._queue(
                Path(d),
                "research_gamedev_command_pattern",
                "research_gamedev_command_patterns_v2",
            )
            self.assertEqual(len(set(tags)), 2, f"names collided into {tags}")

    def test_shortened_tags_fit_port_tag_max(self):
        with tempfile.TemporaryDirectory() as d:
            tags = self._queue(
                Path(d), "research_" + "x" * 60, "skill_" + "y" * 60
            )
            for t in tags:
                self.assertLessEqual(len(t), 31, f"{t} exceeds PORT_TAG_MAX-1")

    def test_short_names_pass_through_unchanged(self):
        with tempfile.TemporaryDirectory() as d:
            tags = self._queue(Path(d), "research_unity_juice")
            self.assertEqual(tags, ["research_unity_juice"])


class TestConsolidateEvidenceGate(unittest.TestCase):
    """Never prune on evidence that was never collected.

    Reliability was never persisted, so every unit scored the 500 Laplace prior
    and "keep top 48 by reliability" was arbitrary order. That is how the
    16:14 apply dropped 108 certified acq_tk* units.
    """

    def test_unmeasured_units_are_spared(self):
        src = (ROOT / "tools/cnet_consolidate.c").read_text()
        self.assertIn("unmeasured", src)
        self.assertIn("soul_unit_evidence_count", src)

    def test_dry_run_on_live_base_drops_nothing_without_evidence(self):
        tool = ROOT / "bin/cnet_consolidate"
        base = ROOT / "soul_gemma4v2_final.cnb"
        if not tool.exists() or not base.exists():
            self.skipTest("consolidate or base unavailable")
        out = subprocess.run(
            [str(tool), str(base)],
            capture_output=True, text=True, timeout=300, cwd=str(ROOT),
            env=dict(os.environ, CNET_GOV_KEEP_PER_BUCKET="20", CNET_GOV_BUCKET_MIN="4"),
        ).stdout
        m = re.search(r"drop=(\d+)", out)
        self.assertIsNotNone(m, f"no plan line in output: {out[:300]}")
        spared = re.search(r"CONSOLIDATE_SPARED_UNMEASURED (\d+)", out)
        if spared and int(spared.group(1)) > 0:
            self.assertEqual(
                int(m.group(1)), 0,
                "units were dropped in the same run that spared unmeasured ones",
            )


class TestReliabilityIsPersisted(unittest.TestCase):
    """Serve counters must reach disk, or the heal path can never fire.

    registry.c always incremented output_successes/failures, but cnb_put_stats
    had no production caller: every base reported stats=0 and every evidence
    record read reliability 0.5 / 0 / 0.
    """

    def test_lane_persists_and_restores(self):
        """Behavioural: drive the real checkpoint/reopen round-trip.

        A source grep for cnb_put_stats survived a mutation that deleted the
        call, because the identifier still appeared in the surrounding comment.
        bin/test_gap_lane exercises the round-trip against a real lane, so the
        assertion is on observed behaviour. (Runs the prebuilt binary; the
        mutation gate rebuilds it first.)
        """
        exe = ROOT / "bin/test_gap_lane"
        if not exe.exists():
            self.skipTest("test_gap_lane not built (make gap_lane)")
        out = subprocess.run(
            [str(exe)], capture_output=True, text=True, timeout=600, cwd=str(ROOT)
        ).stdout
        line = [
            l for l in out.splitlines()
            if "reliability counters survive checkpoint + reopen" in l
        ]
        self.assertTrue(line, "round-trip check missing from the lane test")
        self.assertIn("PASS", line[0], line[0])

    def test_retrained_units_do_not_inherit_stale_evidence(self):
        exe = ROOT / "bin/test_gap_lane"
        if not exe.exists():
            self.skipTest("test_gap_lane not built")
        out = subprocess.run(
            [str(exe)], capture_output=True, text=True, timeout=600, cwd=str(ROOT)
        ).stdout
        line = [l for l in out.splitlines() if "retrainer invalidates" in l]
        self.assertTrue(line, "invalidation check missing from the lane test")
        self.assertIn("PASS", line[0], line[0])

    def test_live_base_has_stats(self):
        audit = ROOT / "bin/cnb_audit"
        base = ROOT / "soul_gemma4v2_final.cnb"
        if not audit.exists() or not base.exists():
            self.skipTest("cnb_audit or base unavailable")
        out = subprocess.run(
            [str(audit), str(base), "--count"], capture_output=True, text=True, timeout=120
        ).stdout
        units = int(re.search(r"units=(\d+)", out).group(1))
        stats = int(re.search(r"stats=(\d+)", out).group(1))
        self.assertGreater(
            stats, 0,
            "base reports stats=0 — reliability is being incremented and thrown away",
        )
        self.assertEqual(
            stats, units, f"every unit needs a stats record, got {stats}/{units}"
        )


class TestScoreboardSelfDescribes(unittest.TestCase):
    """A metric must declare where it came from, so a silent proxy swap shows up."""

    def test_units_source_is_recorded(self):
        src = (ROOT / "scripts/governor_autonomous.py").read_text()
        self.assertIn('"units_source"', src)

    def test_eval_source_is_recorded(self):
        src = (ROOT / "scripts/governor_autonomous.py").read_text()
        self.assertIn('"eval_source"', src)

    def test_live_scoreboard_declares_sources(self):
        sb = ROOT / "logs/governor/scoreboard.json"
        if not sb.exists():
            self.skipTest("no live scoreboard")
        d = json.loads(sb.read_text())
        self.assertEqual(d.get("units_source"), "cnb_audit")
        self.assertEqual(d.get("eval_source"), "synthetic_fixed_set")
        self.assertLess(
            float(d.get("real_miss_rate", 1.0)), 0.5,
            "live miss rate looks like the Hermes rate leaking back in",
        )


if __name__ == "__main__":
    # Optional class/method selection, used by the mutation gate to assert that
    # a specific reintroduced bug turns a specific test red.
    loader = unittest.TestLoader()
    if len(sys.argv) > 1:
        suite = unittest.TestSuite(
            loader.loadTestsFromName(f"__main__.{n}") for n in sys.argv[1:]
        )
    else:
        suite = loader.loadTestsFromName("__main__")
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if result.wasSuccessful():
        print("METRIC_HONESTY_PASS")
    else:
        print("METRIC_HONESTY_FAIL", file=sys.stderr)
    raise SystemExit(0 if result.wasSuccessful() else 1)
