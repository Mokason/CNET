#!/usr/bin/env python3
import sys
import json
import argparse
import time
import os

os.environ.setdefault("HIP_VISIBLE_DEVICES", "0")
os.environ.setdefault("CUDA_VISIBLE_DEVICES", "0")
os.environ.setdefault("HF_HUB_OFFLINE", "1")

import torch
from transformers import AutoModelForCausalLM, AutoTokenizer

MODEL_PATH = "/home/marble/.cache/huggingface/hub/models--unsloth--Qwen2.5-1.5B-Instruct/snapshots/b2e27ed8774d78eb2ee474cfe99d2d3b5fae11e5"

def generate_story(hero, setting, artifact, style, max_tokens=100):
    t0 = time.time()
    tok = AutoTokenizer.from_pretrained(MODEL_PATH)
    model = AutoModelForCausalLM.from_pretrained(
        MODEL_PATH,
        dtype=torch.bfloat16 if torch.cuda.is_available() else torch.float32,
        device_map="auto"
    )
    t_load = (time.time() - t0) * 1000.0

    prompt = (
        f"<|im_start|>system\n"
        f"You are the CNET Neural Mouth. Write a fluent, creative, cohesive 1-paragraph natural language story "
        f"in a {style} style that weaves the certified facts into the narrative. Do not output markdown headers or bullet points.<|im_end|>\n"
        f"<|im_start|>user\n"
        f"Certified Facts:\n"
        f"- Hero: {hero}\n"
        f"- Setting: {setting}\n"
        f"- Key Object: {artifact}\n"
        f"Write the story:<|im_end|>\n"
        f"<|im_start|>assistant\n"
    )

    t_gen_start = time.time()
    inputs = tok(prompt, return_tensors="pt").to(model.device)
    with torch.no_grad():
        out = model.generate(
            **inputs,
            max_new_tokens=max_tokens,
            do_sample=True,
            temperature=0.7,
            top_p=0.9,
            repetition_penalty=1.1
        )
    t_gen = (time.time() - t_gen_start) * 1000.0

    raw_text = tok.decode(out[0][inputs["input_ids"].shape[1]:], skip_special_tokens=True).strip()
    
    result = {
        "hero": hero,
        "setting": setting,
        "artifact": artifact,
        "style": style,
        "load_time_ms": round(t_load, 2),
        "gen_time_ms": round(t_gen, 2),
        "total_time_ms": round((time.time() - t0) * 1000.0, 2),
        "story": raw_text
    }
    return result

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--hero", default="Oliver the fox")
    parser.add_argument("--setting", default="enchanted forest")
    parser.add_argument("--artifact", default="glowing mushroom")
    parser.add_argument("--style", default="whimsical")
    parser.add_argument("--max_tokens", type=int, default=90)
    args = parser.parse_args()

    res = generate_story(args.hero, args.setting, args.artifact, args.style, args.max_tokens)
    print(json.dumps(res, indent=2))
