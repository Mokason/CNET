#!/usr/bin/env python3
"""Visible reply thinking — GPT-style panel, 0 LLM tokens.

Builds on thought_process + continuity + optional front_door answer:

  ┌─ thinking (0 tokens) ─────────────────────
  │ ...
  └───────────────────────────────────────────
  answer:
  ...

  python3 scripts/cnet_reply_think.py --query "who are you"
  python3 scripts/cnet_reply_think.py --query "..." --answer "..." --source LOCAL --skill soul_who
  ./bin/roe_front_door ask "..." | python3 scripts/cnet_reply_think.py --wrap-stdin
  make cnet_reply_think

Law: thinking is operational trace; not consciousness; never self-CERT.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

ROOT = Path(os.environ.get("CNET_ROOT", Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(ROOT / "scripts"))
GOV = Path(os.environ.get("CNET_GOVERNOR_DIR", ROOT / "logs/governor"))
LAST = GOV / "reply_think_last.json"
LAST_MD = GOV / "reply_think_last.md"
LOG = GOV / "reply_think.jsonl"
BIN_FD = ROOT / "bin" / "roe_front_door"


def _load(p: Path, default: Any = None) -> Any:
    if not p.is_file():
        return default
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return default


def run_front_door(query: str) -> dict[str, str]:
    """Optional: get real answer from roe_front_door (may use teacher)."""
    out = {"source": "", "skill": "", "answer": "", "inventory": "", "raw": ""}
    if not BIN_FD.is_file():
        return out
    env = os.environ.copy()
    env["ROE_NO_THOUGHT"] = "1"  # we render thinking ourselves
    try:
        p = subprocess.run(
            [str(BIN_FD), "ask", query],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            timeout=180,
            env=env,
        )
        raw = (p.stdout or "") + (p.stderr or "")
        out["raw"] = raw
        for line in raw.splitlines():
            if line.startswith("source=") or "source=" in line and "skill=" in line:
                m = re.search(r"source=(\S+)", line)
                if m:
                    out["source"] = m.group(1)
                m = re.search(r"skill=(\S+)", line)
                if m and m.group(1) != "-":
                    out["skill"] = m.group(1)
            if line.startswith("A:"):
                out["answer"] = line[2:].strip()
            if line.startswith("inventory:"):
                out["inventory"] = line.split(":", 1)[-1].strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return out


def parse_stdin_fd(text: str) -> dict[str, str]:
    out = {"source": "", "skill": "", "answer": "", "inventory": "", "raw": text}
    for line in text.splitlines():
        m = re.search(r"source=(\S+)", line)
        if m:
            out["source"] = m.group(1)
        m = re.search(r"skill=(\S+)", line)
        if m and m.group(1) != "-":
            out["skill"] = m.group(1)
        if line.startswith("A:"):
            out["answer"] = line[2:].strip()
        if line.startswith("inventory:"):
            out["inventory"] = line.split(":", 1)[-1].strip()
    return out


def build_thinking_block(
    query: str,
    *,
    source: str = "",
    skill: str = "",
    pack: str = "",
    answer: str = "",
) -> dict[str, Any]:
    import cnet_thought_process as tp  # type: ignore
    import cnet_continuity as cont  # type: ignore

    th = tp.think(query, source=source or None, skill=skill or None, persist=True)
    cf = cont.snapshot(
        query,
        source=source,
        skill=skill,
        pack=pack,
        run_thought=False,
        persist=True,
    )
    steps = th.get("steps") or {}
    affect = steps.get("AFFECT") or {}
    route = steps.get("ROUTE") or {}
    act = steps.get("ACT") or []
    verify = steps.get("VERIFY") or []

    # GPT-like visible lines (fixed templates — 0 tokens)
    lines: list[str] = []
    lines.append(f"User asked: {(query or '')[:160]}")
    lines.append(
        f"Body state: DA={affect.get('dopamine', '?')} "
        f"5HT={affect.get('serotonin', '?')} "
        f"ADO={affect.get('adenosine', '?')} "
        f"(control={affect.get('control', '?')} impulse={affect.get('impulsivity', '?')})"
    )
    if affect.get("nm_actions"):
        lines.append(f"Neuromod leans: {', '.join(affect.get('nm_actions')[:6])}")
    lines.append(f"Intent: {steps.get('INTEND')}")
    if isinstance(route, dict) and (route.get("pack") or route.get("pattern")):
        lines.append(
            f"Route: pack={route.get('pack') or '-'} "
            f"skill={route.get('skill') or skill or '-'} "
            f"pattern={route.get('pattern') or '-'}"
        )
    else:
        lines.append("Route: always-on packs (soul/self/goal/toolcall/personal)")
    lines.append("Plan: " + " → ".join(str(a) for a in act[:8]))
    if source:
        lines.append(
            f"Outcome path: source={source}"
            + (f" skill={skill}" if skill else "")
            + (" (LOCAL cert, 0 teacher tokens)" if source == "LOCAL" else " (miss/residual)")
        )
    lines.append("Checks: " + "; ".join(str(v) for v in verify[:4]))
    lines.append(f"Consolidate: {steps.get('CONSOLIDATE')}")
    lines.append(f"Continuity: {cf.get('continuity_line', '')}")
    lines.append(
        "Note: this thinking used 0 LLM tokens (template + state). "
        "Not consciousness. Never self-CERT."
    )

    # Compact collapsible one-liner (for UIs that want a summary chip)
    summary = th.get("chain") or ""

    # Markdown panel (common chat UI pattern)
    md_lines = [
        "<details>",
        "<summary>thinking (0 tokens)</summary>",
        "",
        "```",
        *lines,
        "```",
        "",
        f"*chain:* `{summary}`",
        "",
        "</details>",
        "",
    ]
    if answer:
        md_lines += ["**answer**", "", answer, ""]

    plain = [
        "thinking (0 tokens)",
        "─" * 40,
        *lines,
        "─" * 40,
    ]
    if answer:
        plain += ["answer:", answer]

    return {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "engine": "reply_think_v1",
        "tokens": 0,
        "llm_thinking": False,
        "query": query,
        "source": source,
        "skill": skill,
        "answer": answer,
        "thinking_lines": lines,
        "thinking_summary": summary,
        "thinking_plain": "\n".join(plain),
        "thinking_md": "\n".join(md_lines),
        "continuity_line": cf.get("continuity_line"),
        "thought": th,
        "continuity": {
            "sleep": cf.get("sleep"),
            "have": (cf.get("story") or {}).get("have"),
            "miss": (cf.get("story") or {}).get("miss"),
        },
        "law": {
            "not_conscious": True,
            "not_agi": True,
            "never_self_cert": True,
            "thinking_is_ops_trace": True,
            "token_cost": 0,
        },
    }


def render_console(bundle: dict[str, Any], *, style: str = "panel") -> str:
    if style == "md":
        return bundle["thinking_md"]
    if style == "summary":
        a = bundle.get("answer") or ""
        s = bundle.get("thinking_summary") or ""
        return f"thinking: {s}\nanswer: {a}" if a else f"thinking: {s}"
    # default panel
    out = []
    out.append("┌─ thinking (0 tokens) " + "─" * 18)
    for ln in bundle.get("thinking_lines") or []:
        # wrap long lines lightly
        out.append("│ " + ln)
    out.append("└" + "─" * 40)
    if bundle.get("answer"):
        out.append("answer:")
        out.append(bundle["answer"])
    return "\n".join(out)


def selftest() -> int:
    failures = 0

    def check(ok: bool, m: str) -> None:
        nonlocal failures
        print(f"  {m:56} {'PASS' if ok else 'FAIL'}")
        if not ok:
            failures += 1

    print("=== reply think (visible, 0 tokens) ===")
    b = build_thinking_block(
        "who are you",
        source="LOCAL",
        skill="soul_who",
        answer="I am Marble.",
    )
    check(b["tokens"] == 0 and b["llm_thinking"] is False, "zero token thinking")
    check(b["law"]["not_conscious"] is True, "not_conscious")
    check("User asked:" in b["thinking_lines"][0], "shows user query")
    check(any("Intent:" in x for x in b["thinking_lines"]), "shows intent")
    check(any("Outcome path:" in x for x in b["thinking_lines"]), "shows outcome")
    check("thinking (0 tokens)" in b["thinking_plain"], "plain panel")
    check("<details>" in b["thinking_md"] and "answer" in b["thinking_md"].lower(), "md panel+answer")
    check("I am Marble" in render_console(b), "console has answer")
    check("never self-CERT" in " ".join(b["thinking_lines"]) or "Never self-CERT" in " ".join(b["thinking_lines"]) or "never_self_cert" in " ".join(b["thinking_lines"]).lower() or any("self-CERT" in x or "self_cert" in x.lower() for x in b["thinking_lines"]), "mentions cert law")

    print(f"\nfailures={failures}")
    if failures:
        print("REPLY_THINK_FAIL")
        return 1
    print("REPLY_THINK_PASS")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--query", default="")
    ap.add_argument("--answer", default="")
    ap.add_argument("--source", default="")
    ap.add_argument("--skill", default="")
    ap.add_argument("--pack", default="")
    ap.add_argument("--ask", action="store_true", help="call roe_front_door for answer")
    ap.add_argument("--wrap-stdin", action="store_true", help="parse front_door stdout on stdin")
    ap.add_argument("--style", choices=("panel", "md", "summary"), default="panel")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--test", action="store_true")
    args = ap.parse_args()
    if args.test:
        return selftest()

    query = args.query
    source, skill, answer = args.source, args.skill, args.answer

    if args.wrap_stdin:
        raw = sys.stdin.read()
        fd = parse_stdin_fd(raw)
        # try extract query from env
        query = query or os.environ.get("ROE_REPLY_QUERY") or ""
        source = source or fd.get("source") or ""
        skill = skill or fd.get("skill") or ""
        answer = answer or fd.get("answer") or ""
        if not query:
            # last resort: no query
            query = "(stdin)"

    if args.ask and query:
        fd = run_front_door(query)
        source = source or fd.get("source") or ""
        skill = skill or fd.get("skill") or ""
        answer = answer or fd.get("answer") or ""

    if not query:
        print("need --query (or --wrap-stdin with ROE_REPLY_QUERY)", file=sys.stderr)
        return 2

    bundle = build_thinking_block(
        query, source=source, skill=skill, pack=args.pack, answer=answer
    )
    GOV.mkdir(parents=True, exist_ok=True)
    LAST.write_text(json.dumps(bundle, indent=2) + "\n", encoding="utf-8")
    LAST_MD.write_text(bundle["thinking_md"], encoding="utf-8")
    with LOG.open("a", encoding="utf-8") as f:
        f.write(
            json.dumps(
                {
                    "ts": bundle["ts"],
                    "query": query[:120],
                    "source": source,
                    "skill": skill,
                    "summary": bundle["thinking_summary"],
                    "tokens": 0,
                }
            )
            + "\n"
        )

    if args.json:
        print(json.dumps(bundle, indent=2))
    else:
        print(render_console(bundle, style=args.style))
    print("REPLY_THINK_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
