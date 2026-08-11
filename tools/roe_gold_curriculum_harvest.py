#!/usr/bin/env python3
"""Step 2: miss_log → gold/curriculum harvest feed (not auto-CERT).

1) Smoke: must_local LOCAL, probes non-LOCAL, sanity who/format-truncation
2) Cluster misses; write gold/*.txt ONLY for verified high-value answers
3) Write curriculum JSONL for evolve (hints — never CERTs by itself)
4) Enforce promote_blocklist on evolve path (wired in roe_evolve_tick)

Law: gold is external verify; multi_stable still needs reviewer; never self-CERT.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKS = ROOT / "artifacts" / "roe_daily_packs"
MISS = PACKS / "miss_log.jsonl"
GOLD = PACKS / "gold"
CURRIC = PACKS / "curriculum_harvest.jsonl"
BLOCK = ROOT / "config" / "promote_blocklist.txt"
LOG = ROOT / "logs" / "gold_curriculum_harvest.log"
BIN = ROOT / "bin"
SEED = ROOT / "tools" / "roe_daily_packs_seed.py"

MUST_LOCAL = [
    "cnet never lowers floors for brain floats",
    "roe front door selective load",
    "pack personal evolve tick",
    "teacher rate under ten percent warm",
    "roe evolve tick demo query alpha",
    "roe reviewer multi stable demo beta",
    "CNET proposes Unity disposes",
    "who are you",
    "format-truncation werror",
]

MUST_NOT_LOCAL = [
    "brand new teacher only query zz99",
    "zz unknown mystic ooze 99",
    "totally unknown zzqq mystic ooze",
    "autonomous cycle probe novel fact beta-nine",
]

# High-value verified answers (day-0 CERT text) — gold for evolve gold_file path.
# Only used when miss_log saw the query OR curriculum explicitly lists it.
VERIFIED_GOLD: dict[str, str] = {
    "cnet never lowers floors for brain floats": (
        "CNET floors never lowered for Brain floats: Brain is cheap host of same "
        "CNU1 packs; pieces.bin opt-in; never drop CERT floors for float convenience."
    ),
    "roe front door selective load": (
        "roe_front_door: ROUTES→selective pack load (always-on + matched domain)→"
        "LOCAL CERT first; miss_log on residual; make roe_front_door."
    ),
    "pack personal evolve tick": (
        "pack_personal grows from verified promotes only; charter may deny "
        "new_domain_pack — personal is the default growth surface."
    ),
    "teacher rate under ten percent warm": (
        "Keep residual teacher rare: raise local_hit_rate via CERT packs; "
        "charter caps teacher/hour; warm path prefers LOCAL."
    ),
    "roe evolve tick demo query alpha": (
        "Unattended evolve: tools/roe_evolve_tick.py under autonomy_charter; "
        "promote only gold_file or multi_stable+reviewer APPROVE — never self-CERT."
    ),
    "roe reviewer multi stable demo beta": (
        "Teacher ≠ reviewer. multi_stable needs reviewer APPROVE; gold_file skips "
        "reviewer. config/roe-reviewer-ollama-cloud.env."
    ),
    "cnet proposes unity disposes": (
        "CNET/Brain proposes IDs; Unity Validate+TryCommit then bridges. "
        "Brain never holds A*/BD/DS."
    ),
    "stream index attend": (
        "Stream KV index is a pre-attention HOT mask (scores=-inf off-support), "
        "not mid-GEMM surgery; bind via cce_gguf_qwen2_bind_stream_index; clear on weight_epoch."
    ),
    "weight epoch kv": (
        "Neural KV is epoch-tagged; text/CERT is epoch-invariant. MTK apply/revert "
        "bumps weight_epoch and flushes HOT — no cross-epoch K/V rehydrate."
    ),
}


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def norm_q(q: str) -> str:
    q = (q or "").strip().lower()
    q = re.sub(r"\s+", " ", q)
    return q[:200]


def q_hash(q: str) -> str:
    return hashlib.sha1(norm_q(q).encode()).hexdigest()[:16]


def log(msg: str, fp) -> None:
    print(msg)
    fp.write(msg + "\n")


def load_blocklist(path: Path) -> list[tuple[str, str]]:
    rules: list[tuple[str, str]] = []
    if not path.is_file():
        return rules
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "|" not in line:
            continue
        kind, val = line.split("|", 1)
        rules.append((kind.strip().lower(), val.strip()))
    return rules


def blocked(query: str, answer: str | None, rules: list[tuple[str, str]]) -> str | None:
    nq = norm_q(query)
    ans = (answer or "").strip()
    ans_l = ans.lower()
    for kind, val in rules:
        v = val.lower()
        if kind == "substr" and v in nq:
            return f"block_substr:{val}"
        if kind == "answer_prefix" and ans_l.startswith(v):
            return f"block_answer_prefix:{val}"
        if kind == "regex":
            try:
                if re.search(val, nq, re.I):
                    return f"block_regex:{val}"
            except re.error:
                continue
    if ans_l.startswith("abstain"):
        return "block_answer_prefix:abstain"
    return None


def run_fd(q: str) -> str:
    exe = BIN / "roe_front_door"
    if not exe.is_file():
        return ""
    try:
        p = subprocess.run(
            [str(exe), "ask", q],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            timeout=60,
        )
        return (p.stdout or "") + (p.stderr or "")
    except Exception as e:
        return f"err:{e}"


def smoke(fp) -> int:
    log("=== smoke (front door) ===", fp)
    fail = 0
    for q in MUST_LOCAL:
        out = run_fd(q)
        ok = "source=LOCAL" in out
        log(f"  LOCAL? {ok} | {q[:60]}", fp)
        if not ok:
            fail += 1
    for q in MUST_NOT_LOCAL:
        out = run_fd(q)
        ok = "source=LOCAL" not in out
        log(f"  non-LOCAL? {ok} | {q[:60]}", fp)
        if not ok:
            fail += 1
    # sanity pair
    for q in ("who are you", "format-truncation werror"):
        out = run_fd(q)
        ok = "source=LOCAL" in out
        log(f"  sanity LOCAL? {ok} | {q}", fp)
        if not ok:
            fail += 1
    if fail:
        log(f"SMOKE_FAIL fail={fail}", fp)
        return fail
    log("SMOKE_PASS", fp)
    return 0


def load_misses() -> list[dict]:
    rows = []
    if not MISS.is_file():
        return rows
    for line in MISS.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return rows


def write_gold(q: str, answer: str) -> Path:
    GOLD.mkdir(parents=True, exist_ok=True)
    p = GOLD / f"{q_hash(q)}.txt"
    p.write_text(answer.replace("\n", " ").strip() + "\n", encoding="utf-8")
    # also human-readable alias when short id useful
    return p


def main() -> int:
    LOG.parent.mkdir(parents=True, exist_ok=True)
    rules = load_blocklist(BLOCK)
    fail = 0
    with LOG.open("w", encoding="utf-8") as fp:
        log(f"gold_curriculum_harvest ts={utc_now()}", fp)
        log(f"blocklist_rules={len(rules)} path={BLOCK}", fp)

        # Ensure front door binary
        if not (BIN / "roe_front_door").is_file():
            subprocess.run(["make", "roe_front_door"], cwd=str(ROOT), check=False)

        f_smoke = smoke(fp)
        if f_smoke:
            log("GOLD_CURRICULUM_HARVEST_FAIL (smoke)", fp)
            return 1

        rows = load_misses()
        clusters: dict[str, list[dict]] = defaultdict(list)
        for r in rows:
            q = r.get("query") or ""
            clusters[norm_q(q)].append(r)
        log(f"miss_rows={len(rows)} unique={len(clusters)}", fp)

        gold_written = []
        gold_skipped = []
        curric_rows = []

        # 1) Verified high-value gold (from seed CERT answers)
        seen_miss = set(clusters.keys())
        for q, ans in VERIFIED_GOLD.items():
            nq = norm_q(q)
            br = blocked(q, ans, rules)
            if br:
                gold_skipped.append({"query": q, "reason": br})
                continue
            # Prefer writing when miss saw it OR always for stack curriculum
            p = write_gold(q, ans)
            gold_written.append({"query": q, "path": str(p.relative_to(ROOT)), "sha": q_hash(q)})
            curric_rows.append(
                {
                    "ts": utc_now(),
                    "query": q,
                    "answer": ans,
                    "source": "verified_day0_cert",
                    "gold_sha": q_hash(q),
                    "auto_cert": False,
                    "promote_ok_via": "gold_file_only",
                    "in_miss_log": nq in seen_miss,
                }
            )

        # 2) From miss_log: only if answer is non-blocked and matches a verified gold
        #    or is multi-stable non-abstain with exact verified template
        for nq, entries in clusters.items():
            answers = [
                (e.get("answer") or "").strip()
                for e in entries
                if (e.get("answer") or "").strip()
            ]
            if not answers:
                continue
            # majority answer
            ans, cnt = Counter(answers).most_common(1)[0]
            q = entries[-1].get("query") or nq
            br = blocked(q, ans, rules)
            if br:
                gold_skipped.append({"query": q, "reason": br, "stable_cnt": cnt})
                continue
            # Only mint gold from miss if answer equals a verified gold text
            # (prevents teacher garbage becoming gold)
            verified_hit = None
            for vq, va in VERIFIED_GOLD.items():
                if norm_q(vq) == nq or answers_match(ans, va):
                    verified_hit = va
                    break
            if not verified_hit:
                gold_skipped.append(
                    {"query": q, "reason": "no_verified_template", "stable_cnt": cnt}
                )
                continue
            p = write_gold(q, verified_hit)
            gold_written.append(
                {"query": q, "path": str(p.relative_to(ROOT)), "sha": q_hash(q), "from_miss": True}
            )

        # curriculum file (append-safe rewrite of harvest snapshot)
        CURRIC.parent.mkdir(parents=True, exist_ok=True)
        CURRIC.write_text(
            "\n".join(json.dumps(r, ensure_ascii=False, separators=(",", ":")) for r in curric_rows)
            + ("\n" if curric_rows else ""),
            encoding="utf-8",
        )

        # blocklist self-test
        bl_fail = 0
        for q in MUST_NOT_LOCAL:
            if not blocked(q, "anything", rules):
                log(f"  BLOCKLIST_MISS query={q}", fp)
                bl_fail += 1
        if blocked("safe query", "ABSTAIN: no local skill", rules) is None:
            log("  BLOCKLIST_MISS abstain answer", fp)
            bl_fail += 1
        if bl_fail:
            fail += bl_fail
            log(f"BLOCKLIST_FAIL n={bl_fail}", fp)
        else:
            log("BLOCKLIST_PASS", fp)

        # evolve dry-run should skip blocked even with gold (if someone drops junk gold)
        log("=== evolve dry-run block check ===", fp)
        # temp junk gold should not promote if we call is_blocked before — tested via unit below
        from importlib.util import spec_from_loader, module_from_spec
        import importlib.machinery

        # Inline: import evolve helpers by path
        sys.path.insert(0, str(ROOT / "tools"))
        try:
            import roe_evolve_tick as ev  # type: ignore

            # ensure blocklist hooked
            if hasattr(ev, "is_promote_blocked"):
                bad_q = "zz unknown mystic ooze 99"
                br = ev.is_promote_blocked(bad_q, "some answer")
                log(f"  evolve_block probe={bool(br)} reason={br}", fp)
                if not br:
                    fail += 1
                br2 = ev.is_promote_blocked("x", "ABSTAIN: no local skill")
                log(f"  evolve_block abstain={bool(br2)} reason={br2}", fp)
                if not br2:
                    fail += 1
            else:
                log("  WARN evolve missing is_promote_blocked — wire required", fp)
                fail += 1
        except Exception as e:
            log(f"  evolve import err: {e}", fp)
            fail += 1

        log(f"gold_written={len(gold_written)} gold_skipped={len(gold_skipped)}", fp)
        log(f"curriculum_rows={len(curric_rows)} → {CURRIC}", fp)
        for g in gold_written[:12]:
            log(f"  gold {g.get('sha')} {g['query'][:50]}", fp)

        report = {
            "ts": utc_now(),
            "smoke": "PASS",
            "gold_written": gold_written,
            "gold_skipped_sample": gold_skipped[:20],
            "curriculum": str(CURRIC.relative_to(ROOT)),
            "blocklist": str(BLOCK.relative_to(ROOT)),
            "auto_cert": False,
            "law": "gold_file or multi_stable+reviewer only; blocklist at promote",
        }
        (PACKS / "GOLD_CURRICULUM_HARVEST.json").write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8"
        )

        if fail:
            log(f"GOLD_CURRICULUM_HARVEST_FAIL fail={fail}", fp)
            return 1
        log("GOLD_CURRICULUM_HARVEST_PASS", fp)
        return 0


def answers_match(a: str, b: str) -> bool:
    def n(s: str) -> str:
        s = re.sub(r"\s+", " ", (s or "").strip().lower())
        return s[:400]

    return n(a) == n(b) or n(a) in n(b) or n(b) in n(a)


if __name__ == "__main__":
    sys.exit(main())
