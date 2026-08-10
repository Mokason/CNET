#!/usr/bin/env python3
"""Dual-GPU OmniDoc full-set inference (Unlimited baseline for real SOTA path).

Shards page indices across GPUs via HIP_VISIBLE_DEVICES / CUDA_VISIBLE_DEVICES.
Each worker loads one Unlimited copy and writes predictions_unlimited/<stem>.md

Usage:
  # parent (spawns 2 workers):
  python tools/roe_omnidoc_dual_gpu_infer.py --gpus 0,1 --mode unlimited

  # single worker (called by parent):
  HIP_VISIBLE_DEVICES=0 python tools/roe_omnidoc_dual_gpu_infer.py --worker 0 --workers 2
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
GT = ROOT / "artifacts" / "omnidoc_bench" / "OmniDocBench.json"
IMG = ROOT / "artifacts" / "omnidoc_bench" / "images"
OUT = ROOT / "artifacts" / "omnidoc_full" / "predictions_unlimited"
WORK = ROOT / "artifacts" / "omnidoc_full" / "work_unl"
MODEL = Path(os.path.expanduser("~/AI/Models/baidu-Unlimited-OCR"))
LOG = ROOT / "logs"


def download_missing(data: list, max_dl: int = 0) -> int:
    from huggingface_hub import hf_hub_download

    IMG.mkdir(parents=True, exist_ok=True)
    missing = []
    for i, p in enumerate(data):
        name = Path(p["page_info"]["image_path"]).name
        if not (IMG / name).is_file():
            missing.append((i, p["page_info"]["image_path"], name))
    print(f"[dl] missing={len(missing)}/{len(data)}", flush=True)
    n = 0
    for i, ip, name in missing:
        if max_dl and n >= max_dl:
            break
        for remote in (f"images/{name}", f"images/{ip}", ip):
            try:
                hf_hub_download(
                    "opendatalab/OmniDocBench",
                    remote,
                    repo_type="dataset",
                    local_dir=str(ROOT / "artifacts" / "omnidoc_bench"),
                )
                n += 1
                if n % 25 == 0:
                    print(f"[dl] got {n}", flush=True)
                break
            except Exception:
                continue
    print(f"[dl] downloaded_attempts_ok~{n}", flush=True)
    return n


def find_img(image_path: str) -> Path | None:
    name = Path(image_path).name
    for c in (IMG / name, ROOT / "artifacts" / "omnidoc_bench" / image_path):
        if c.is_file():
            return c
    return None


def run_worker(worker: int, workers: int, skip_existing: bool) -> int:
    import sys
    sys.stdout.reconfigure(line_buffering=True) if hasattr(sys.stdout, "reconfigure") else None
    os.environ.setdefault("HIP_VISIBLE_DEVICES", os.environ.get("HIP_VISIBLE_DEVICES", "0"))
    # After HIP_VISIBLE_DEVICES, torch sees only one device as cuda:0
    print(f"[w{worker}] import torch...", flush=True)
    import torch
    from transformers import AutoModel, AutoTokenizer

    data = json.loads(GT.read_text())
    # shard
    idxs = [i for i in range(len(data)) if i % workers == worker]
    ready = []
    for i in idxs:
        img = find_img(data[i]["page_info"]["image_path"])
        if img:
            ready.append((i, data[i], img))
    print(f"[w{worker}] shard={len(idxs)} ready={len(ready)} hip={os.environ.get('HIP_VISIBLE_DEVICES')}", flush=True)
    if not ready:
        print(f"[w{worker}] nothing ready", flush=True)
        return 0

    OUT.mkdir(parents=True, exist_ok=True)
    print(f"[w{worker}] load tokenizer...", flush=True)
    tok = AutoTokenizer.from_pretrained(str(MODEL), trust_remote_code=True)
    print(f"[w{worker}] load model weights...", flush=True)
    model = AutoModel.from_pretrained(
        str(MODEL), trust_remote_code=True, use_safetensors=True, torch_dtype=torch.bfloat16
    )
    print(f"[w{worker}] to cuda...", flush=True)
    model = model.eval().to("cuda")
    print(f"[w{worker}] ready mem_gb={torch.cuda.memory_allocated()/1e9:.2f}", flush=True)

    import contextlib
    import io

    t0 = time.time()
    done = 0
    for i, page, img in ready:
        stem = Path(page["page_info"]["image_path"]).stem
        out_md = OUT / f"{stem}.md"
        if skip_existing and out_md.is_file() and out_md.stat().st_size > 10:
            done += 1
            continue
        work = WORK / f"w{worker}_{stem}"
        work.mkdir(parents=True, exist_ok=True)
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                model.infer(
                    tok,
                    prompt="<image>document parsing.",
                    image_file=str(img),
                    output_path=str(work),
                    base_size=1024,
                    image_size=640,
                    crop_mode=True,
                    max_length=8192,
                    save_results=True,
                )
        except Exception as e:
            (work / "error.txt").write_text(str(e))
            print(f"[w{worker}] ERR {stem}: {e}", flush=True)
            continue
        md = work / "result.md"
        txt = md.read_text(encoding="utf-8", errors="replace") if md.is_file() else buf.getvalue()
        out_md.write_text(txt or "", encoding="utf-8")
        done += 1
        if done % 5 == 0 or done == len(ready):
            dt = time.time() - t0
            rate = done / max(dt, 1e-6)
            print(f"[w{worker}] {done}/{len(ready)} rate={rate:.3f}/s eta_s={(len(ready)-done)/max(rate,1e-6):.0f}", flush=True)
    print(f"[w{worker}] done {done} in {time.time()-t0:.1f}s", flush=True)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gpus", default="0,1", help="comma GPU ids for parent spawn")
    ap.add_argument("--worker", type=int, default=-1, help="worker id if child")
    ap.add_argument("--workers", type=int, default=2)
    ap.add_argument("--download-only", action="store_true")
    ap.add_argument("--download-first", action="store_true", default=True)
    ap.add_argument("--skip-existing", action="store_true", default=True)
    ap.add_argument("--max-download", type=int, default=0)
    args = ap.parse_args()

    LOG.mkdir(parents=True, exist_ok=True)
    data = json.loads(GT.read_text())
    print(f"[main] gt={len(data)}", flush=True)

    if args.download_only or args.download_first:
        download_missing(data, max_dl=args.max_download)

    if args.download_only:
        # recount
        have = sum(1 for p in data if find_img(p["page_info"]["image_path"]))
        print(f"[main] images_ready={have}/{len(data)}")
        return 0

    if args.worker >= 0:
        return run_worker(args.worker, args.workers, args.skip_existing)

    # parent: spawn one process per GPU
    gpu_ids = [g.strip() for g in args.gpus.split(",") if g.strip() != ""]
    workers = len(gpu_ids)
    py = str(ROOT / ".venv-unlimited-ocr" / "bin" / "python")
    script = str(Path(__file__).resolve())
    procs = []
    for w, gid in enumerate(gpu_ids):
        env = os.environ.copy()
        env["HIP_VISIBLE_DEVICES"] = gid
        env["CUDA_VISIBLE_DEVICES"] = gid
        # hide iGPU issues
        log = open(LOG / f"roe_omnidoc_dual_w{w}.log", "w")
        cmd = [py, script, "--worker", str(w), "--workers", str(workers)]
        if args.skip_existing:
            cmd.append("--skip-existing")
        print(f"[main] spawn worker {w} gpu={gid}", flush=True)
        p = subprocess.Popen(cmd, cwd=str(ROOT), env=env, stdout=log, stderr=subprocess.STDOUT)
        procs.append((w, p, log))

    rc = 0
    for w, p, log in procs:
        c = p.wait()
        log.close()
        print(f"[main] worker {w} exit={c}", flush=True)
        if c != 0:
            rc = c
    # summary
    npred = len(list(OUT.glob("*.md")))
    print(f"[main] predictions_unlimited={npred}", flush=True)
    (ROOT / "artifacts" / "omnidoc_full" / "dual_infer_summary.json").write_text(
        json.dumps({"predictions": npred, "gt": len(data), "gpus": gpu_ids}, indent=2)
    )
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
