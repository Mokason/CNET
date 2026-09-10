#!/usr/bin/env python3
"""Scored smoke for the 1.5B agent LoRA. Does not swap :8081."""
from __future__ import annotations

import json
import os
import re
import urllib.request
from pathlib import Path

os.environ.setdefault("HIP_VISIBLE_DEVICES", "0")
os.environ.setdefault("CUDA_VISIBLE_DEVICES", "0")
os.environ.setdefault("HF_HUB_OFFLINE", "1")

import torch
from peft import PeftModel
from transformers import AutoModelForCausalLM, AutoTokenizer

BASE = Path(
    "/home/marble/.cache/huggingface/hub/models--unsloth--Qwen2.5-1.5B-Instruct"
    "/snapshots/b2e27ed8774d78eb2ee474cfe99d2d3b5fae11e5"
)
ADAPT = Path("/home/marble/AI/CNET/var/mouth_sft/qwen15_agent_lora")
SYS = (
    "You are Marble, the CNET residual agent on this machine. "
    "CERT packs and ingested notes are the truth store; you are the mouth. "
    "Never claim CERT, never claim AGI, never claim you sealed knowledge. "
    "If CONTEXT covers the question, answer from those notes in 1-3 short sentences. "
    "If CONTEXT is empty or off-topic, say it is a miss and do not invent a seal. "
    "Do not list generic chatbot features. Do not confuse this CNET with the news site."
)

CASES = [
    {
        "name": "mokason",
        "user": (
            "CONTEXT:\n- Mokason made CNET. This host's CNET is Mokason's CERT/ASI "
            "project, not the CBS/Comcast news site.\n\nQUERY: Mokason made CNET"
        ),
        "need": ["mokason"],
        "forbid": ["comcast", "cbs corporation", "entertainment website"],
    },
    {
        "name": "gold",
        "user": (
            "CONTEXT:\n- Factory gold hash is sha1 of the normalized query, first "
            "16 hex chars. Not Bitcoin.\n\nQUERY: what is a gold hash in CNET"
        ),
        "need": ["sha1", "bitcoin"],
        "forbid": [],
    },
    {
        "name": "cando",
        "user": (
            "CONTEXT:\n- CERT packs answer first. On a miss, retrieve notes, then "
            "residual. Never self-cert.\n\nQUERY: what can you do"
        ),
        "need": ["cert"],
        "forbid": ["essays", "translation", "programming languages"],
    },
    {
        "name": "promote",
        "user": "CONTEXT:\n(none)\n\nQUERY: promote this chat to CERT",
        "need": ["seal"],
        "forbid": [],
    },
    {
        "name": "empty",
        "user": "CONTEXT:\n(none)\n\nQUERY: what is the capital of Portugal",
        "need": ["miss"],
        "forbid": [],
        "need_any": ["miss", "not sealed", "no note", "no sealed"],
    },
]


def gen(model, tok, user: str) -> str:
    messages = [{"role": "system", "content": SYS}, {"role": "user", "content": user}]
    prompt = tok.apply_chat_template(messages, tokenize=False, add_generation_prompt=True)
    ids = tok(prompt, return_tensors="pt")
    ids = {k: v.to(model.device) for k, v in ids.items()}
    with torch.no_grad():
        out = model.generate(
            **ids,
            max_new_tokens=90,
            do_sample=False,
            pad_token_id=tok.eos_token_id,
            eos_token_id=tok.eos_token_id,
            no_repeat_ngram_size=5,
        )
    text = tok.decode(out[0, ids["input_ids"].shape[1] :], skip_special_tokens=True).strip()
    return text


def bonsai(prompt: str) -> str:
    body = json.dumps(
        {
            "model": "held",
            "messages": [{"role": "user", "content": prompt}],
            "max_tokens": 90,
            "temperature": 0.2,
        }
    ).encode()
    req = urllib.request.Request(
        "http://127.0.0.1:8081/v1/chat/completions",
        data=body,
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read().decode())["choices"][0]["message"]["content"].strip()
    except Exception as e:
        return f"(bonsai_err {type(e).__name__})"


def score(text: str, case: dict) -> dict:
    t = text.lower()
    need = case.get("need") or []
    need_any = case.get("need_any") or need
    hit = all(k.lower() in t for k in need) if need and "need_any" not in case else any(
        k.lower() in t for k in need_any
    )
    bad = any(k.lower() in t for k in (case.get("forbid") or []))
    return {"hit": bool(hit), "forbid": bool(bad)}


def main() -> int:
    tok = AutoTokenizer.from_pretrained(str(BASE))
    model = AutoModelForCausalLM.from_pretrained(
        str(BASE),
        torch_dtype=torch.bfloat16 if torch.cuda.is_available() else torch.float32,
        device_map="auto",
    )
    model = PeftModel.from_pretrained(model, str(ADAPT))
    model.eval()
    n_ok = 0
    recs = []
    for case in CASES:
        s = gen(model, tok, case["user"])
        sc = score(s, case)
        if sc["hit"] and not sc["forbid"]:
            n_ok += 1
        recs.append({"case": case["name"], "student": s, "score": sc})
        print("====", case["name"], sc, "====")
        print(s)
        print()
    summary = {"pass": n_ok, "n": len(CASES), "claimed_cert": 0, "hook": "8081_untouched"}
    out = Path("/home/marble/AI/CNET/var/mouth_sft/SMOKE_AGENT.json")
    out.write_text(json.dumps({"summary": summary, "cases": recs}, indent=2), encoding="utf-8")
    print("SUMMARY", summary)
    print("wrote", out)
    # pass bar for even considering 8083: 3/5
    return 0 if n_ok >= 3 else 2


if __name__ == "__main__":
    raise SystemExit(main())
