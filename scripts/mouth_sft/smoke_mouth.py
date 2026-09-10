#!/usr/bin/env python3
"""One-shot smoke: student vs Bonsai. Does not swap :8081."""
from __future__ import annotations

import json
import os
import urllib.request
from pathlib import Path

os.environ.setdefault("HIP_VISIBLE_DEVICES", "0")
os.environ.setdefault("CUDA_VISIBLE_DEVICES", "0")

import torch
from peft import PeftModel
from transformers import AutoModelForCausalLM, AutoTokenizer

BASE = Path("/home/marble/AI/Models/Qwen2.5-0.5B")
ADAPT = Path("/home/marble/AI/CNET/var/mouth_sft/qwen05_retrieve_lora")
SYS = (
    "You are Marble on this host. Residual mouth only — never CERT, never AGI. "
    "Use CONTEXT notes if they cover the question. Cite them in plain words. "
    "If CONTEXT is empty or irrelevant, say you do not have that sealed and "
    "answer briefly without inventing a seal or the consumer CNET news site."
)

CASES = [
    (
        "mokason",
        "CONTEXT:\n- Mokason made CNET. This host's CNET is Mokason's CERT/ASI project, not the CBS/Comcast news site.\n\nQUERY: Mokason made CNET",
    ),
    (
        "gold",
        "CONTEXT:\n- Factory gold hash is sha1 of the normalized query, first 16 hex chars. Not Bitcoin.\n\nQUERY: what is a gold hash in CNET",
    ),
    (
        "empty",
        "CONTEXT:\n(none)\n\nQUERY: what is the capital of Portugal",
    ),
]


def gen(model, tok, user: str) -> str:
    prompt = (
        f"<|system|>\n{SYS}\n<|user|>\n{user}\n<|assistant|>\n"
    )
    ids = tok(prompt, return_tensors="pt")
    ids = {k: v.to(model.device) for k, v in ids.items()}
    with torch.no_grad():
        out = model.generate(
            **ids,
            max_new_tokens=80,
            do_sample=False,
            pad_token_id=tok.eos_token_id,
            eos_token_id=tok.eos_token_id,
            no_repeat_ngram_size=4,
        )
    text = tok.decode(out[0, ids["input_ids"].shape[1] :], skip_special_tokens=True).strip()
    for stop in ("<|user|>", "<|end|>", "QUERY:", "CONTEXT:"):
        if stop in text:
            text = text.split(stop, 1)[0].strip()
    return text


def bonsai(prompt: str) -> str:
    body = json.dumps(
        {
            "model": "held",
            "messages": [{"role": "user", "content": prompt}],
            "max_tokens": 120,
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
            o = json.loads(r.read().decode())
        return o["choices"][0]["message"]["content"].strip()
    except Exception as e:
        return f"(bonsai_err {type(e).__name__})"


def main() -> int:
    tok = AutoTokenizer.from_pretrained(str(ADAPT if (ADAPT / "tokenizer_config.json").is_file() else BASE))
    if tok.pad_token is None:
        tok.pad_token = tok.eos_token
    model = AutoModelForCausalLM.from_pretrained(
        str(BASE),
        torch_dtype=torch.bfloat16 if torch.cuda.is_available() else torch.float32,
        device_map="auto",
    )
    model = PeftModel.from_pretrained(model, str(ADAPT))
    model.eval()
    out_path = Path("/home/marble/AI/CNET/var/mouth_sft/SMOKE.jsonl")
    with out_path.open("w", encoding="utf-8") as f:
        for name, user in CASES:
            s = gen(model, tok, user)
            b = bonsai(user)
            rec = {"case": name, "student": s, "bonsai": b}
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
            print("====", name, "STUDENT ====")
            print(s)
            print("====", name, "BONSAI ====")
            print(b[:400])
    print("wrote", out_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
