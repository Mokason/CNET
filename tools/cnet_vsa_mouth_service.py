#!/usr/bin/env python3
"""High-speed warm Neural Mouth microservice for CNET-VSA Hybrid Architecture & Discord Peer.
Listens on 127.0.0.1:8084.
"""
import sys
import os
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

os.environ.setdefault("HIP_VISIBLE_DEVICES", "0")
os.environ.setdefault("CUDA_VISIBLE_DEVICES", "0")
os.environ.setdefault("HF_HUB_OFFLINE", "1")

from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from cnet_vsa_live_capsules import CAPSULE_STORE

import torch
from transformers import AutoModelForCausalLM, AutoTokenizer

MODEL_PATH = "/home/marble/.cache/huggingface/hub/models--unsloth--Qwen2.5-1.5B-Instruct/snapshots/b2e27ed8774d78eb2ee474cfe99d2d3b5fae11e5"
HOST = "127.0.0.1"
PORT = 8084

print(f"[Mouth Service] Loading Qwen2.5-1.5B from {MODEL_PATH} ...", flush=True)
t0 = time.time()
tokenizer = AutoTokenizer.from_pretrained(MODEL_PATH)
model = AutoModelForCausalLM.from_pretrained(
    MODEL_PATH,
    dtype=torch.bfloat16 if torch.cuda.is_available() else torch.float32,
    device_map="auto"
)
model.eval()
print(f"[Mouth Service] Ready on {HOST}:{PORT} (Loaded in {time.time() - t0:.2f}s, GPU: {torch.cuda.is_available()})", flush=True)

def clean_neural_completion(text: str) -> str:
    """Ensures generated neural text terminates at a clean sentence or code fence boundary."""
    text = text.rstrip()
    if not text:
        return text
    # Ensure any unclosed code fences are closed
    if text.count("```") % 2 != 0:
        text += "\n```"
    # If already ending cleanly with punctuation or formatting quote/bracket
    if text[-1] in {".", "!", "?", '"', "'", "”", "’", "`", "}", ")"}:
        return text
    # Find the last sentence terminator
    last_stop = -1
    for i in range(len(text) - 1, -1, -1):
        if text[i] in {".", "!", "?"}:
            last_stop = i + 1
            break
    if last_stop > 20:
        trimmed = text[:last_stop].rstrip()
        if trimmed.count("```") % 2 != 0:
            trimmed += "\n```"
        if trimmed.count("**") % 2 != 0:
            trimmed += "**"
        elif trimmed.count("*") % 2 != 0:
            trimmed += "*"
        return trimmed
    return text + "."

def generate_prose(hero, setting, artifact, style, max_tokens=320):
    t_start = time.time()
    prompt = (
        f"<|im_start|>system\n"
        f"You are the CNET Neural Mouth. Your task is to write a single fluid, captivating paragraph in a {style} style.\n"
        f"Strict Requirements:\n"
        f"1. You must feature the Hero: '{hero}'.\n"
        f"2. You must feature the Setting: '{setting}'.\n"
        f"3. You must feature the Object/Event: '{artifact}'.\n"
        f"4. Stay focused on these elements without inventing extraneous weapons or conflicting factions.\n"
        f"5. Output only the complete narrative paragraph with no preamble or title.<|im_end|>\n"
        f"<|im_start|>user\n"
        f"Compose the complete {style} story paragraph now.<|im_end|>\n"
        f"<|im_start|>assistant\n"
    )

    inputs = tokenizer(prompt, return_tensors="pt").to(model.device)
    with torch.no_grad():
        out = model.generate(
            **inputs,
            max_new_tokens=max_tokens,
            do_sample=True,
            temperature=0.65,
            top_p=0.88,
            repetition_penalty=1.12
        )
    gen_time_ms = (time.time() - t_start) * 1000.0

    raw_text = tokenizer.decode(out[0][inputs["input_ids"].shape[1]:], skip_special_tokens=True).strip()
    raw_text = clean_neural_completion(raw_text)
    return {
        "story": raw_text,
        "gen_time_ms": round(gen_time_ms, 2),
        "tokens": len(out[0]) - inputs["input_ids"].shape[1]
    }

