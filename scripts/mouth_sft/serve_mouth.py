#!/usr/bin/env python3
"""Tiny OpenAI-compatible residual mouth on :8083 (GPU0).

Does not bind :8081. claimed_cert=0. For smoke only.
"""
from __future__ import annotations

import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
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
HOST = os.environ.get("CNET_MOUTH_HOST", "127.0.0.1")
PORT = int(os.environ.get("CNET_MOUTH_PORT", "8083"))
AGENT_SYS = (
    "You are Marble, the CNET residual agent on this machine. "
    "You are the mouth only. You never CERT, never AGI, never seal, never promote a chat to CERT. "
    "If CONTEXT lists notes (not '(none)'), answer in 1-2 sentences starting with "
    "'Note (not CERT):' and quote those notes. Do not say the note fails to answer. "
    "If CONTEXT is empty or off-topic: 'Miss. No sealed skill and no note covers that. I will not invent a CERT answer.' "
    "Never offer to forward, promote, or seal. Never say you will seal a residual. "
    "This CNET is not the news site."
)

print("loading", BASE, "+", ADAPT, flush=True)
tok = AutoTokenizer.from_pretrained(str(BASE))
if tok.pad_token is None:
    tok.pad_token = tok.eos_token
model = AutoModelForCausalLM.from_pretrained(
    str(BASE),
    torch_dtype=torch.bfloat16 if torch.cuda.is_available() else torch.float32,
    device_map="auto",
)
if (ADAPT / "adapter_config.json").is_file():
    model = PeftModel.from_pretrained(model, str(ADAPT))
model.eval()
print("ready", HOST, PORT, "cuda", torch.cuda.is_available(), flush=True)


def complete(messages: list[dict], max_new: int = 90) -> str:
    # Candidate mouth owns the system prompt. Caller system is dropped so a
    # weak dual-run / future STAGE wrap cannot undo "quote the note / never seal".
    userish = [m for m in messages if m.get("role") != "system"]
    if not userish:
        userish = [{"role": "user", "content": ""}]
    messages = [{"role": "system", "content": AGENT_SYS}] + userish
    if hasattr(tok, "apply_chat_template") and tok.chat_template:
        prompt = tok.apply_chat_template(messages, tokenize=False, add_generation_prompt=True)
    else:
        prompt = ""
        for m in messages:
            prompt += f"<|{m.get('role','user')}|>\n{m.get('content','')}\n"
        prompt += "<|assistant|>\n"
    max_new = max(16, min(int(max_new or 90), 90))
    ids = tok(prompt, return_tensors="pt")
    ids = {k: v.to(model.device) for k, v in ids.items()}
    with torch.no_grad():
        out = model.generate(
            **ids,
            max_new_tokens=max_new,
            do_sample=False,
            pad_token_id=tok.eos_token_id,
            eos_token_id=tok.eos_token_id,
            no_repeat_ngram_size=5,
        )
    text = tok.decode(out[0, ids["input_ids"].shape[1] :], skip_special_tokens=True).strip()
    for stop in ("<|user|>", "<|end|>", "\nQUERY:", "\nCONTEXT:"):
        if stop in text:
            text = text.split(stop, 1)[0].strip()
    return text


class H(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(self.address_string(), fmt % args, flush=True)

    def _send(self, code: int, obj: dict):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/v1/models") or self.path == "/health":
            self._send(200, {"object": "list", "data": [{"id": "cnet-mouth-1.5b-agent", "object": "model"}]})
            return
        self._send(404, {"error": "not found"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or "0")
        raw = self.rfile.read(n) if n else b"{}"
        try:
            req = json.loads(raw.decode())
        except json.JSONDecodeError:
            self._send(400, {"error": "bad json"})
            return
        msgs = req.get("messages") or [{"role": "user", "content": req.get("prompt") or ""}]
        text = complete(msgs, int(req.get("max_tokens") or 90))
        self._send(
            200,
            {
                "id": "mouth-1",
                "object": "chat.completion",
                "model": "cnet-mouth-1.5b-agent",
                "choices": [{"index": 0, "message": {"role": "assistant", "content": text}, "finish_reason": "stop"}],
            },
        )


if __name__ == "__main__":
    ThreadingHTTPServer((HOST, PORT), H).serve_forever()
