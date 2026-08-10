#!/usr/bin/env python3
"""Full OmniDocBench prediction exporter (official layout).

Writes:
  artifacts/omnidoc_full/predictions_unlimited/<stem>.md
  artifacts/omnidoc_full/predictions_roe_hard/<stem>.md
  artifacts/omnidoc_full/manifest.jsonl

Modes:
  --download-only     fetch missing images from HF
  --limit N           only first N pages (after filter)
  --mode unlimited|roe_hard|both
  --skip-existing     resume
  --ids 1,2,3         specific page indices

Official eval needs: prediction folder of .md named like image stems.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

GT_PATH = ROOT / "artifacts" / "omnidoc_bench" / "OmniDocBench.json"
IMG_DIR = ROOT / "artifacts" / "omnidoc_bench" / "images"
OUT_ROOT = ROOT / "artifacts" / "omnidoc_full"
MODEL = Path(os.path.expanduser("~/AI/Models/baidu-Unlimited-OCR"))


def stem_of(image_path: str) -> str:
    return Path(image_path).stem


def find_image(image_path: str) -> Path | None:
    name = Path(image_path).name
    for c in (
        IMG_DIR / name,
        IMG_DIR / image_path,
        ROOT / "artifacts" / "omnidoc_bench" / image_path,
    ):
        if c.is_file():
            return c
    # fuzzy
    hits = list(IMG_DIR.glob(f"*{name}")) if IMG_DIR.is_dir() else []
    return hits[0] if hits else None


def download_image(image_path: str) -> Path | None:
    from huggingface_hub import hf_hub_download

    name = Path(image_path).name
    candidates = [
        f"images/{name}",
        f"images/{image_path}",
        image_path if str(image_path).startswith("images/") else None,
    ]
    local_dir = str(ROOT / "artifacts" / "omnidoc_bench")
    for remote in candidates:
        if not remote:
            continue
        try:
            p = hf_hub_download(
                "opendatalab/OmniDocBench",
                remote,
                repo_type="dataset",
                local_dir=local_dir,
            )
            return Path(p)
        except Exception:
            continue
    return None


def ensure_images(pages: list[dict], download: bool) -> list[tuple[int, dict, Path]]:
    out = []
    missing = 0
    for i, page in pages:
        ip = page["page_info"]["image_path"]
        img = find_image(ip)
        if not img and download:
            img = download_image(ip)
        if img and img.is_file():
            out.append((i, page, img))
        else:
            missing += 1
    print(f"[data] ready={len(out)} missing={missing}", flush=True)
    return out


def load_model():
    import torch
    from transformers import AutoModel, AutoTokenizer

    print("[model] loading Unlimited-OCR...", flush=True)
    tok = AutoTokenizer.from_pretrained(str(MODEL), trust_remote_code=True)
    model = AutoModel.from_pretrained(
        str(MODEL),
        trust_remote_code=True,
        use_safetensors=True,
        torch_dtype=torch.bfloat16,
    )
    model = model.eval().to("cuda")
    print(f"[model] ready mem_gb={torch.cuda.memory_allocated()/1e9:.2f}", flush=True)
    return tok, model


def infer_unlimited(model, tok, img: Path, work: Path) -> str:
    import contextlib
    import io

    work.mkdir(parents=True, exist_ok=True)
    buf = io.StringIO()
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
    stdout = buf.getvalue()
    md = work / "result.md"
    if md.is_file():
        return md.read_text(encoding="utf-8", errors="replace")
    # fallback strip dets from stdout-ish files
    from roe_omnidoc_quality_improve import strip_det  # type: ignore

    return strip_det(stdout)


def infer_roe_hard(model, tok, img: Path, work: Path, tags: list[str]) -> str:
    """Budgeted hard pipeline: gundam + base, det assemble, best-of by length/stability."""
    from roe_ocr_hard_beat import assemble_from_raw, formula_cleanup  # type: ignore
    from roe_omnidoc_quality_improve import strip_det  # type: ignore

    import contextlib
    import io

    work.mkdir(parents=True, exist_ok=True)
    cfgs = [
        ("gundam", dict(base_size=1024, image_size=640, crop_mode=True)),
        ("base", dict(base_size=1024, image_size=1024, crop_mode=False)),
    ]
    # newspapers: only gundam (base can hang / be huge)
    if "newspaper" in tags:
        cfgs = [cfgs[0]]

    cands = []
    for name, cfg in cfgs:
        odir = work / name
        odir.mkdir(parents=True, exist_ok=True)
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                model.infer(
                    tok,
                    prompt="<image>document parsing.",
                    image_file=str(img),
                    output_path=str(odir),
                    max_length=8192,
                    save_results=True,
                    **cfg,
                )
        except Exception as e:
            (odir / "error.txt").write_text(str(e))
            continue
        stdout = buf.getvalue()
        (odir / "stdout.txt").write_text(stdout, encoding="utf-8", errors="replace")
        md = odir / "result.md"
        cleaned = md.read_text(encoding="utf-8", errors="replace") if md.is_file() else ""
        raw = stdout if "<|det|>" in stdout else cleaned
        txt = assemble_from_raw(raw, tags) if "<|det|>" in raw else strip_det(raw or cleaned)
        if "math" in tags or "equation_hard" in tags:
            txt = formula_cleanup(txt)
        if txt.strip():
            cands.append(txt)
            (odir / "assembled.md").write_text(txt, encoding="utf-8")

    if not cands:
        return ""
    # pick longest non-repetitive
    def score(t: str) -> float:
        lines = [ln for ln in t.splitlines() if ln.strip()]
        uniq = len(set(lines)) / max(1, len(lines))
        return min(len(t), 12000) / 12000.0 * 0.6 + uniq * 0.4

    return max(cands, key=score)


def page_tags(page: dict) -> list[str]:
    try:
        from roe_ocr_hard_beat import is_hard_page  # type: ignore

        hard, tags = is_hard_page(page)
        return tags if hard else []
    except Exception:
        return []


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--download-only", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--mode", choices=["unlimited", "roe_hard", "both"], default="both")
    ap.add_argument("--skip-existing", action="store_true", default=True)
    ap.add_argument("--ids", type=str, default="")
    ap.add_argument("--stratified", type=int, default=0, help="pick N stratified by data_source")
    args = ap.parse_args()

    os.environ.setdefault("HIP_VISIBLE_DEVICES", "0")
    OUT_ROOT.mkdir(parents=True, exist_ok=True)
    IMG_DIR.mkdir(parents=True, exist_ok=True)

    data = json.loads(GT_PATH.read_text())
    print(f"[data] gt_pages={len(data)}", flush=True)

    if args.ids:
        id_list = [int(x) for x in args.ids.split(",") if x.strip()]
        pages = [(i, data[i]) for i in id_list if 0 <= i < len(data)]
    elif args.stratified > 0:
        from collections import defaultdict
        import random

        random.seed(7)
        by = defaultdict(list)
        for i, p in enumerate(data):
            src = p.get("page_info", {}).get("page_attribute", {}).get("data_source", "na")
            by[src].append(i)
        picked = []
        # round-robin until N
        keys = list(by.keys())
        ptr = {k: 0 for k in keys}
        for k in keys:
            random.shuffle(by[k])
        while len(picked) < args.stratified and any(ptr[k] < len(by[k]) for k in keys):
            for k in keys:
                if ptr[k] < len(by[k]):
                    picked.append(by[k][ptr[k]])
                    ptr[k] += 1
                    if len(picked) >= args.stratified:
                        break
        pages = [(i, data[i]) for i in picked]
        print(f"[data] stratified={len(pages)} sources={len(keys)}", flush=True)
    else:
        pages = list(enumerate(data))
        if args.limit > 0:
            pages = pages[: args.limit]

    ready = ensure_images(pages, download=True)
    if args.download_only:
        # also try bulk remaining
        if args.limit == 0 and not args.ids and args.stratified == 0:
            print("[data] downloading all missing images (this is large)...", flush=True)
            ensure_images(list(enumerate(data)), download=True)
        print("[data] download-only done")
        return 0

    if not ready:
        print("no images ready")
        return 2

    pred_unl = OUT_ROOT / "predictions_unlimited"
    pred_roe = OUT_ROOT / "predictions_roe_hard"
    pred_unl.mkdir(parents=True, exist_ok=True)
    pred_roe.mkdir(parents=True, exist_ok=True)
    manifest = OUT_ROOT / "manifest.jsonl"
    mf = open(manifest, "a", encoding="utf-8")

    tok = model = None
    if args.mode in ("unlimited", "roe_hard", "both"):
        tok, model = load_model()

    t0 = time.time()
    done = 0
    for i, page, img in ready:
        stem = stem_of(page["page_info"]["image_path"])
        tags = page_tags(page)
        rec = {"idx": i, "stem": stem, "img": str(img), "tags": tags}

        if args.mode in ("unlimited", "both"):
            out_md = pred_unl / f"{stem}.md"
            if args.skip_existing and out_md.is_file() and out_md.stat().st_size > 0:
                rec["unlimited"] = "skip"
            else:
                print(f"[{done+1}/{len(ready)}] unl {stem}", flush=True)
                t1 = time.time()
                txt = infer_unlimited(model, tok, img, OUT_ROOT / "work_unl" / stem)
                out_md.write_text(txt or "", encoding="utf-8")
                rec["unlimited_sec"] = time.time() - t1
                rec["unlimited"] = "ok"

        if args.mode in ("roe_hard", "both"):
            out_md = pred_roe / f"{stem}.md"
            if args.skip_existing and out_md.is_file() and out_md.stat().st_size > 0:
                rec["roe_hard"] = "skip"
            else:
                print(f"[{done+1}/{len(ready)}] roe {stem} tags={tags}", flush=True)
                t1 = time.time()
                # hard path only if tagged hard; else same as unlimited single for speed
                if tags:
                    txt = infer_roe_hard(model, tok, img, OUT_ROOT / "work_roe" / stem, tags)
                else:
                    txt = infer_unlimited(model, tok, img, OUT_ROOT / "work_roe" / stem)
                out_md.write_text(txt or "", encoding="utf-8")
                rec["roe_hard_sec"] = time.time() - t1
                rec["roe_hard"] = "ok"

        mf.write(json.dumps(rec) + "\n")
        mf.flush()
        done += 1

    mf.close()
    print(f"[done] pages={done} sec={time.time()-t0:.1f}")
    print(f"predictions_unlimited → {pred_unl}")
    print(f"predictions_roe_hard → {pred_roe}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
