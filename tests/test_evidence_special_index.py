#!/usr/bin/env python3
"""A tracked file the index was told to stop watching is still part of the tree.

WHY THIS EXISTS. Both canonical evidence runners --
``scripts/gate_evidence.py`` and ``tests/run_capability_cert.py`` -- bound the
working tree by walking ``git status --porcelain``. That command deliberately
does not report paths marked ``assume-unchanged`` or ``skip-worktree``: the
whole point of those bits is to tell git to stop stat-ing the file. Both runners
noticed the gap and recorded a *count* of such paths next to the digest, which
proves only how many blind spots there were, not that nothing moved inside one.

In this repository that is not a hypothetical: 83 of 2020 tracked paths carry
the assume-unchanged bit, including ``src/nn.c``, ``include/nn.h`` and
``src/legacy/main.c`` -- the trainer, its public header, and the legacy
persistence path. A producer could rewrite any of them mid-run and both runners
would emit an unchanged binding and a green verdict.

Every case below builds a DISPOSABLE git repository under ``mkdtemp`` and runs
the real runners inside it, copied verbatim so their ``parents[1]`` root is the
throwaway tree. Nothing here touches the CNET repository, no network is used,
and each subprocess is bounded by an explicit timeout.

The mutated file is deliberately NOT a declared evaluator source. A declared
source is already bound by ``source_set_digest``; the hole being tested is the
*worktree* binding, so the fixture must move a file that only the worktree
binding could ever have seen.

Usage: python3 tests/test_evidence_special_index.py
Exit 0 = both runners bind special-index paths, 1 = at least one does not.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TIMEOUT = 180

failures: list[str] = []
checks = 0


def check(condition: bool, message: str) -> None:
    global checks
    checks += 1
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}")


def run(args: list[str], cwd: Path, env: dict[str, str] | None = None):
    return subprocess.run(
        args,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=TIMEOUT,
        check=False,
        env=env,
    )


def git(repo: Path, *args: str) -> None:
    done = run(["git", *args], repo)
    if done.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed:\n{done.stdout}")


WATCHED = "src/watched.c"
WATCHED_ORIGINAL = "/* the trainer nobody is watching */\nint watched(void) { return 1; }\n"


def build_repo(tmp: Path) -> Path:
    """A throwaway repo holding both real runners and one watched-then-ignored file."""
    repo = tmp / "repo"
    (repo / "scripts").mkdir(parents=True)
    (repo / "tests").mkdir(parents=True)
    (repo / "src").mkdir(parents=True)
    (repo / "logs").mkdir(parents=True)
    (repo / "config" / "capability_manifests").mkdir(parents=True)
    (repo / "tests" / "fixtures").mkdir(parents=True)

    # CNET_EVIDENCE_LEGACY=<rev> installs the runners as they were at <rev>
    # instead of the working tree, so the RED this gate was written against
    # stays re-runnable instead of being a screenshot in a report.
    legacy = os.environ.get("CNET_EVIDENCE_LEGACY")
    for source, destination, required in (
        ("scripts/gate_evidence.py", repo / "scripts" / "gate_evidence.py", True),
        ("tests/run_capability_cert.py",
         repo / "tests" / "run_capability_cert.py", True),
        # The prerequisite module the runner imports. It does not exist at the
        # legacy revisions this gate replays, and the runners there do not
        # import it, so its absence THERE is expected -- while its absence in
        # the working tree would silently turn every capability case into an
        # import error that reads like a refusal.
        ("scripts/capability_evaluator_prereq.py",
         repo / "scripts" / "capability_evaluator_prereq.py", False),
    ):
        if legacy:
            shown = subprocess.run(
                ["git", "show", f"{legacy}:{source}"],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                timeout=60, check=False,
            )
            if shown.returncode != 0:
                if required:
                    raise RuntimeError(
                        f"{source} does not exist at {legacy}, and this gate "
                        "cannot replay a runner it cannot install"
                    )
                continue
            destination.write_bytes(shown.stdout)
        else:
            shutil.copy2(ROOT / source, destination)
    (repo / WATCHED).write_text(WATCHED_ORIGINAL, encoding="utf-8")

    # A capability whose evaluator declares ONLY itself, so nothing about the
    # watched file is bound by the declared-source digest.
    fixture = {
        "capability_id": "special_index_probe",
        "cases": [{"id": "only-case", "prompt": "ping", "expected": "pong"}],
        "expected_markers": ["PROBE_MARKER"],
    }
    fixture_path = repo / "tests" / "fixtures" / "capability_special_index_probe.json"
    fixture_path.write_text(json.dumps(fixture, indent=2) + "\n", encoding="utf-8")
    fixture_sha = hashlib.sha256(fixture_path.read_bytes()).hexdigest()

    evaluator = repo / "tests" / "probe_evaluator.py"
    evaluator.write_text(
        "#!/usr/bin/env python3\n"
        "import hashlib, os, sys\n"
        "fixture = os.environ['CNET_HELD_OUT_FIXTURE']\n"
        "sha = hashlib.sha256(open(fixture,'rb').read()).hexdigest()\n"
        "print('HELDOUT_CASE id=only-case reads=1')\n"
        "print(f'HELDOUT_FIXTURE capability=special_index_probe sha256={sha} "
        "cases=1 consumed=1')\n"
        "print('HELDOUT_METRIC cases_passed=1 cases_declared=1 metric=1.000 errors=0')\n"
        "print('PROBE_MARKER')\n"
        "print('SPECIAL_INDEX_PROBE_PASS')\n"
        "if os.environ.get('PROBE_MUTATE'):\n"
        "    open(os.environ['PROBE_MUTATE'],'a').write('/* mutated mid-run */\\n')\n",
        encoding="utf-8",
    )
    evaluator.chmod(0o755)

    # The evaluator allowlist is `make`/`dotnet` and stays that way -- the probe
    # goes through a one-line Makefile rather than relaxing it.
    (repo / "Makefile").write_text(
        "probe:\n\t@%s tests/probe_evaluator.py\n" % sys.executable,
        encoding="utf-8",
    )

    manifest = {
        "schema_version": 1,
        "capability_id": "special_index_probe",
        "title": "Special index probe",
        "owner": "integrity",
        "held_out_fixture": "tests/fixtures/capability_special_index_probe.json",
        "evaluator": ["make", "probe"],
        "evaluator_sources": ["tests/probe_evaluator.py"],
        "required_marker": "SPECIAL_INDEX_PROBE_PASS",
        "evidence_artifact": "logs/special_index_probe.log",
        "failure_envelope": "Fails if the probe cannot read its fixture.",
        "metric_source": "heldout_receipt",
        "absolute_floor": 1.0,
        "baseline_metric": 1.0,
        "regression_budget": 0.0,
        "timeout_seconds": 60,
    }
    (repo / "config" / "capability_manifests" / "special_index_probe.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    # `__pycache__/` is ignored here for the same reason it is ignored in CNET:
    # importing the runner writes a bytecode cache, and an ignored build
    # artifact is not the untracked change this gate is looking for.
    (repo / ".gitignore").write_text("logs/\n__pycache__/\n", encoding="utf-8")

    git(repo, "init", "-q", "-b", "main")
    git(repo, "config", "user.email", "integrity@example.invalid")
    git(repo, "config", "user.name", "integrity")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "fixture")
    # The bit under test: git is told to stop watching a tracked source.
    git(repo, "update-index", "--assume-unchanged", WATCHED)

    listed = run(["git", "ls-files", "-v", WATCHED], repo).stdout.strip()
    if not listed.startswith("h "):
        raise RuntimeError(f"assume-unchanged not set: {listed!r}")
    _ = fixture_sha
    return repo


def special_digest(repo: Path) -> str | None:
    """The special-index digest the last gate_evidence run recorded, if any.

    A runner that never emitted one returns None rather than raising, so the
    legacy lane reports a missing binding as a failure instead of a traceback.
    """
    evidence = repo / "logs" / "probe.log.evidence.json"
    try:
        binding = json.loads(evidence.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return binding.get("binding_pre", {}).get("special_index_sha256")


def status_is_blind(repo: Path) -> bool:
    """Confirm the premise: git status really does not report the mutation."""
    return run(["git", "status", "--porcelain=v1"], repo).stdout.strip() == ""


def gate_evidence(repo: Path, producer: list[str]):
    return run(
        [
            sys.executable,
            "scripts/gate_evidence.py",
            "probe",
            "logs/probe.log",
            "SPECIAL_INDEX_PROBE_PASS",
            "--",
            *producer,
        ],
        repo,
    )


def capability_cert(repo: Path, env_extra: dict[str, str] | None = None):
    env = dict(os.environ)
    env.pop("PROBE_MUTATE", None)
    if env_extra:
        env.update(env_extra)
    return run(
        [sys.executable, "tests/run_capability_cert.py",
         "--output", "logs/probe_cert.json"],
        repo,
        env=env,
    )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="cnet-special-index-") as raw:
        tmp = Path(raw)

        # ---- case 1: gate_evidence, producer mutates the unwatched file -----
        repo = build_repo(tmp)
        mutator = (
            f"import pathlib; print('SPECIAL_INDEX_PROBE_PASS'); "
            f"pathlib.Path('{WATCHED}').write_text('/* rewritten mid-run */\\n')"
        )
        done = gate_evidence(repo, [sys.executable, "-c", mutator])
        print(f"GATE_EVIDENCE_MUTATED exit={done.returncode}")
        check(
            (repo / WATCHED).read_text(encoding="utf-8") != WATCHED_ORIGINAL,
            "the producer really did rewrite the assume-unchanged file",
        )
        check(
            status_is_blind(repo),
            "premise: git status is blind to the mutation (else this proves nothing)",
        )
        check(
            done.returncode != 0,
            "gate_evidence must REFUSE a run that rewrote an assume-unchanged "
            f"tracked file (exit was {done.returncode})",
        )
        check(
            "bound_state_changed" in done.stdout,
            "gate_evidence must name the drift it refused on",
        )

        # ---- case 2: gate_evidence control, nothing moves -------------------
        repo = build_repo(tmp / "clean")
        done = gate_evidence(
            repo, [sys.executable, "-c", "print('SPECIAL_INDEX_PROBE_PASS')"]
        )
        print(f"GATE_EVIDENCE_PRISTINE exit={done.returncode}")
        check(
            done.returncode == 0,
            "a pristine tree with special-index paths must still PASS "
            f"(exit was {done.returncode}); a binding that refuses everything "
            "proves nothing",
        )

        # ---- case 3: capability cert, evaluator mutates the unwatched file --
        repo = build_repo(tmp / "cert")
        done = capability_cert(repo, {"PROBE_MUTATE": str(repo / WATCHED)})
        print(f"CAPABILITY_CERT_MUTATED exit={done.returncode}")
        check(
            (repo / WATCHED).read_text(encoding="utf-8") != WATCHED_ORIGINAL,
            "the evaluator really did append to the assume-unchanged file",
        )
        check(
            status_is_blind(repo),
            "premise: git status is blind to the evaluator's mutation",
        )
        check(
            done.returncode != 0,
            "run_capability_cert must REFUSE to certify a run that rewrote an "
            f"assume-unchanged tracked file (exit was {done.returncode})",
        )
        check(
            "bound state changed" in done.stdout,
            "run_capability_cert must name the drift it refused on",
        )

        # ---- case 4: capability cert control, nothing moves -----------------
        repo = build_repo(tmp / "cert-clean")
        done = capability_cert(repo)
        print(f"CAPABILITY_CERT_PRISTINE exit={done.returncode}")
        check(
            done.returncode == 0,
            "a pristine tree with special-index paths must still certify "
            f"(exit was {done.returncode})",
        )

        # ---- case 5: a symlink is bound by identity, never by target --------
        # Following a symlink means reading whatever it points at: a path
        # outside the repo, or a device that never returns. The binding must
        # record where the link points and never open it.
        repo = build_repo(tmp / "symlink")
        link = repo / "src" / "pointer"
        os.symlink("/dev/zero", link)
        git(repo, "add", "src/pointer")
        git(repo, "commit", "-q", "-m", "symlink")
        git(repo, "update-index", "--assume-unchanged", "src/pointer")
        done = gate_evidence(
            repo, [sys.executable, "-c", "print('SPECIAL_INDEX_PROBE_PASS')"]
        )
        print(f"GATE_EVIDENCE_SYMLINK exit={done.returncode}")
        check(
            done.returncode == 0,
            "a symlink to /dev/zero among the bound paths must not hang or fail "
            f"the gate (exit was {done.returncode})",
        )
        zero_digest = special_digest(repo)
        check(
            zero_digest is not None,
            "the evidence JSON must record a special-index digest; a runner "
            "that does not emit one cannot have bound those paths",
        )

        # Repoint it and run again. /dev/zero and /dev/null both read as empty,
        # so a binding that FOLLOWED the link would produce the same digest for
        # two different trees. Identity must move.
        link.unlink()
        os.symlink("/dev/null", link)
        done = gate_evidence(
            repo, [sys.executable, "-c", "print('SPECIAL_INDEX_PROBE_PASS')"]
        )
        print(f"GATE_EVIDENCE_SYMLINK_REPOINTED exit={done.returncode}")
        check(
            done.returncode == 0,
            f"a symlink to /dev/null must also not hang (exit {done.returncode})",
        )
        null_digest = special_digest(repo)
        check(
            zero_digest is not None and zero_digest != null_digest,
            "a bound symlink must be identified by WHERE IT POINTS: /dev/zero "
            "and /dev/null both read as empty, so an equal digest proves the "
            "binding followed the link instead of recording it",
        )

        # ... and repointing it DURING a run is drift, like any other change.
        repointer = (
            "import os, pathlib; print('SPECIAL_INDEX_PROBE_PASS'); "
            "p='src/pointer'; os.unlink(p); os.symlink('/dev/zero', p)"
        )
        done = gate_evidence(repo, [sys.executable, "-c", repointer])
        print(f"GATE_EVIDENCE_SYMLINK_MOVED exit={done.returncode}")
        check(
            done.returncode != 0,
            "repointing a bound symlink mid-run must be refused as drift "
            f"(exit was {done.returncode})",
        )

    if failures:
        print(f"EVIDENCE_SPECIAL_INDEX_FAIL checks={checks} failures={len(failures)}")
        return 1
    print(
        f"EVIDENCE_SPECIAL_INDEX_PASS checks={checks} runners=2 "
        "special_index=bound symlinks=identity_only"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
