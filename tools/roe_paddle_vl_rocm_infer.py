#!/usr/bin/env python3
"""PaddleOCR-VL-1.6 page-level infer on AMD ROCm via transformers.

NOTE: This is the transformers full-page OCR path (task prompt "OCR:"),
not the official Paddle CUDA layout+VLM pipeline. Quality may lag the
published 96.33 Overall; it is the best GPU path available without NVIDIA.

Usage:
  HIP_VISIBLE_DEVICES=0 python tools/roe_paddle_vl_rocm_infer.py --shard 0 --shards 2
  HIP_VISIBLE_DEVICES=1 python tools/roe_paddle_vl_rocm_infer.py --shard 1 --shards 2
"""
from __future__ import annotations

import argparse
import json
import os
import re
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GT = ROOT / "artifacts/omnidoc_bench/OmniDocBench.json"
IMG = ROOT / "artifacts/omnidoc_bench/images"
OUT = ROOT / "artifacts/omnidoc_full/predictions_paddle_vl16"
WORK = ROOT / "artifacts/omnidoc_full/work_paddle_vl16"
MODEL = "PaddlePaddle/PaddleOCR-VL-1.6"


def find_img(ip: str) -> Path | None:
    name = Path(ip).name
    p = IMG / name
    return p if p.is_file() else None


def strip_prompt(text: str) -> str:
    """Remove chat/prompt scaffolding if present."""
    t = text or ""
    # common patterns
    for sep in ("Assistant:", "OCR:", "User:"):
        if sep in t:
            # take last assistant chunk when multi-turn echo
            parts = t.split(sep)
            t = parts[-1]
    t = t.strip()
    # drop leading role noise
    t = re.sub(r"^(User:|System:).*\n", "", t, flags=re.I)
    return t.strip()


def hb(path: Path, msg: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    line = f"{time.strftime('%H:%M:%S')} {msg}\n"
    with open(path, "a") as f:
        f.write(line)
        f.flush()
    print(msg, flush=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--shard", type=int, default=0)
    ap.add_argument("--shards", type=int, default=1)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--skip-existing", action="store_true", default=True)
    ap.add_argument("--max-new-tokens", type=int, default=4096)
    ap.add_argument("--prompt", type=str, default="OCR:")
    args = ap.parse_args()

    os.environ.setdefault("TORCH_ROCM_AOTRITON_ENABLE_EXPERIMENTAL", "1")
    os.environ.setdefault("HF_HUB_DISABLE_PROGRESS_BARS", "1")
    os.environ.setdefault("TOKENIZERS_PARALLELISM", "false")

    log = ROOT / "logs" / f"roe_paddle_vl16_s{args.shard}.log"
    # also tee-ish via hb file
    hbf = ROOT / "logs" / f"roe_paddle_vl16_s{args.shard}_hb.txt"

    import torch
    from PIL import Image
    from transformers import AutoModel, AutoProcessor

    data = json.loads(GT.read_text())
    jobs = []
    for i, page in enumerate(data):
        if i % args.shards != args.shard:
            continue
        img = find_img(page["page_info"]["image_path"])
        if not img:
            continue
        stem = Path(page["page_info"]["image_path"]).stem
        out_md = OUT / f"{stem}.md"
        if args.skip_existing and out_md.is_file() and out_md.stat().st_size > 20:
            continue
        jobs.append((i, stem, img, out_md))
    if args.limit:
        jobs = jobs[: args.limit]

    OUT.mkdir(parents=True, exist_ok=True)
    WORK.mkdir(parents=True, exist_ok=True)
    hb(hbf, f"[paddle-vl] shard={args.shard}/{args.shards} hip={os.environ.get('HIP_VISIBLE_DEVICES')} todo={len(jobs)}")
    if not jobs:
        hb(hbf, "[paddle-vl] nothing to do")
        return 0

    t0 = time.time()
    hb(hbf, "[paddle-vl] load processor+model")
    processor = AutoProcessor.from_pretrained(MODEL, trust_remote_code=True)
    model = AutoModel.from_pretrained(MODEL, trust_remote_code=True, torch_dtype=torch.bfloat16)
    model = model.eval().to("cuda")
    torch.cuda.synchronize()
    hb(hbf, f"[paddle-vl] ready mem={torch.cuda.memory_allocated()/1e9:.2f}GB load_s={time.time()-t0:.1f}")

    done = 0
    t1 = time.time()
    for i, stem, img, out_md in jobs:
        try:
            image = Image.open(img).convert("RGB")
            messages = [
                {
                    "role": "user",
                    "content": [
                        {"type": "image", "image": image},
                        {"type": "text", "text": args.prompt},
                    ],
                }
            ]
            inputs = processor.apply_chat_template(
                messages,
                tokenize=True,
                add_generation_prompt=True,
                return_dict=True,
                return_tensors="pt",
            )
            inputs = {k: (v.to("cuda") if hasattr(v, "to") else v) for k, v in inputs.items()}
            with torch.no_grad():
                out = model.generate(**inputs, max_new_tokens=args.max_new_tokens, do_sample=False)
            text = processor.batch_decode(out, skip_special_tokens=True)[0]
            text = strip_prompt(text)
            out_md.write_text(text, encoding="utf-8")
            done += 1
            dt = time.time() - t1
            rate = done / max(dt, 1e-3)
            eta = (len(jobs) - done) / max(rate, 1e-9)
            hb(
                hbf,
                f"[paddle-vl] wrote {stem[:50]} ({done}/{len(jobs)}) L={len(text)} "
                f"rate={rate*3600:.1f}/h eta={eta/3600:.1f}h",
            )
            torch.cuda.empty_cache()
        except Exception as e:
            (WORK / f"err_s{args.shard}_{stem}.txt").write_text(str(e))
            hb(hbf, f"[paddle-vl] ERR {stem}: {e}")
            continue
    hb(hbf, f"[paddle-vl] done={done} total_s={time.time()-t0:.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
