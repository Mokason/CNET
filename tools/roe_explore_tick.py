#!/usr/bin/env python3
"""ROE explore_tick — offline structured creativity (not AGI, not self-CERT).

Idle organ:
  neuromod headroom (ADO low) → mutate CERT seeds → optional Teacher draft
  → sandbox / structural verify → curriculum hints only (auto_cert=false)

NEVER writes pack_personal. NEVER seals CERT. Blocklist applies.

  python3 tools/roe_explore_tick.py
  python3 tools/roe_explore_tick.py --dry-run
  make roe_explore_tick
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import re
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

_MIN = os.environ.get("CNET_MINIMAL_ROOT", "").strip()
_minp = Path(_MIN) if _MIN else None
PACKS = Path(
    os.environ.get("CNET_PACKS_ROOT")
    or (
        str(_minp / "data" / "roe_daily_packs")
        if _minp and (_minp / "data" / "roe_daily_packs").is_dir()
        else ROOT / "artifacts" / "roe_daily_packs"
    )
)
GOV = Path(os.environ.get("CNET_GOVERNOR_DIR", ROOT / "logs" / "governor"))
SCHEDULE = GOV / "schedule_gate.json"
NEUROMOD = GOV / "neuromod_state.json"
CURRIC_EXPLORE = PACKS / "curriculum_explore.jsonl"
CURRIC_HARVEST = PACKS / "curriculum_harvest.jsonl"
REPORT = PACKS / "EXPLORE_TICK.json"
BLOCKLIST = Path(
    os.environ.get("CNET_BLOCKLIST")
    or (
        str(_minp / "config" / "promote_blocklist.txt")
        if _minp and (_minp / "config" / "promote_blocklist.txt").is_file()
        else ROOT / "config" / "promote_blocklist.txt"
    )
)
BIN_FD = Path(
    os.environ.get("CNET_FRONT_DOOR_BIN")
    or (
        str(_minp / "bin" / "roe_front_door")
        if _minp and (_minp / "bin" / "roe_front_door").is_file()
        else ROOT / "bin" / "roe_front_door"
    )
)

# Explore when ADO is BELOW this (headroom). High ADO = rest/consolidate, no explore.
ADO_EXPLORE_MAX = float(os.environ.get("ROE_EXPLORE_ADO_MAX", "0.50"))
MAX_TRIALS = int(os.environ.get("ROE_EXPLORE_MAX_TRIALS", "6"))
TEACHER = os.environ.get("ROE_EXPLORE_TEACHER", "0") == "1"


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def load_json(path: Path) -> dict:
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


def append_jsonl(path: Path, row: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as f:
        f.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n")


def idle_gate() -> tuple[bool, str, dict]:
    """Allow explore only with ADO headroom and not consolidating hard."""
    sch = load_json(SCHEDULE)
    nm = load_json(NEUROMOD)
    levels = nm.get("levels") or nm
    ado = float(levels.get("adenosine", sch.get("adenosine", 0.35)) or 0.35)
    da = float(levels.get("dopamine", 0.5) or 0.5)
    srt = float(levels.get("serotonin", 0.55) or 0.55)
    meta = {"adenosine": ado, "dopamine": da, "serotonin": srt, "schedule": sch}

    if sch.get("pause_grow_probes") or sch.get("defer_new_explore"):
        return False, "schedule_defer_explore", meta
    if ado >= ADO_EXPLORE_MAX:
        return False, f"ado_high:{ado:.3f}>={ADO_EXPLORE_MAX}", meta
    # optional load proxy: loadavg
    try:
        load1 = os.getloadavg()[0]
        cpus = os.cpu_count() or 1
        meta["load1"] = load1
        meta["cpus"] = cpus
        if load1 > cpus * 0.85:
            return False, f"load_high:{load1:.2f}", meta
    except OSError:
        pass
    return True, "ado_headroom", meta


def load_blocklist() -> list[tuple[str, str]]:
    rules = []
    if not BLOCKLIST.is_file():
        return rules
    for line in BLOCKLIST.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "|" not in line:
            continue
        k, v = line.split("|", 1)
        rules.append((k.strip().lower(), v.strip()))
    return rules


def blocked(q: str, ans: str, rules: list[tuple[str, str]]) -> str | None:
    try:
        from roe_evolve_tick import is_promote_blocked

        return is_promote_blocked(q, ans, rules if rules else None)
    except Exception:
        nq = (q or "").lower()
        al = (ans or "").lower()
        if al.startswith("abstain"):
            return "block_abstain"
        for k, v in rules:
            if k == "substr" and v.lower() in nq:
                return f"block_substr:{v}"
        return None


def collect_cert_seeds(limit: int = 80) -> list[dict]:
    """Seed queries from pack queries_train + ROUTES patterns."""
    seeds: list[dict] = []
    seen = set()
    routes = PACKS / "ROUTES.jsonl"
    if routes.is_file():
        for ln in routes.read_text(encoding="utf-8", errors="replace").splitlines():
            ln = ln.strip()
            if not ln:
                continue
            try:
                r = json.loads(ln)
            except json.JSONDecodeError:
                continue
            pat = (r.get("pattern") or "").strip()
            if len(pat) < 4 or pat.lower() in seen:
                continue
            seen.add(pat.lower())
            seeds.append(
                {
                    "q": pat,
                    "pack": r.get("pack") or "",
                    "skill": r.get("skill") or "",
                    "source": "routes",
                }
            )
    for qf in PACKS.glob("pack_*/queries_train.txt"):
        pack = qf.parent.name
        for line in qf.read_text(encoding="utf-8", errors="replace").splitlines():
            q = line.strip()
            if len(q) < 6 or q.lower() in seen:
                continue
            # skip known OOD markers in train files
            if any(x in q.lower() for x in ("zz", "mystic", "quantum flute", "galactic")):
                continue
            seen.add(q.lower())
            seeds.append({"q": q, "pack": pack, "skill": "", "source": "queries_train"})
    return seeds[:limit]


# Deterministic mutation operators (no ML)
_MUTATORS = [
    lambda q: q + " with malformed empty args",
    lambda q: q + " when tool returns error 127",
    lambda q: q + " under -Werror and tight buffers",
    lambda q: q + " offline no network",
    lambda q: q.replace("how", "how not") if "how" in q else q + " fail-closed recovery",
    lambda q: "boundary: " + q,
    lambda q: q + " after weight_epoch bump",
    lambda q: q + " with stream_ix budget 32",
]


def mutate(seed_q: str, rng: random.Random) -> str:
    op = rng.choice(_MUTATORS)
    out = op(seed_q).strip()
    # clamp length
    return out[:180]


def teacher_draft(q: str) -> str | None:
    if not TEACHER or not BIN_FD.is_file():
        return None
    env = {
        **os.environ,
        "ROE_LIVE": "1",
        "ROE_LLM": "1",
        "ROE_LLM_THINK": "0",
        "ROE_LOOKUP": "0",
    }
    cmd = [str(BIN_FD), "ask", q]
    if PACKS.is_dir():
        cmd.extend(["--root", str(PACKS)])
    try:
        p = subprocess.run(
            cmd, cwd=str(ROOT), capture_output=True, text=True, timeout=90, env=env
        )
        out = p.stdout or ""
        for line in out.splitlines():
            if line.startswith("A:"):
                return line[2:].strip()
    except (OSError, subprocess.SubprocessError):
        return None
    return None


def synthetic_draft(q: str, pack: str) -> str:
    """Deterministic non-LLM draft hypothesis (always available)."""
    base = (
        f"EXPLORE hypothesis for [{pack or 'general'}]: "
        f"handle «{q}» fail-closed; prefer LOCAL CERT; "
        f"verify with tools; never self-CERT; log miss if outside coverage."
    )
    if "werror" in q.lower() or "format" in q.lower() or "c " in q.lower():
        base += (
            " C sketch: use path_join2/bounded memcpy; "
            "/* explore */ int probe(char *dst, size_t n, const char *a){ "
            "if(!dst||n<2||!a)return -1; dst[0]=0; return 0; }"
        )
    if "systemd" in q.lower() or "systemctl" in q.lower():
        base += " Ops sketch: systemctl --user is-active UNIT || true; never start stopped GPU jobs unasked."
    if "error 127" in q.lower() or "malformed" in q.lower():
        base += " Recovery: detect missing binary; abstain or install path; do not invent success."
    return base[:500]


_CODE_C = re.compile(r"```(?:c|cpp)\s*([\s\S]*?)```", re.I)
_CODE_SH = re.compile(r"```(?:bash|sh|shell)\s*([\s\S]*?)```", re.I)
_CODE_C_INLINE = re.compile(r"\bint\s+\w+\s*\([^)]*\)\s*\{[^}]+\}")


def sandbox_verify(draft: str) -> tuple[bool, str, dict]:
    """Hard checks: blocklist-safe structure + optional compile/bash -n."""
    info: dict = {"checks": []}
    if not draft or len(draft.strip()) < 24:
        return False, "too_short", info
    if draft.lower().startswith("abstain"):
        return False, "abstain_draft", info
    if "never self-cert" in draft.lower() and "self-cert" == draft.lower().strip():
        return False, "empty_law_parrot", info

    # Extract C snippets
    c_snips = _CODE_C.findall(draft)
    if not c_snips:
        m = _CODE_C_INLINE.search(draft)
        if m:
            c_snips = [m.group(0)]
    for i, snip in enumerate(c_snips[:2]):
        with tempfile.TemporaryDirectory(prefix="cnet_explore_") as td:
            src = Path(td) / f"probe_{i}.c"
            # wrap fragment if needed
            body = snip.strip()
            if "main(" not in body and "int " in body:
                body = body + "\n" if body.endswith("}") else body
                wrap = f"#include <stddef.h>\n{body}\n" if "main" not in body else body
                if "main" not in wrap:
                    wrap = (
                        "#include <stddef.h>\n"
                        + body
                        + "\nint main(void){ return 0; }\n"
                    )
            else:
                wrap = body if "#include" in body else "#include <stddef.h>\n" + body
            src.write_text(wrap, encoding="utf-8")
            try:
                p = subprocess.run(
                    ["gcc", "-std=c11", "-fsyntax-only", "-Wall", str(src)],
                    capture_output=True,
                    text=True,
                    timeout=15,
                )
                info["checks"].append(
                    {"lang": "c", "rc": p.returncode, "err": (p.stderr or "")[:200]}
                )
                if p.returncode != 0:
                    return False, "c_syntax_fail", info
            except FileNotFoundError:
                info["checks"].append({"lang": "c", "rc": -1, "err": "no_gcc"})
                # no gcc: do not pass code claims
                return False, "no_gcc", info
            except subprocess.TimeoutExpired:
                return False, "c_timeout", info

    sh_snips = _CODE_SH.findall(draft)
    for i, snip in enumerate(sh_snips[:2]):
        with tempfile.TemporaryDirectory(prefix="cnet_explore_sh_") as td:
            sh = Path(td) / f"probe_{i}.sh"
            sh.write_text(snip.strip() + "\n", encoding="utf-8")
            try:
                p = subprocess.run(
                    ["bash", "-n", str(sh)],
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                info["checks"].append(
                    {"lang": "bash", "rc": p.returncode, "err": (p.stderr or "")[:200]}
                )
                if p.returncode != 0:
                    return False, "bash_syntax_fail", info
            except Exception as e:
                return False, f"bash_check_err:{e}", info

    # Non-code drafts: require operational keywords (structured, not freeform poetry)
    if not c_snips and not sh_snips:
        keys = ("fail-closed", "local", "cert", "verify", "never self-cert", "abstain", "tool")
        hit = sum(1 for k in keys if k in draft.lower())
        info["checks"].append({"lang": "prose", "keyword_hits": hit})
        if hit < 2:
            return False, "prose_too_weak", info

    return True, "sandbox_ok", info


def trial_id(q: str, draft: str) -> str:
    h = hashlib.sha1((q + "\n" + draft).encode()).hexdigest()[:12]
    return f"explore_{h}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true", help="ignore idle neuromod gate")
    ap.add_argument("--max-trials", type=int, default=MAX_TRIALS)
    ap.add_argument("--teacher", action="store_true", help="allow Teacher drafts")
    args = ap.parse_args()
    global TEACHER
    if args.teacher:
        TEACHER = True

    ok_gate, reason, meta = idle_gate()
    report = {
        "ts": utc_now(),
        "gate_ok": ok_gate or args.force,
        "gate_reason": reason if not args.force else f"forced:{reason}",
        "neuromod": meta,
        "teacher": TEACHER,
        "dry_run": args.dry_run,
        "trials": [],
        "queued": [],
        "skipped": [],
        "auto_cert": False,
        "writes_pack_personal": False,
        "law": "never_self_cert; curriculum hints only",
    }

    if not (ok_gate or args.force):
        report["note"] = "explore deferred — no ADO headroom / load / schedule"
        if not args.dry_run:
            REPORT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"explore_tick DEFER reason={reason} ado={meta.get('adenosine')}")
        print("ROE_EXPLORE_TICK_PASS")  # defer is success (idle organ)
        print(f"  report → {REPORT}")
        return 0

    # charter budget (optional)
    try:
        sys.path.insert(0, str(ROOT / "scripts"))
        import cnet_autonomy_charter as ac  # type: ignore

        ch = ac.check_action("explore", tick_promotes=0)
        if not ch.get("ok", True) and not args.force:
            report["skipped"].append({"reason": "charter_explore", "detail": ch})
            REPORT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            print("explore_tick DEFER charter")
            print("ROE_EXPLORE_TICK_PASS")
            return 0
    except Exception:
        pass

    seeds = collect_cert_seeds()
    if not seeds:
        report["skipped"].append({"reason": "no_seeds"})
        REPORT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print("explore_tick no seeds")
        print("ROE_EXPLORE_TICK_PASS")
        return 0

    day = datetime.now(timezone.utc).strftime("%Y%m%d")
    rng = random.Random(int(hashlib.sha1(day.encode()).hexdigest()[:8], 16) ^ int(time.time()) // 3600)
    rules = load_blocklist()
    rng.shuffle(seeds)
    n_ok = 0

    for seed in seeds:
        if n_ok >= args.max_trials:
            break
        q0 = seed["q"]
        q = mutate(q0, rng)
        br0 = blocked(q, "x", rules)
        if br0:
            report["skipped"].append({"query": q, "reason": br0})
            continue

        draft = None
        src = "synthetic"
        if TEACHER:
            draft = teacher_draft(q)
            if draft:
                src = "teacher"
        if not draft:
            draft = synthetic_draft(q, seed.get("pack") or "")
            src = "synthetic"

        br = blocked(q, draft, rules)
        if br:
            report["skipped"].append({"query": q, "reason": br})
            continue

        ok_sb, sb_reason, sb_info = sandbox_verify(draft)
        trial = {
            "id": trial_id(q, draft),
            "seed_q": q0,
            "query": q,
            "pack": seed.get("pack"),
            "draft_source": src,
            "draft_preview": draft[:160],
            "sandbox_ok": ok_sb,
            "sandbox_reason": sb_reason,
            "sandbox": sb_info,
        }
        report["trials"].append(trial)
        if not ok_sb:
            report["skipped"].append({"query": q, "reason": f"sandbox:{sb_reason}"})
            continue

        row = {
            "ts": utc_now(),
            "query": q,
            "answer": draft,
            "source": "explore_tick",
            "draft_source": src,
            "seed_q": q0,
            "pack": seed.get("pack") or "",
            "auto_cert": False,
            "promote_ok_via": "gold_or_multi_stable_reviewer_only",
            "sandbox_reason": sb_reason,
            "explore_id": trial["id"],
            "never_self_cert": True,
        }
        if not args.dry_run:
            append_jsonl(CURRIC_EXPLORE, row)
            append_jsonl(CURRIC_HARVEST, row)
        report["queued"].append(
            {"id": trial["id"], "query": q[:80], "draft_source": src}
        )
        n_ok += 1

    if not args.dry_run:
        REPORT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    print(f"explore_tick gate={report['gate_reason']} teacher={TEACHER}")
    print(f"  seeds={len(seeds)} queued={len(report['queued'])} trials={len(report['trials'])}")
    print(f"  skipped={len(report['skipped'])} auto_cert=false pack_personal=never")
    for q in report["queued"][:8]:
        print(f"  + {q['id']} {q['draft_source']} {q['query'][:50]}")
    print(f"  curriculum → {CURRIC_EXPLORE}")
    print(f"  report → {REPORT}")
    print("ROE_EXPLORE_TICK_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
