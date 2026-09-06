#!/usr/bin/env python3
"""ROE evolve tick — unattended gardener (not AGI).

Goal: learn / mint capsules WITHOUT the human pressing go/accept every turn.

Auto-promote ONLY when a fail-closed policy says so:
  gold_file     — artifacts/roe_daily_packs/gold/<sha1>.txt matches teacher answer
  multi_stable  — same (query_norm, answer) seen >= N times in miss_log (default 3)
  reviewer      — separate Ollama REVIEWER role APPROVE (not teacher)

When ROE_EVOLVE_REVIEWER=1 (default if reviewer env present):
  multi_stable / teacher proposals also need reviewer APPROVE.
  gold_file can skip reviewer (gold is already external verify).

Never promotes from a single novel LLM answer (that would be self-CERT).
Persona packs (pack_soul_*) are never auto-written.

Usage:
  python3 tools/roe_evolve_tick.py              # one tick
  python3 tools/roe_evolve_tick.py --dry-run
  make roe_evolve_tick

Env:
  ROE_EVOLVE_STABLE_N=3
  ROE_EVOLVE_MAX_PROMOTES=20
  ROE_EVOLVE_TEACHER=1
  ROE_EVOLVE_REVIEWER=1   # separate reviewer gate (tools/roe_reviewer.py)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Deploy-aware: CNET_PACKS_ROOT / CNET_FRONT_DOOR_BIN / CNET_MINIMAL_ROOT
_MIN = os.environ.get("CNET_MINIMAL_ROOT", "").strip()
if _MIN:
    _minp = Path(_MIN)
else:
    _minp = None
PACKS = Path(
    os.environ.get("CNET_PACKS_ROOT")
    or (
        str(_minp / "data" / "roe_daily_packs")
        if _minp and (_minp / "data" / "roe_daily_packs").is_dir()
        else ROOT / "artifacts" / "roe_daily_packs"
    )
)
MISS = PACKS / "miss_log.jsonl"
GOLD_DIR = PACKS / "gold"
PERSONAL = PACKS / "pack_personal"
REPORT = PACKS / "EVOLVE_TICK.json"
STATE = PACKS / "evolve_state.json"
BIN_FD = Path(
    os.environ.get("CNET_FRONT_DOOR_BIN")
    or (
        str(_minp / "bin" / "roe_front_door")
        if _minp and (_minp / "bin" / "roe_front_door").is_file()
        else ROOT / "bin" / "roe_front_door"
    )
)
BLOCKLIST = Path(
    os.environ.get("CNET_BLOCKLIST")
    or (
        str(_minp / "config" / "promote_blocklist.txt")
        if _minp and (_minp / "config" / "promote_blocklist.txt").is_file()
        else ROOT / "config" / "promote_blocklist.txt"
    )
)

# reviewer helper
sys.path.insert(0, str(ROOT / "tools"))
try:
    import roe_reviewer as roe_reviewer_mod  # type: ignore
except ImportError:
    roe_reviewer_mod = None  # type: ignore


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def norm_q(q: str) -> str:
    q = (q or "").strip().lower()
    q = re.sub(r"\s+", " ", q)
    return q[:200]


def q_hash(q: str) -> str:
    return hashlib.sha1(norm_q(q).encode()).hexdigest()[:16]


def skill_id_for(q: str) -> str:
    return "auto_" + q_hash(q)


def pattern_for(q: str) -> str:
    """Stable substring trigger: first 4+ meaningful tokens, max ~48 chars."""
    toks = re.findall(r"[a-z0-9][a-z0-9\-]{1,}", norm_q(q))
    if not toks:
        return norm_q(q)[:40] or "auto"
    # drop ultra-common stopwords
    stop = {"the", "a", "an", "to", "of", "and", "or", "is", "are", "how", "what", "please"}
    toks = [t for t in toks if t not in stop] or toks
    pat = " ".join(toks[:6])
    return pat[:48]


def load_jsonl(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    out = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return out


def append_jsonl(path: Path, row: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as f:
        f.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n")


def ensure_personal_pack() -> None:
    PERSONAL.mkdir(parents=True, exist_ok=True)
    cat = PERSONAL / "catalog.jsonl"
    if not cat.exists():
        cat.write_text("", encoding="utf-8")
    abi = PERSONAL / "PACK.abi"
    if not abi.exists():
        abi.write_text(
            "\n".join(
                [
                    "ROE_DAILY_PACK",
                    "abi_version 1",
                    "id pack_personal",
                    "title Unattended promote garden (verified only)",
                    "cat personal",
                    "kind garden",
                    "never_self_cert 1",
                    "second_brain 0",
                    "seal_path forbidden",
                    "load_policy always_on_optional",
                    "",
                ]
            ),
            encoding="utf-8",
        )
    (PERSONAL / "MANIFEST.txt").write_text(
        "pack_personal: auto-promoted skills after gold/multi_stable only.\n"
        "never_self_cert:1  human button not required per turn.\n",
        encoding="utf-8",
    )


def load_catalog_ids(path: Path) -> set[str]:
    ids = set()
    for row in load_jsonl(path):
        if "id" in row:
            ids.add(row["id"])
    return ids


def write_skill(pack_dir: Path, sid: str, pattern: str, answer: str, intent: str = "auto") -> None:
    sk = pack_dir / "skills" / sid
    sk.mkdir(parents=True, exist_ok=True)
    (sk / "SKILL.roe").write_text(
        "\n".join(
            [
                "ROE_SKILL 1",
                f"id {sid}",
                f"intent {intent}",
                f"pattern {pattern}",
                f"answer {answer.replace(chr(10), ' ').strip()[:500]}",
                "privilege 0",
                "certified 1",
                "",
            ]
        ),
        encoding="utf-8",
    )
    (sk / "manifest.roe").write_text(
        "\n".join(
            [
                "ROE_MANIFEST 1",
                f"skill {sid}",
                "format roe_text_capsule",
                "never_self_cert 1",
                "second_brain 0",
                "provenance evolve_tick_auto_policy",
                "",
            ]
        ),
        encoding="utf-8",
    )
    # append catalog if new
    cat = pack_dir / "catalog.jsonl"
    existing = load_catalog_ids(cat)
    if sid in existing:
        # rewrite line
        rows = [r for r in load_jsonl(cat) if r.get("id") != sid]
    else:
        rows = load_jsonl(cat)
    rows.append(
        {
            "id": sid,
            "intent": intent,
            "pattern": pattern,
            "answer": answer.replace("\n", " ").strip()[:500],
            "privilege": 0,
            "hits": 0,
        }
    )
    cat.write_text(
        "\n".join(json.dumps(r, ensure_ascii=False, separators=(",", ":")) for r in rows) + "\n",
        encoding="utf-8",
    )


def gold_path(q: str) -> Path:
    return GOLD_DIR / f"{q_hash(q)}.txt"


def load_blocklist(path: Path | None = None) -> list[tuple[str, str]]:
    p = path or BLOCKLIST
    rules: list[tuple[str, str]] = []
    if not p.is_file():
        return rules
    for line in p.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "|" not in line:
            continue
        kind, val = line.split("|", 1)
        rules.append((kind.strip().lower(), val.strip()))
    return rules


def is_promote_blocked(query: str, answer: str | None = None,
                       rules: list[tuple[str, str]] | None = None) -> str | None:
    """Return block reason or None. Blocks probe junk and ABSTAIN-as-answer."""
    rules = rules if rules is not None else load_blocklist()
    nq = norm_q(query)
    ans = (answer or "").strip()
    ans_l = ans.lower()
    for kind, val in rules:
        v = val.lower()
        if kind == "substr" and v in nq:
            return f"blocklist_substr:{val}"
        if kind == "answer_prefix" and ans_l.startswith(v):
            return f"blocklist_answer:{val}"
        if kind == "regex":
            try:
                if re.search(val, nq, re.I):
                    return f"blocklist_regex:{val}"
            except re.error:
                continue
    # Hard law even if blocklist file missing
    if ans_l.startswith("abstain") or ans_l.startswith("abstain:"):
        return "blocklist_answer:abstain"
    if "no local skill" in ans_l and "ask user" in ans_l:
        return "blocklist_answer:no_local_skill_abstain"
    return None


def load_gold(q: str) -> str | None:
    p = gold_path(q)
    if p.is_file():
        t = p.read_text(encoding="utf-8", errors="replace").strip()
        return t or None
    # also gold.jsonl
    gj = PACKS / "gold.jsonl"
    nq = norm_q(q)
    for row in load_jsonl(gj):
        if norm_q(row.get("query", "")) == nq and row.get("answer"):
            return str(row["answer"]).strip()
    return None


def answers_match(a: str, b: str) -> bool:
    def c(x: str) -> str:
        x = x.lower().strip()
        x = re.sub(r"\s+", " ", x)
        return x[:400]

    return c(a) == c(b) or c(a) in c(b) or c(b) in c(a)


def cluster_misses(rows: list[dict]) -> dict[str, list[dict]]:
    g: dict[str, list[dict]] = defaultdict(list)
    for r in rows:
        q = r.get("query") or r.get("q") or ""
        if not q:
            continue
        g[norm_q(q)].append(r)
    return g


def pick_stable_answer(entries: list[dict]) -> tuple[str | None, int, str]:
    """Return (answer, count, reason_candidate)."""
    counts: dict[str, int] = defaultdict(int)
    for e in entries:
        ans = (e.get("answer") or e.get("teacher_answer") or e.get("snippet") or "").strip()
        if not ans:
            continue
        # strip tags
        ans = re.sub(r"^\[(llm-live|llm-untrusted|lookup)\]\s*", "", ans)
        counts[ans] += 1
    if not counts:
        return None, 0, ""
    best = max(counts.items(), key=lambda kv: kv[1])
    return best[0], best[1], "multi_stable"


def fd_cmd(q: str) -> list[str]:
    """front_door ask with optional package packs root."""
    cmd = [str(BIN_FD), "ask", q]
    if PACKS.is_dir():
        cmd.extend(["--root", str(PACKS)])
    return cmd


def already_local(q: str) -> bool:
    if not BIN_FD.is_file():
        return False
    try:
        p = subprocess.run(
            fd_cmd(q),
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            timeout=30,
            env={**os.environ, "ROE_LIVE": "0", "ROE_LLM": "0"},
        )
        out = (p.stdout or "") + (p.stderr or "")
        return "source=LOCAL" in out or "src=LOCAL" in out
    except (subprocess.SubprocessError, OSError):
        return False


def try_teacher_fill(q: str) -> str | None:
    """Optional live teacher for open miss (still untrusted until policy)."""
    if not BIN_FD.is_file():
        return None
    env = {
        **os.environ,
        "ROE_LIVE": os.environ.get("ROE_LIVE", "1"),
        "ROE_LLM": os.environ.get("ROE_LLM", "1"),
        "ROE_LOOKUP": os.environ.get("ROE_LOOKUP", "0"),
        "ROE_LLM_THINK": os.environ.get("ROE_LLM_THINK", "0"),
        "ROE_LLM_MODEL": os.environ.get("ROE_LLM_MODEL", "deepseek-v4-flash:cloud"),
    }
    # load teacher env if present
    te = ROOT / "config" / "roe-teacher-ollama-cloud.env"
    if te.is_file():
        for line in te.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            env.setdefault(k.strip(), v.strip().strip('"'))
    try:
        p = subprocess.run(
            fd_cmd(q),
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            timeout=180,
            env=env,
        )
        out = p.stdout or ""
        for line in out.splitlines():
            if line.startswith("A:"):
                return line[2:].strip()
        m = re.search(r"A:\s*(.+)", out)
        return m.group(1).strip() if m else None
    except (subprocess.SubprocessError, OSError):
        return None


def promote_via_front_door(q: str, gold: str, dry: bool) -> bool:
    if dry:
        return True
    if not BIN_FD.is_file():
        # fallback write skill only
        return False
    cmd = [
        str(BIN_FD),
        "ask",
        q,
        "--accept",
        "--gold",
        gold,
        "--promote-pack",
        "pack_personal",
    ]
    if PACKS.is_dir():
        cmd.extend(["--root", str(PACKS)])
    try:
        p = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True, timeout=60)
        out = (p.stdout or "") + (p.stderr or "")
        return "promote=yes" in out or p.returncode == 0
    except (subprocess.SubprocessError, OSError):
        return False


def load_state() -> dict:
    if STATE.is_file():
        try:
            return json.loads(STATE.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            pass
    return {"promoted_ids": [], "last_tick": None, "ticks": 0}


def save_state(st: dict) -> None:
    STATE.write_text(json.dumps(st, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--stable-n", type=int, default=int(os.environ.get("ROE_EVOLVE_STABLE_N", "3")))
    ap.add_argument("--max-promotes", type=int, default=int(os.environ.get("ROE_EVOLVE_MAX_PROMOTES", "20")))
    ap.add_argument(
        "--teacher",
        action="store_true",
        default=os.environ.get("ROE_EVOLVE_TEACHER", "0") in ("1", "true", "yes"),
        help="fill open misses via live teacher (still needs policy to promote)",
    )
    ap.add_argument("--seed-demo", action="store_true", help="write a demo miss+gold for gate")
    ap.add_argument(
        "--reviewer",
        action="store_true",
        default=None,
        help="require separate reviewer APPROVE for non-gold promotes",
    )
    ap.add_argument("--no-reviewer", action="store_true", help="disable reviewer gate")
    args = ap.parse_args()

    # reviewer default: on if env 1 or config file exists, unless --no-reviewer
    if args.no_reviewer:
        use_reviewer = False
    elif args.reviewer:
        use_reviewer = True
    else:
        env_r = os.environ.get("ROE_EVOLVE_REVIEWER", "").lower()
        if env_r in ("0", "false", "no"):
            use_reviewer = False
        elif env_r in ("1", "true", "yes"):
            use_reviewer = True
        else:
            use_reviewer = (ROOT / "config" / "roe-reviewer-ollama-cloud.env").is_file()

    PACKS.mkdir(parents=True, exist_ok=True)
    GOLD_DIR.mkdir(parents=True, exist_ok=True)
    ensure_personal_pack()
    st = load_state()
    promoted_ids = set(st.get("promoted_ids") or [])

    if args.seed_demo:
        demo_q = "roe evolve tick demo query alpha"
        demo_a = "Evolve tick demo answer: unattended promote via gold_file policy."
        append_jsonl(
            MISS,
            {
                "ts": utc_now(),
                "query": demo_q,
                "answer": demo_a,
                "source": "LLM",
                "pack_tried": None,
            },
        )
        gold_path(demo_q).write_text(demo_a + "\n", encoding="utf-8")
        # multi stable copies
        for _ in range(2):
            append_jsonl(
                MISS,
                {
                    "ts": utc_now(),
                    "query": demo_q,
                    "answer": demo_a,
                    "source": "LLM",
                },
            )

    rows = load_jsonl(MISS)
    clusters = cluster_misses(rows)

    policy = ["gold_file", f"multi_stable>={args.stable_n}"]
    if use_reviewer:
        policy.append("reviewer_approve_non_gold")

    report = {
        "ts": utc_now(),
        "miss_rows": len(rows),
        "clusters": len(clusters),
        "stable_n": args.stable_n,
        "promoted": [],
        "skipped": [],
        "reviewer_enabled": use_reviewer,
        "dry_run": args.dry_run,
        "policy": policy,
        "human_accept_required": False,
        "roles": {"teacher": "propose", "reviewer": "gate", "human": "optional_gold_batch"},
        "note": "teacher≠reviewer; gold_file skips reviewer; multi_stable needs reviewer if enabled",
    }

    n_prom = 0
    for nq, entries in sorted(clusters.items(), key=lambda kv: -len(kv[1])):
        if n_prom >= args.max_promotes:
            break
        # representative query (original casing from last entry)
        q = entries[-1].get("query") or nq
        sid = skill_id_for(q)

        # Blocklist first — probe/ABSTAIN never CERT (even if stale state id)
        br0 = is_promote_blocked(q, None)
        if br0:
            report["skipped"].append({"query": q, "reason": br0})
            promoted_ids.discard(sid)
            continue

        if sid in promoted_ids or sid in load_catalog_ids(PERSONAL / "catalog.jsonl"):
            report["skipped"].append({"query": q, "reason": "already_promoted"})
            continue
        if already_local(q):
            report["skipped"].append({"query": q, "reason": "already_local"})
            continue

        gold = load_gold(q)
        ans, cnt, _ = pick_stable_answer(entries)

        # optional teacher fill if no answer yet
        if args.teacher and not ans and not gold:
            t = try_teacher_fill(q)
            if t:
                ans = t
                append_jsonl(
                    MISS,
                    {
                        "ts": utc_now(),
                        "query": q,
                        "answer": t,
                        "source": "LLM",
                        "via": "evolve_teacher",
                    },
                )
                cnt = 1

        reason = None
        final = None
        if gold:
            # Gold is external verify — always wins over teacher text.
            # (Teacher mismatch is logged but does not block gold promote.)
            if ans and not answers_match(ans, gold):
                reason = "gold_file_overrides_teacher"
            else:
                reason = "gold_file"
            final = gold
        elif ans and cnt >= args.stable_n:
            final = ans
            reason = f"multi_stable_{cnt}"
        else:
            report["skipped"].append(
                {
                    "query": q,
                    "reason": "need_gold_or_stable",
                    "stable_cnt": cnt,
                    "need": args.stable_n,
                    "has_gold": bool(gold),
                }
            )
            continue

        # Promote path blocklist: probe junk + ABSTAIN answers never CERT
        br = is_promote_blocked(q, final)
        if br:
            report["skipped"].append({"query": q, "reason": br})
            continue

        # never auto-write soul packs
        if "soul" in nq and "who are you" in nq:
            report["skipped"].append({"query": q, "reason": "persona_guard"})
            continue
        # autonomy charter: promote budget (personal only)
        try:
            sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
            import cnet_autonomy_charter as ac  # type: ignore

            if not ac.check_action("promote", tick_promotes=n_prom).get("ok"):
                report["skipped"].append({"query": q, "reason": "charter_promote_budget"})
                continue
        except Exception:
            pass

        # Separate REVIEWER role (not teacher). gold_file skips reviewer.
        review_meta = None
        if use_reviewer and reason != "gold_file":
            if roe_reviewer_mod is None:
                report["skipped"].append({"query": q, "reason": "reviewer_module_missing"})
                continue
            review_meta = roe_reviewer_mod.review(q, final)
            append_jsonl(
                PACKS / "review_log.jsonl",
                {
                    "ts": utc_now(),
                    "query": q,
                    "verdict": review_meta.get("verdict"),
                    "approved": review_meta.get("approved"),
                    "reason": review_meta.get("reason"),
                    "model": review_meta.get("model"),
                    "role": "reviewer",
                },
            )
            if not review_meta.get("approved"):
                report["skipped"].append(
                    {
                        "query": q,
                        "reason": "reviewer_reject",
                        "verdict": review_meta.get("verdict"),
                        "detail": (review_meta.get("reason") or "")[:120],
                    }
                )
                continue
            reason = f"{reason}+reviewer"

        pat = pattern_for(q)
        ok_fd = promote_via_front_door(q, final, args.dry_run)
        if not args.dry_run:
            write_skill(PERSONAL, sid, pat, final)
        n_prom += 1
        promoted_ids.add(sid)
        row = {
            "query": q,
            "skill_id": sid,
            "pattern": pat,
            "reason": reason,
            "front_door": ok_fd,
            "answer_preview": final[:120],
        }
        if review_meta:
            row["reviewer"] = {
                "verdict": review_meta.get("verdict"),
                "reason": (review_meta.get("reason") or "")[:120],
                "model": review_meta.get("model"),
            }
        report["promoted"].append(row)
        append_jsonl(
            PACKS / "evolve_promotes.jsonl",
            {
                "ts": utc_now(),
                "query": q,
                "skill_id": sid,
                "reason": reason,
                "dry_run": args.dry_run,
                "reviewer": (review_meta or {}).get("verdict"),
            },
        )

    st["promoted_ids"] = sorted(promoted_ids)
    st["last_tick"] = utc_now()
    st["ticks"] = int(st.get("ticks") or 0) + 1
    if not args.dry_run:
        save_state(st)
    REPORT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    print(f"evolve_tick packs={PACKS}")
    print(f"  miss_rows={report['miss_rows']} clusters={report['clusters']}")
    print(f"  promoted={len(report['promoted'])} skipped={len(report['skipped'])}")
    print(f"  reviewer_enabled={report.get('reviewer_enabled')}")
    for p in report["promoted"][:10]:
        print(f"  + {p['skill_id']} reason={p['reason']} q={p['query'][:50]}")
    print(f"  report → {REPORT}")

    # Autonomy spine bridge: structured miss/teach turns → seal-gated tick.
    # Residual freeform never CERT. Best-effort; evolve tick still passes if CLI absent.
    try:
        import subprocess
        auto_bin = ROOT / "bin" / "cnet_autonomy_tick_cli"
        auto_dir = PACKS / "autonomy_tick"
        auto_dir.mkdir(parents=True, exist_ok=True)
        if auto_bin.is_file() and os.access(auto_bin, os.X_OK):
            # drain a few teach-shaped or TAG n lines from miss log if present
            turns = []
            if MISS.is_file():
                for row in load_jsonl(MISS)[-20:]:
                    q = (row.get("query") or "").strip()
                    a = (row.get("answer") or "").strip()
                    if q.startswith("teach ") or (len(q.split()) == 2 and q.split()[1].isdigit()):
                        turns.append(q)
                    elif a.startswith("teach "):
                        turns.append(a)
            if not turns and args.seed_demo:
                turns = ["teach evolve_demo 1 2"]
            for turn in turns[:5]:
                if args.dry_run:
                    report.setdefault("autonomy_ticks", []).append({"turn": turn, "dry": True})
                    continue
                cp = subprocess.run(
                    [str(auto_bin), str(auto_dir), turn],
                    cwd=str(ROOT),
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                report.setdefault("autonomy_ticks", []).append(
                    {
                        "turn": turn,
                        "rc": cp.returncode,
                        "out": (cp.stdout or "")[-200:],
                    }
                )
            if turns:
                print(f"  autonomy_tick turns={len(turns)} workdir={auto_dir}")
    except Exception as ex:
        report.setdefault("autonomy_ticks", []).append({"error": str(ex)[:160]})

    print("ROE_EVOLVE_TICK_PASS")
    if not report["promoted"] and not args.seed_demo:
        print("  (idle — no eligible promotes; drop gold or repeat misses)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
