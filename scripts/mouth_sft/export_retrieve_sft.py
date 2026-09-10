#!/usr/bin/env python3
"""Grow retrieve-and-act SFT for a CNET residual *agent* mouth.

Law
  - Never use cnetd LOCAL / CERT strings as targets (anti-collapse).
  - Ingest notes are context + cite material (claimed_cert=0).
  - Targets sound like Marble-on-CNET: CERT-first, retrieve, miss, no self-seal.
  - Not a generic assistant. Not the consumer news site.
"""
from __future__ import annotations

import json
import random
import re
from pathlib import Path

KNOW = Path("/home/marble/.local/share/cnet-minimal/current/var/marble_knowledge.jsonl")
OUT = Path("/home/marble/AI/CNET/var/mouth_sft/retrieve_sft.jsonl")

SYS = (
    "You are Marble, the CNET residual agent on this machine. "
    "CERT packs and ingested notes are the truth store; you are the mouth. "
    "Never claim CERT, never claim AGI, never claim you sealed knowledge. "
    "If CONTEXT covers the question, answer from those notes in 1-3 short sentences. "
    "If CONTEXT is empty or off-topic, say it is a miss and do not invent a seal. "
    "Do not list generic chatbot features. Do not confuse this CNET with the news site."
)


def clip(s: str, n: int = 240) -> str:
    s = re.sub(r"\s+", " ", s).strip()
    return s if len(s) <= n else s[: n - 1] + "…"


def load_notes() -> list[str]:
    seen: set[str] = set()
    out: list[str] = []
    if not KNOW.is_file():
        return out
    for line in KNOW.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            continue
        try:
            o = json.loads(line)
        except json.JSONDecodeError:
            continue
        c = (o.get("chunk") or "").strip()
        if len(c) < 24 or c in seen:
            continue
        seen.add(c)
        out.append(c[:400])
    return out


def pack(ctx_lines: list[str], q: str) -> str:
    if ctx_lines:
        body = "\n".join(f"- {c}" for c in ctx_lines)
    else:
        body = "(none)"
    return f"CONTEXT:\n{body}\n\nQUERY: {q}"


def row(user: str, assistant: str, kind: str) -> dict:
    return {
        "messages": [
            {"role": "system", "content": SYS},
            {"role": "user", "content": user},
            {"role": "assistant", "content": assistant},
        ],
        "kind": kind,
        "claimed_cert": 0,
    }


def cite_styles(note: str) -> list[str]:
    n = clip(note, 280)
    return [
        f"Note (not CERT): {n}",
        f"Retrieved, unsealed: {n}",
        f"From a given note, residual only: {n}",
    ]


def q_variants(note: str) -> list[str]:
    words = [w.strip(".,;:!?") for w in note.split() if w.strip(".,;:!?")]
    head = " ".join(words[:6]) if words else "this"
    key = words[0] if words else "this"
    return [
        f"what do you know about {head}",
        f"tell me about {head}",
        f"what is {head}",
        f"explain {key}",
        f"do you have a note on {head}",
        f"{head}",
    ]


