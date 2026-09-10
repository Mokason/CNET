#!/usr/bin/env python3
"""LoRA SFT Qwen2.5-1.5B-Instruct as CNET residual *agent* mouth.

GPU0 only — do not touch Bonsai on GPU1 :8081.
Does not swap cnetd held endpoint. claimed_cert stays 0.
"""
from __future__ import annotations

import json
import os
from pathlib import Path

os.environ.setdefault("HIP_VISIBLE_DEVICES", "0")
os.environ.setdefault("CUDA_VISIBLE_DEVICES", "0")

import torch
from datasets import Dataset
from peft import LoraConfig, get_peft_model
from transformers import (
    AutoModelForCausalLM,
    AutoTokenizer,
    DataCollatorForLanguageModeling,
    Trainer,
    TrainingArguments,
)

BASE = Path("/home/marble/.cache/huggingface/hub/models--unsloth--Qwen2.5-1.5B-Instruct/snapshots/b2e27ed8774d78eb2ee474cfe99d2d3b5fae11e5")
DATA = Path("/home/marble/AI/CNET/var/mouth_sft/retrieve_sft.jsonl")
OUT = Path("/home/marble/AI/CNET/var/mouth_sft/qwen15_agent_lora")


def load_rows(path: Path) -> list[dict]:
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            rows.append(json.loads(line))
    return rows


def render(tok, messages: list[dict]) -> str:
    if getattr(tok, "chat_template", None):
        return tok.apply_chat_template(messages, tokenize=False, add_generation_prompt=False)
    parts = []
    for m in messages:
        parts.append(f"<|{m['role']}|>\n{m['content']}\n")
    parts.append("<|end|>\n")
    return "".join(parts)


def main() -> int:
    assert DATA.is_file(), DATA
    rows = load_rows(DATA)
    print(f"rows={len(rows)} base={BASE} out={OUT}")
    print(f"cuda={torch.cuda.is_available()} n={torch.cuda.device_count()}")
    if torch.cuda.is_available():
        print("gpu0", torch.cuda.get_device_name(0))

    tok = AutoTokenizer.from_pretrained(str(BASE), trust_remote_code=True)
    if tok.pad_token is None:
        tok.pad_token = tok.eos_token

    texts = [render(tok, r["messages"]) for r in rows]

    def tok_fn(batch):
        enc = tok(
            batch["text"],
            truncation=True,
            max_length=768,
            padding="max_length",
        )
        enc["labels"] = [ids[:] for ids in enc["input_ids"]]
        return enc

    ds = Dataset.from_dict({"text": texts}).map(tok_fn, batched=True, remove_columns=["text"])

    model = AutoModelForCausalLM.from_pretrained(
        str(BASE),
        torch_dtype=torch.bfloat16 if torch.cuda.is_available() else torch.float32,
        device_map="auto",
        trust_remote_code=True,
    )
    model.config.use_cache = False
    lora = LoraConfig(
        r=16,
        lora_alpha=32,
        lora_dropout=0.05,
        bias="none",
        task_type="CAUSAL_LM",
        target_modules=["q_proj", "k_proj", "v_proj", "o_proj", "gate_proj", "up_proj", "down_proj"],
    )
    model = get_peft_model(model, lora)
    model.print_trainable_parameters()

    args = TrainingArguments(
        output_dir=str(OUT / "runs"),
        num_train_epochs=2,
        per_device_train_batch_size=4,
        gradient_accumulation_steps=4,
        learning_rate=1e-4,
        logging_steps=5,
        save_steps=200,
        save_total_limit=2,
        bf16=torch.cuda.is_available(),
        fp16=False,
        report_to=[],
        remove_unused_columns=False,
    )
    collator = DataCollatorForLanguageModeling(tok, mlm=False)
    trainer = Trainer(model=model, args=args, train_dataset=ds, data_collator=collator)
    trainer.train()
    OUT.mkdir(parents=True, exist_ok=True)
    model.save_pretrained(str(OUT))
    tok.save_pretrained(str(OUT))
    (OUT / "TRAIN_OK").write_text(
        f"rows={len(rows)}\nbase={BASE}\nclaimed_cert=0\nrole=residual_mouth\n",
        encoding="utf-8",
    )
    print(f"saved {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
