#!/usr/bin/env python3
"""Reliable full OmniDoc Unlimited infer (single process, one GPU).

Usage:
  HIP_VISIBLE_DEVICES=0 python tools/roe_omnidoc_full_simple.py --shard 0 --shards 2
  HIP_VISIBLE_DEVICES=1 python tools/roe_omnidoc_full_simple.py --shard 1 --shards 2
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GT = ROOT / "artifacts/omnidoc_bench/OmniDocBench.json"
IMG = ROOT / "artifacts/omnidoc_bench/images"
OUT = ROOT / "artifacts/omnidoc_full/predictions_unlimited"
WORK = ROOT / "artifacts/omnidoc_full/work_simple"
MODEL = Path(os.path.expanduser("~/AI/Models/baidu-Unlimited-OCR"))


def find_img(ip: str) -> Path | None:
    name = Path(ip).name
    p = IMG / name
    return p if p.is_file() else None


def hb(msg: str) -> None:
    """Heartbeat to disk (stdout can hang under some ROCm/no-TTY cases)."""
    p = ROOT / "logs" / "roe_simple_heartbeat.txt"
    p.parent.mkdir(parents=True, exist_ok=True)
    line = f"{time.strftime('%H:%M:%S')} {msg}\n"
    with open(p, "a") as f:
        f.write(line)
        f.flush()
    print(msg, flush=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--shard", type=int, default=0)
    ap.add_argument("--shards", type=int, default=1)
    ap.add_argument("--skip-existing", action="store_true", default=True)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    os.environ.setdefault("HF_HUB_DISABLE_PROGRESS_BARS", "1")
    os.environ.setdefault("TRANSFORMERS_VERBOSITY", "error")
    os.environ.setdefault("TOKENIZERS_PARALLELISM", "false")

    hb(f"[simple] shard={args.shard}/{args.shards} hip={os.environ.get('HIP_VISIBLE_DEVICES')}")
    import torch
    from transformers import AutoModel, AutoTokenizer
    import contextlib, io

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
        if args.skip_existing and out_md.is_file() and out_md.stat().st_size > 10:
            continue
        jobs.append((i, stem, img, out_md))
    if args.limit:
        jobs = jobs[: args.limit]
    hb(f"[simple] todo={len(jobs)}")
    if not jobs:
        hb("[simple] nothing to do")
        return 0

    OUT.mkdir(parents=True, exist_ok=True)
    WORK.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    hb("[simple] load tokenizer")
    tok = AutoTokenizer.from_pretrained(str(MODEL), trust_remote_code=True)
    hb("[simple] load weights cpu")
    model = AutoModel.from_pretrained(
        str(MODEL), trust_remote_code=True, use_safetensors=True, torch_dtype=torch.bfloat16
    )
    hb("[simple] empty_cache + to cuda")
    torch.cuda.empty_cache()
    # non_blocking copy path
    model = model.eval()
    model.to(device="cuda", non_blocking=False)
    torch.cuda.synchronize()
    hb(f"[simple] ready mem={torch.cuda.memory_allocated()/1e9:.2f}GB load_s={time.time()-t0:.1f}")

    done = 0
    t1 = time.time()
    for i, stem, img, out_md in jobs:
        w = WORK / f"s{args.shard}_{stem}"
        w.mkdir(parents=True, exist_ok=True)
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                model.infer(
                    tok,
                    prompt="<image>document parsing.",
                    image_file=str(img),
                    output_path=str(w),
                    base_size=1024,
                    image_size=640,
                    crop_mode=True,
                    max_length=8192,
                    save_results=True,
                )
            md = w / "result.md"
            txt = md.read_text(encoding="utf-8", errors="replace") if md.is_file() else ""
            out_md.write_text(txt, encoding="utf-8")
            done += 1
            hb(f"[simple] wrote {stem} ({done}/{len(jobs)})")
        except Exception as e:
            (w / "err.txt").write_text(str(e))
            hb(f"[simple] ERR {stem}: {e}")
            continue
        if done % 5 == 0 or done == len(jobs):
            dt = time.time() - t1
            rate = done / max(dt, 1e-3)
            eta = (len(jobs) - done) / max(rate, 1e-3)
            hb(f"[simple] {done}/{len(jobs)} rate={rate:.3f}/s eta={eta/60:.1f}m")
    hb(f"[simple] done={done} total_s={time.time()-t0:.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
