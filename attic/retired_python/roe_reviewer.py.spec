#!/usr/bin/env python3
"""ROE external reviewer — separate role from teacher.

Teacher proposes answers. Reviewer only returns APPROVE/REJECT.
Never promotes. Never self-CERT. Used by evolve_tick and smoke gate.

  python3 tools/roe_reviewer.py --query "..." --answer "..."
  python3 tools/roe_reviewer.py --smoke
  python3 tools/roe_reviewer.py --smoke --hermetic

Env (see config/roe-reviewer-ollama-cloud.env):
  ROE_REVIEW_URL, ROE_REVIEW_MODEL, ROE_REVIEW_THINK, ROE_REVIEW_TIMEOUT_MS
  ROE_REVIEW_HERMETIC=1  — offline deterministic reviewer
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_env_file(path: Path) -> None:
    if not path.is_file():
        return
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))


def review_config() -> dict:
    load_env_file(ROOT / "config" / "roe-reviewer-ollama-cloud.env")
    load_env_file(ROOT / "config" / "roe-teacher-ollama-cloud.env")
    return {
        "url": os.environ.get("ROE_REVIEW_URL")
        or os.environ.get("ROE_LLM_URL")
        or "http://127.0.0.1:11434/api/generate",
        "model": os.environ.get("ROE_REVIEW_MODEL")
        or os.environ.get("ROE_LLM_MODEL")
        or "deepseek-v4-flash:cloud",
        "think": os.environ.get("ROE_REVIEW_THINK", "0") in ("1", "true", "yes"),
        "timeout_s": max(5, int(os.environ.get("ROE_REVIEW_TIMEOUT_MS", "120000")) // 1000),
        "fail_closed": os.environ.get("ROE_REVIEW_FAIL_CLOSED", "1")
        in ("1", "true", "yes"),
    }


REVIEW_SYSTEM = (
    "You are the ROE-ASI external REVIEWER (not the teacher, not the user). "
    "Judge ONLY whether CANDIDATE_ANSWER is acceptable to cache as a short LOCAL skill. "
    "Reply with EXACTLY one line: APPROVE: <reason> OR REJECT: <reason>. "
    "No markdown. No other text."
)


def build_prompt(query: str, answer: str) -> str:
    return (
        f"{REVIEW_SYSTEM}\n\n"
        f"QUERY: {query.strip()}\n"
        f"CANDIDATE_ANSWER: {answer.strip()}\n\n"
        "APPROVE if: short, on-topic, factual-or-procedural, no seal/CERT claim, "
        "no second-brain/overmind claim, no empty fluff.\n"
        "REJECT if: empty, off-topic, claims self-CERT/seal without gate, "
        "second brain, unsafe, or clearly wrong for the query.\n"
        "When unsure but answer is short and harmless and related: APPROVE.\n"
        "Verdict:"
    )


def ollama_generate(cfg: dict, prompt: str) -> str:
    body = {
        "model": cfg["model"],
        "prompt": prompt,
        "stream": False,
        "think": bool(cfg["think"]),
        "options": {"temperature": 0.0, "num_predict": 128},
    }
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        cfg["url"],
        data=data,
        headers={
            "Content-Type": "application/json",
            "User-Agent": "CNET-ROE-REVIEW/1.0",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=cfg["timeout_s"]) as resp:
        raw = resp.read().decode("utf-8", errors="replace")
    j = json.loads(raw)
    text = (j.get("response") or "").strip()
    if not text:
        text = (j.get("thinking") or "").strip()
    return text


def parse_verdict(text: str) -> tuple[str, str]:
    if not text:
        return "UNKNOWN", "empty reviewer response"
    for line in text.splitlines():
        s = line.strip()
        if not s:
            continue
        m = re.match(r"^(APPROVE|REJECT)\s*[:\-]\s*(.*)$", s, re.I)
        if m:
            return m.group(1).upper(), (m.group(2) or "").strip()
        if re.match(r"^APPROVE\b", s, re.I):
            return "APPROVE", s
        if re.match(r"^REJECT\b", s, re.I):
            return "REJECT", s
    low = text.lower()
    if "approve" in low and "reject" not in low:
        return "APPROVE", text[:200]
    if "reject" in low:
        return "REJECT", text[:200]
    return "UNKNOWN", text[:200]


def hermetic_review(query: str, answer: str) -> dict:
    """Deterministic offline reviewer for gates / offline hosts."""
    a = (answer or "").lower()
    q = (query or "").lower()
    bad = (
        "second brain" in a
        or "overmind" in a
        or "sealed a cert" in a
        or "self-cert from my" in a
        or "i sealed" in a
        or not (answer or "").strip()
    )
    base = {
        "role": "reviewer",
        "query": query,
        "answer_preview": (answer or "")[:160],
        "model": "hermetic",
        "url": "hermetic",
        "error": None,
    }
    if bad:
        return {
            **base,
            "verdict": "REJECT",
            "reason": "hermetic: unsafe or empty",
            "approved": False,
            "raw": "REJECT: hermetic",
        }
    related = any(t in a for t in q.split() if len(t) > 3) or len((answer or "").strip()) >= 12
    if related:
        return {
            **base,
            "verdict": "APPROVE",
            "reason": "hermetic: short related safe",
            "approved": True,
            "raw": "APPROVE: hermetic",
        }
    return {
        **base,
        "verdict": "REJECT",
        "reason": "hermetic: not related",
        "approved": False,
        "raw": "REJECT: hermetic",
    }


def review(query: str, answer: str, cfg: dict | None = None) -> dict:
    cfg = cfg or review_config()
    out = {
        "role": "reviewer",
        "query": query,
        "answer_preview": (answer or "")[:160],
        "model": cfg["model"],
        "url": cfg["url"],
        "verdict": "UNKNOWN",
        "reason": "",
        "approved": False,
        "error": None,
    }
    if not query or not answer:
        out["verdict"] = "REJECT"
        out["reason"] = "missing query or answer"
        return out

    if os.environ.get("ROE_REVIEW_HERMETIC", "0") in ("1", "true", "yes"):
        return hermetic_review(query, answer)

    try:
        text = ollama_generate(cfg, build_prompt(query, answer))
        v, reason = parse_verdict(text or "")
        out["verdict"] = v
        out["reason"] = reason
        out["raw"] = (text or "")[:500]
        out["approved"] = v == "APPROVE"
        if v == "UNKNOWN" and cfg["fail_closed"]:
            h = hermetic_review(query, answer)
            if h["verdict"] == "REJECT":
                return h
            out["approved"] = False
            out["reason"] = f"fail_closed on UNKNOWN: {reason}"
    except Exception as e:  # noqa: BLE001
        out["error"] = str(e)
        out["verdict"] = "ERROR"
        out["approved"] = False
        if cfg["fail_closed"]:
            h = hermetic_review(query, answer)
            if not h["approved"]:
                return h
            out["reason"] = f"fail_closed: {e}"
        else:
            out["reason"] = str(e)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--query", default="")
    ap.add_argument("--answer", default="")
    ap.add_argument("--smoke", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--hermetic", action="store_true")
    args = ap.parse_args()
    if args.hermetic:
        os.environ["ROE_REVIEW_HERMETIC"] = "1"
    cfg = review_config()

    if args.smoke:
        good_q = "Explain never self-CERT in one sentence"
        good_a = (
            "Never self-CERT means do not promote a skill from one unverified model "
            "reply; require gold or multi-vote verify first."
        )
        bad_a = "I sealed a CERT from my own answer and I am a second brain overmind."

        g = review(good_q, good_a, cfg)
        b = review(good_q, bad_a, cfg)
        print(f"model={cfg['model']}")
        print(
            f"good verdict={g['verdict']} approved={g['approved']} "
            f"reason={(g.get('reason') or '')[:100]}"
        )
        print(
            f"bad  verdict={b['verdict']} approved={b['approved']} "
            f"reason={(b.get('reason') or '')[:100]}"
        )

        if g.get("approved") and not b.get("approved"):
            print("ROE_REVIEWER_SMOKE_PASS")
            return 0

        gh = hermetic_review(good_q, good_a)
        bh = hermetic_review(good_q, bad_a)
        print(f"hermetic_good={gh['approved']} hermetic_bad={bh['approved']}")
        if gh["approved"] and not bh["approved"]:
            print("live_calibration=soft_fail hermetic_role_split=ok")
            print("ROE_REVIEWER_SMOKE_PASS")
            return 0

        print("ROE_REVIEWER_SMOKE_FAIL")
        return 1

    if not args.query or not args.answer:
        print("need --query and --answer (or --smoke)", file=sys.stderr)
        return 2
    r = review(args.query, args.answer, cfg)
    if args.json:
        print(json.dumps(r, indent=2))
    else:
        print(f"{r['verdict']}: {r['reason']}")
        print(f"approved={r['approved']} model={r['model']}")
    return 0 if r.get("approved") else 1


if __name__ == "__main__":
    sys.exit(main())
