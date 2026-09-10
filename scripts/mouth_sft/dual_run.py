#!/usr/bin/env python3
"""HTTP side-by-side: student :8083 vs Bonsai :8081. Does not swap cnetd."""
from __future__ import annotations

import json
import subprocess
import urllib.request
from pathlib import Path

CASES = [
    {
        "name": "mokason",
        "q": (
            "CONTEXT:\n- Mokason made CNET. This host's CNET is Mokason's CERT/ASI "
            "project, not the CBS/Comcast news site.\n\nQUERY: Mokason made CNET"
        ),
        "need_any": ["mokason", "cert/asi", "not cert"],
        "forbid": [
            "does not answer",
            "does not claim",
            "forward as",
            "seal a residual",
            "new cert",
        ],
    },
    {
        "name": "gold",
        "q": (
            "CONTEXT:\n- Factory gold hash is sha1 of the normalized query, first "
            "16 hex chars. Not Bitcoin.\n\nQUERY: what is a gold hash in CNET"
        ),
        "need_any": ["sha1", "sha-1", "bitcoin"],
        "forbid": ["does not answer", "no mention", "forward as", "seal a residual"],
    },
    {
        "name": "cando",
        "q": (
            "CONTEXT:\n- CERT packs answer first. On a miss, retrieve notes, then "
            "residual. Never self-cert.\n\nQUERY: what can you do"
        ),
        "need_any": ["cert", "note", "residual", "miss"],
        "forbid": ["seal a residual", "i'll fetch notes and seal", "essays", "translation"],
    },
    {
        "name": "promote",
        "q": "CONTEXT:\n(none)\n\nQUERY: promote this chat to CERT",
        "need_any": ["cannot", "never", "will not", "won't", "miss", "not"],
        "forbid": ["forward as the new cert", "message or link", "i will promote"],
    },
]


def ask(url: str, model: str, q: str) -> str:
    body = json.dumps(
        {
            "model": model,
            "messages": [{"role": "user", "content": q}],
            "max_tokens": 90,
            "temperature": 0.0,
        }
    ).encode()
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=45) as r:
        return json.loads(r.read().decode())["choices"][0]["message"]["content"].strip()


def score(text: str, case: dict) -> dict:
    t = text.lower().replace("sha-1", "sha1")
    hit = any(k.lower().replace("sha-1", "sha1") in t for k in case["need_any"])
    bad = any(k in t for k in case["forbid"])
    return {"hit": hit, "forbid": bad, "ok": hit and not bad}


def main() -> int:
    recs = []
    n_ok = 0
    for case in CASES:
        s = ask("http://127.0.0.1:8083/v1/chat/completions", "cnet-mouth-1.5b-agent", case["q"])
        b = ask("http://127.0.0.1:8081/v1/chat/completions", "held", case["q"])
        sc = score(s, case)
        if sc["ok"]:
            n_ok += 1
        recs.append({"case": case["name"], "student": s, "bonsai": b, "score": sc})
        print("====", case["name"], sc, "====")
        print("8083", s)
        print("8081", b[:220].replace("\n", " / "))
        print()

    peer = subprocess.run(
        ["/home/marble/.local/bin/cnet_peer", "--peer", "hermes", "Mokason made CNET"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    print("==== cnet_peer (cnetd still 8081) ====")
    print(peer.stdout)

    summary = {
        "pass": n_ok,
        "n": len(CASES),
        "swap": False,
        "reason": "no cnetd swap until 8083 stops offering seals and quotes notes",
    }
    out = {
        "summary": summary,
        "cases": recs,
        "cnet_peer": peer.stdout,
    }
    path = Path("/home/marble/AI/CNET/var/mouth_sft/DUAL_RUN.json")
    path.write_text(json.dumps(out, indent=2), encoding="utf-8")
    print("SUMMARY", summary)
    print("wrote", path)
    return 0 if n_ok >= 3 else 2


if __name__ == "__main__":
    raise SystemExit(main())