def generate_chat(messages, max_tokens=512):
    t_start = time.time()

    # Extract last user query and resolve contextual pronouns if needed
    last_user_query = ""
    for m in reversed(messages):
        if m.get("role") == "user":
            c = m.get("content", "")
            if ":" in c:
                c = c.split(":", 1)[1].strip()
            last_user_query = c
            break

    # Contextual resolution for short or pronoun-heavy followups
    probe_query = last_user_query
    if any(pron in last_user_query.lower().split() for pron in ["it", "this", "that", "them", "these", "those"]) or len(last_user_query.split()) <= 4:
        for m in reversed(messages[:-1]):
            if m.get("role") == "user":
                prev_c = m.get("content", "")
                if ":" in prev_c: prev_c = prev_c.split(":", 1)[1].strip()
                probe_query = f"{prev_c} {last_user_query}"
                break

    # Grounding probe: search CNET live capsules
    grounded_caps = CAPSULE_STORE.query(probe_query, top_k=2, min_sim=0.18) if probe_query else []

    system_prompt = (
        "You are Marble, the intelligent AI companion in CNET. "
        "You are conversing with users on Discord.\n"
        "Guidelines:\n"
        "1. Be friendly, natural, knowledgeable, helpful, and concise.\n"
        "2. Multi-Turn Context: Always maintain conversation continuity. When the user asks 'how can you use it?', 'why?', or uses pronouns ('it', 'this', 'that'), resolve them directly from the preceding turns.\n"
        "3. Hardware Strict Rule: CNET runs exclusively on AMD GPUs with ROCm (gfx1201). NEVER mention NVIDIA or CUDA. Never invent ports like 'ROCCAM'. Always refer to AMD GPUs, ROCm, and HIP.\n"
        "4. Keep responses engaging, technical when asked, and formatted cleanly for Discord markdown. Do not output repetitive filler."
    )

    if grounded_caps:
        facts_block = "\n".join(f"- {c['statement']}" for c in grounded_caps)
        system_prompt += (
            f"\n\n[CERTIFIED KNOWLEDGE BASE - GROUND TRUTH]:\n{facts_block}\n"
            f"(STRICT REQUIREMENT: The facts above are absolute ground truth. Your response must accurately reflect them.)"
        )

    conv_messages = [{"role": "system", "content": system_prompt}]
    for m in messages[-8:]:
        conv_messages.append({"role": m.get("role", "user"), "content": m.get("content", "")})

    prompt = tokenizer.apply_chat_template(conv_messages, tokenize=False, add_generation_prompt=True)
    inputs = tokenizer(prompt, return_tensors="pt").to(model.device)
    with torch.no_grad():
        out = model.generate(
            **inputs,
            max_new_tokens=max_tokens,
            do_sample=True,
            temperature=0.7,
            top_p=0.9,
            repetition_penalty=1.1
        )
    gen_time_ms = (time.time() - t_start) * 1000.0
    raw_text = tokenizer.decode(out[0][inputs["input_ids"].shape[1]:], skip_special_tokens=True).strip()
    raw_text = clean_neural_completion(raw_text)

    grounding_status = "unconstrained"
    if grounded_caps:
        top_cap = grounded_caps[0]
        audit = CAPSULE_STORE.audit_anti_hallucination(last_user_query, raw_text)
        if audit.get("grounded"):
            grounding_status = "grounded_verified"
            raw_text += f"\n\n🛡️ `[CNET Grounded: {top_cap['statement']}]`"
        elif audit.get("verdict") == "GROUNDING_GAP_OR_CONTRADICTION":
            grounding_status = "overridden_safe"
            raw_text = (
                f"According to CNET's certified knowledge base: **{top_cap['statement']}**.\n\n"
                f"🛡️ `[Anti-Hallucination Guard: Overrode ungrounded model output with certified capsule {top_cap['id']}]`"
            )

    return {
        "reply": raw_text,
        "grounded": grounding_status,
        "gen_time_ms": round(gen_time_ms, 2),
        "tokens": len(out[0]) - inputs["input_ids"].shape[1]
    }

class RequestHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if self.path == "/health":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"status": "ok", "service": "cnet-vsa-mouth-warm"}')
            return
        elif self.path == "/capsules":
            caps = CAPSULE_STORE.list_all()
            resp_body = json.dumps({"status": "ok", "capsules": caps}).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp_body)))
            self.end_headers()
            self.wfile.write(resp_body)
            return
        self.send_response(404)
        self.end_headers()

    def do_POST(self):
        content_len = int(self.headers.get("Content-Length", 0))
        raw_body = self.rfile.read(content_len).decode("utf-8") if content_len > 0 else "{}"
        try:
            data = json.loads(raw_body)
        except Exception:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(b'{"error": "invalid json"}')
            return

        if self.path == "/generate":
            hero = data.get("hero", "curious fox")
            setting = data.get("setting", "enchanted forest")
            artifact = data.get("artifact", "glowing mushroom")
            style = data.get("style", "whimsical")
            max_tokens = int(data.get("max_tokens", 320))

            res = generate_prose(hero, setting, artifact, style, max_tokens)
            resp_body = json.dumps(res).encode("utf-8")

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp_body)))
            self.end_headers()
            self.wfile.write(resp_body)
            return

        elif self.path == "/chat":
            messages = data.get("messages", [])
            if not messages and "prompt" in data:
                messages = [{"role": "user", "content": data["prompt"]}]
            max_tokens = int(data.get("max_tokens", 512))

            res = generate_chat(messages, max_tokens)
            resp_body = json.dumps(res).encode("utf-8")

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp_body)))
            self.end_headers()
            self.wfile.write(resp_body)
            return

        elif self.path == "/learn":
            text = data.get("text", "")
            author = data.get("author", "user")
            cid = data.get("channel_id", "")
            res = CAPSULE_STORE.learn(text, author, cid)
            resp_body = json.dumps(res).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp_body)))
            self.end_headers()
            self.wfile.write(resp_body)
            return

        self.send_response(404)
        self.end_headers()

if __name__ == "__main__":
    server = ThreadingHTTPServer((HOST, PORT), RequestHandler)
    server.serve_forever()