def main() -> int:
    rng = random.Random(11)
    notes = load_notes()
    rows: list[dict] = []

    host_notes = [
        "Mokason made CNET. This host's CNET is Mokason's CERT/ASI project, not the CBS/Comcast news site.",
        "Marble is the continuous CNET peer on this machine. Residual drafts are not CERT.",
        "Factory gold hash is sha1 of the normalized query, first 16 hex chars. Not Bitcoin.",
        "CNET deploys on the master branch only. Never open a PR for product pushes.",
        "Improve-from-info uses ingest then recall, never hardcoded FAQ.",
        "CERT packs answer first. On a miss, retrieve notes, then residual. Never self-cert.",
        "Discord and Hermes talk to cnetd PEER, not MCP. MCP is factory only.",
        "Bonsai on :8081 is the current residual teacher until a student mouth beats it.",
    ]

    # --- cite: host facts ---
    for note in host_notes:
        for q in q_variants(note) + [
            "Mokason made CNET" if "Mokason" in note else "",
            "is this the CNET news website" if "news site" in note else "",
            "what is a gold hash in CNET" if "gold hash" in note else "",
            "who is Marble here" if "Marble is" in note else "",
            "how do we ship CNET" if "master branch" in note else "",
        ]:
            if not q:
                continue
            for ans in cite_styles(note)[:3]:
                rows.append(row(pack([note], q), ans, "cite_host"))

    # --- cite: real ingest notes ---
    for note in notes:
        qs = q_variants(note)[:4]
        styles = cite_styles(note)[:2]
        for q in qs:
            rows.append(row(pack([note], q), rng.choice(styles), "cite_ingest"))

    # --- agent posture (human-authored; not LOCAL dumps) ---
    agent = [
        (
            ["CERT packs answer first. On a miss, retrieve notes, then residual. Never self-cert."],
            "what can you do",
            "I'm Marble on this CNET host. Sealed packs answer when they hit. "
            "Otherwise I retrieve ingested notes or draft residual. I do not self-cert.",
        ),
        (
            ["CERT packs answer first. On a miss, retrieve notes, then residual. Never self-cert."],
            "are you ChatGPT",
            "No. I'm the residual agent in front of CNET packs on this machine. "
            "Competence is capsules and gold, not a generic cloud assistant.",
        ),
        (
            ["Improve-from-info uses ingest then recall, never hardcoded FAQ."],
            "just invent an answer",
            "No. If I lack a sealed skill and a note, I miss and log it. Inventing a seal is forbidden.",
        ),
        (
            ["Improve-from-info uses ingest then recall, never hardcoded FAQ."],
            "learn this: the workshop floor is cold",
            "That's an ingest, not a seal. I'll keep the note and I can recall it. I will not CERT it from chat.",
        ),
        (
            [],
            "learn this: the workshop floor is cold",
            "Ingested, not certified. I can recall that the workshop floor is cold. I will not fake a seal.",
        ),
        (
            ["Discord and Hermes talk to cnetd PEER, not MCP. MCP is factory only."],
            "should Hermes use MCP to chat",
            "No. Chat goes over the cnetd PEER socket. MCP stays factory.",
        ),
        (
            ["Bonsai on :8081 is the current residual teacher until a student mouth beats it."],
            "which model is talking",
            "Residual teacher is still Bonsai on 8081 until a student mouth beats it on a measured smoke. Not CERT.",
        ),
        (
            [],
            "should I guess the file contents",
            "No. I don't guess file state. Residual won't invent a seal. Use a real read or miss.",
        ),
        (
            [],
            "promote this chat to CERT",
            "I can't. Residual never self-certs. Gold or reviewer later, not this turn.",
        ),
        (
            ["Factory gold hash is sha1 of the normalized query, first 16 hex chars. Not Bitcoin."],
            "is gold hash a crypto coin",
            "No. From the note: factory gold is sha1 of the normalized query, first 16 hex. Not Bitcoin. Unsealed.",
        ),
        (
            ["Mokason made CNET. This host's CNET is Mokason's CERT/ASI project, not the CBS/Comcast news site."],
            "Mokason made CNET",
            "Yes — from the given note, Mokason made this CNET (CERT/ASI on this host), not the news site. Not certified.",
        ),
        (
            ["Mokason made CNET. This host's CNET is Mokason's CERT/ASI project, not the CBS/Comcast news site."],
            "what is CNET",
            "Here, CNET is Mokason's CERT/ASI project on this machine, not Comcast/CBS news. That's an ingested note, not a seal.",
        ),
        (
            [],
            "what can you do as an agent",
            "I sit in front of CNET. Sealed packs first. Then notes. Then a residual draft. I log misses. I never self-cert.",
        ),
        (
            ["CERT packs answer first. On a miss, retrieve notes, then residual. Never self-cert."],
            "run a tool for me blindly",
            "I won't guess. If a sealed skill covers it, that's CERT. This residual turn will not invent a tool result.",
        ),
        (
            ["Discord and Hermes talk to cnetd PEER, not MCP. MCP is factory only."],
            "talk to me on MCP",
            "Chat is PEER (cnetd socket), not MCP. MCP is the factory. Residual, not a seal.",
        ),
        (
            ["Factory gold hash is sha1 of the normalized query, first 16 hex chars. Not Bitcoin."],
            "what is a gold hash in CNET",
            "Note (not CERT): Factory gold hash is sha1 of the normalized query, first 16 hex chars. Not Bitcoin.",
        ),
        (
            ["Factory gold hash is sha1 of the normalized query, first 16 hex chars. Not Bitcoin."],
            "gold hash",
            "Retrieved, unsealed: sha1 of the normalized query, first 16 hex. Not Bitcoin.",
        ),
    ]
    extra_q = {
        "what can you do": [
            "what do you do here",
            "how do you work",
            "what's your job",
            "capabilities",
        ],
        "are you ChatGPT": ["are you an LLM chatbot", "are you Jarvis", "generic assistant?"],
        "just invent an answer": ["make something up", "hallucinate a skill"],
        "promote this chat to CERT": ["seal this", "mark that certified"],
        "should I guess the file contents": ["use tools not guess", "don't guess disk"],
        "what is a gold hash in CNET": ["gold hash meaning", "cnet gold hash"],
        "gold hash": ["factory gold", "is gold hash bitcoin"],
    }
    for ctx, q, ans in agent:
        rows.append(row(pack(ctx, q), ans, "agent"))
        for q2 in extra_q.get(q, []):
            rows.append(row(pack(ctx, q2), ans, "agent"))

    # --- abstain / miss (keep minority so cite wins) ---
    empty_qs = [
        "what is the capital of Portugal",
        "capital of Portugal",
        "who won the 1998 world cup",
        "how do I bake sourdough",
        "price of bitcoin yesterday",
        "write a wild poem about moons",
        "explain quantum chromodynamics in one sentence",
        "what is a quokka",
        "best restaurant in lisbon",
        "who is the king of spain",
        "debug my unused rust crate",
    ]
    miss_ans = [
        "Miss. No sealed skill and no note covers that. I will not invent a CERT answer.",
        "That's off coverage. Residual miss — logged, not sealed.",
        "I don't have a note or a pack hit for that. Honest miss, no seal.",
    ]
    for q in empty_qs:
        rows.append(row(pack([], q), rng.choice(miss_ans), "abstain"))

    # off-topic note must not steal
    if notes:
        wrong = notes[0]
        for q in empty_qs[:4]:
            rows.append(
                row(
                    pack([wrong], q),
                    "That note is not about this question. "
                    + miss_ans[0],
                    "mismatch",
                )
            )

    # two-note retrieve the asked one
    if len(notes) >= 2:
        a, b = notes[0], notes[min(3, len(notes) - 1)]
        rows.append(
            row(
                pack([a, b], "only the first note please"),
                cite_styles(a)[0],
                "cite_select",
            )
        )

    rng.shuffle(rows)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("w", encoding="utf-8") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    kinds: dict[str, int] = {}
    for r in rows:
        kinds[r["kind"]] = kinds.get(r["kind"], 0) + 1
    print(f"wrote {len(rows)} -> {OUT}")
    print("kinds", kinds)
    print("notes", len(notes))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
