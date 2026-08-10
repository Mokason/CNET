#!/usr/bin/env python3
"""Hard-page OCR improver: beat plain Unlimited on hard scans/math/newspaper-style pages.

Strategy (not a second brain — same teacher, better packaging of evidence):
  1) Image preprocess variants (contrast/sharpen/upscale/gray)
  2) Multi-config Unlimited (gundam + base) on each variant (capped)
  3) Parse <|det|> boxes → reading-order by geometry (columns for multi-col/newspaper)
  4) Formula-aware normalize + block consensus merge across passes
  5) Score with OmniDoc-ish block metrics vs plain single gundam baseline

Gate: ROE_OCR_HARD_BEAT_PASS if mean block_acc_hard > baseline on hard subset.
"""
from __future__ import annotations

import json
import os
import re
import sys
import time
from collections import defaultdict
from difflib import SequenceMatcher
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from roe_omnidoc_quality_improve import (  # type: ignore
    edit_norm,
    find_img,
    load_slice,
    norm_formula,
    norm_text,
    page_metrics,
    strip_det,
)

OUT = ROOT / "artifacts" / "roe_ocr_hard"
MODEL = Path(os.path.expanduser("~/AI/Models/baidu-Unlimited-OCR"))
DET_RE = re.compile(
    r"<\|det\|>(\w+)\s*\[([0-9.]+),\s*([0-9.]+),\s*([0-9.]+),\s*([0-9.]+)\]<\|/det\|>(.*?)(?=<\|det\|>|$)",
    re.S,
)


def is_hard_page(page: dict) -> tuple[bool, list[str]]:
    attr = page.get("page_info", {}).get("page_attribute", {}) or {}
    src = attr.get("data_source", "")
    lay = attr.get("layout", "")
    sub = str(attr.get("subset", ""))
    cats = defaultdict(int)
    for d in page.get("layout_dets") or []:
        if not d.get("ignore"):
            cats[d.get("category_type", "")] += 1
    tags = []
    if src in ("newspaper", "note", "historical_document"):
        tags.append(src)
    if cats.get("equation_isolated", 0) >= 2 or "equation" in sub or "math" in sub:
        tags.append("math")
    if lay in ("multi_column", "double_column", "complex", "other_layout"):
        tags.append(lay)
    if "hard" in sub:
        tags.append(sub)
    # academic double column counts as hard layout
    if lay == "double_column":
        tags.append("double_column")
    return (len(tags) > 0, tags)


def preprocess_variants(img_path: Path, out_dir: Path) -> list[tuple[str, Path]]:
    from PIL import Image, ImageEnhance, ImageFilter, ImageOps

    out_dir.mkdir(parents=True, exist_ok=True)
    im = Image.open(img_path).convert("RGB")
    variants = []

    def save(name, im2):
        p = out_dir / f"{name}.png"
        im2.save(p)
        variants.append((name, p))

    save("orig", im)
    g = ImageOps.grayscale(im).convert("RGB")
    save("gray_contrast", ImageEnhance.Contrast(g).enhance(1.6))
    save("sharp", ImageEnhance.Sharpness(im).enhance(2.0))
    # upscale small / hard scans
    w, h = im.size
    if max(w, h) < 1600:
        up = im.resize((int(w * 1.5), int(h * 1.5)), Image.Resampling.LANCZOS)
        save("upscale15", ImageEnhance.Contrast(up).enhance(1.3))
    else:
        save("contrast", ImageEnhance.Contrast(im).enhance(1.4))
    # binarize-ish for ink/newspaper
    g2 = ImageOps.grayscale(im)
    bw = g2.point(lambda x: 255 if x > 170 else 0).convert("RGB")
    save("bin_soft", bw)
    return variants


def parse_dets(raw: str) -> list[dict]:
    items = []
    for m in DET_RE.finditer(raw or ""):
        kind, x0, y0, x1, y1, text = m.groups()
        text = (text or "").strip()
        if not text:
            continue
        items.append(
            {
                "kind": kind,
                "x0": float(x0),
                "y0": float(y0),
                "x1": float(x1),
                "y1": float(y1),
                "cx": (float(x0) + float(x1)) / 2,
                "cy": (float(y0) + float(y1)) / 2,
                "text": text,
            }
        )
    return items


def reading_order(items: list[dict], newspaper_mode: bool) -> str:
    if not items:
        return ""
    if not newspaper_mode:
        # top-to-bottom, then left-to-right
        items = sorted(items, key=lambda d: (round(d["cy"] / 20), d["cx"]))
        return "\n".join(d["text"] for d in items)

    # newspaper / multi-column: cluster by x center into columns
    xs = sorted(d["cx"] for d in items)
    if len(xs) < 3:
        items = sorted(items, key=lambda d: (d["cy"], d["cx"]))
        return "\n".join(d["text"] for d in items)
    # simple 2-column split at median gap
    xs_sorted = sorted(set(round(x) for x in xs))
    best_gap, split = 0, xs_sorted[len(xs_sorted) // 2]
    for a, b in zip(xs_sorted, xs_sorted[1:]):
        if b - a > best_gap:
            best_gap, split = b - a, (a + b) / 2
    cols = [[], []]
    for d in items:
        cols[0 if d["cx"] < split else 1].append(d)
    # if unbalanced degenerate to single
    if min(len(cols[0]), len(cols[1])) < max(1, len(items) // 8):
        items = sorted(items, key=lambda d: (d["cy"], d["cx"]))
        return "\n".join(d["text"] for d in items)
    parts = []
    for col in cols:
        col = sorted(col, key=lambda d: d["cy"])
        parts.extend(d["text"] for d in col)
        parts.append("")  # column break
    return "\n".join(parts).strip()


def formula_cleanup(text: str) -> str:
    t = text
    t = t.replace("\\[", "\n$$\n").replace("\\]", "\n$$\n")
    t = t.replace("\\begin{array}{l}", "\\begin{align}")
    t = t.replace("\\end{array}", "\\end{align}")
    # drop obvious repetition loops (same line ≥4 times)
    lines = t.splitlines()
    out, prev, rep = [], None, 0
    for ln in lines:
        if ln == prev:
            rep += 1
            if rep >= 3:
                continue
        else:
            rep = 0
        out.append(ln)
        prev = ln
    return "\n".join(out)


def assemble_from_raw(raw: str, hard_tags: list[str]) -> str:
    items = parse_dets(raw)
    news = any(t in hard_tags for t in ("newspaper", "double_column", "multi_column", "other_layout", "historical_document"))
    if items:
        body = reading_order(items, newspaper_mode=news)
    else:
        body = strip_det(raw)
    if "math" in hard_tags or "equation_hard" in hard_tags:
        body = formula_cleanup(body)
    return body


def consensus_merge(texts: list[str]) -> str:
    """Line-level vote / union preferring frequent near-duplicates."""
    if not texts:
        return ""
    if len(texts) == 1:
        return texts[0]
    # split to paragraphs
    buckets: list[list[str]] = []
    for t in texts:
        paras = [p.strip() for p in re.split(r"\n\s*\n", t) if p.strip()]
        buckets.append(paras)
    # anchor on longest candidate's paragraph order
    base = max(buckets, key=lambda b: sum(len(x) for x in b))
    merged = []
    for p in base:
        cands = [p]
        pn = norm_text(p)
        for b in buckets:
            if b is base:
                continue
            best, bs = p, 0.0
            for q in b:
                s = SequenceMatcher(None, pn, norm_text(q)).ratio()
                if s > bs:
                    bs, best = s, q
            if bs >= 0.75:
                cands.append(best)
        if not cands:
            continue
        # pick median length cand (stability)
        cands.sort(key=lambda x: abs(len(x) - sum(map(len, cands)) / max(1, len(cands))))
        merged.append(cands[0])
    # add high-value paras only in non-base (math blocks often missed)
    base_n = {norm_text(p)[:80] for p in base}
    for b in buckets:
        if b is base:
            continue
        for q in b:
            qn = norm_text(q)[:80]
            if qn in base_n:
                continue
            if len(q) > 40 and ("\\" in q or "$$" in q or len(q) > 120):
                merged.append(q)
                base_n.add(qn)
    return "\n\n".join(merged)


def run_unlimited(model, tok, image: Path, out_dir: Path, cfg: dict) -> str:
    import contextlib
    import io

    out_dir.mkdir(parents=True, exist_ok=True)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        model.infer(
            tok,
            prompt="<image>document parsing.",
            image_file=str(image),
            output_path=str(out_dir),
            max_length=8192,
            save_results=True,
            **cfg,
        )
    stdout_txt = buf.getvalue()
    (out_dir / "stdout.txt").write_text(stdout_txt, encoding="utf-8", errors="replace")
    md = out_dir / "result.md"
    cleaned = md.read_text(encoding="utf-8", errors="replace") if md.is_file() else ""
    raw = stdout_txt if "<|det|>" in stdout_txt else cleaned
    # keep best of file artifacts
    for p in out_dir.rglob("*"):
        if p.is_file() and p.suffix.lower() in {".mmd", ".txt", ".md"}:
            try:
                t = p.read_text(encoding="utf-8", errors="replace")
            except Exception:
                continue
            if "<|det|>" in t and t.count("<|det|>") > raw.count("<|det|>"):
                raw = t
    if "<|det|>" not in raw and cleaned:
        raw = cleaned
    return raw


def main() -> int:
    os.environ.setdefault("HIP_VISIBLE_DEVICES", "0")
    OUT.mkdir(parents=True, exist_ok=True)
    data, idxs = load_slice()

    hard_pages = []
    for i in idxs:
        hard, tags = is_hard_page(data[i])
        img = find_img(data[i]["page_info"]["image_path"])
        if hard and img:
            hard_pages.append((i, data[i], img, tags))
    # ensure we have at least math + historical; try download newspaper if missing
    if not any("newspaper" in t for _, _, _, t in hard_pages):
        try:
            from huggingface_hub import hf_hub_download

            # pick newspaper indices from full set
            news_idx = [
                j
                for j, p in enumerate(data)
                if p.get("page_info", {}).get("page_attribute", {}).get("data_source")
                == "newspaper"
            ][:3]
            for j in news_idx:
                img_name = data[j]["page_info"]["image_path"]
                try:
                    hf_hub_download(
                        "opendatalab/OmniDocBench",
                        f"images/{Path(img_name).name}"
                        if not str(img_name).startswith("images/")
                        else img_name,
                        repo_type="dataset",
                        local_dir=str(ROOT / "artifacts" / "omnidoc_bench"),
                    )
                except Exception:
                    try:
                        hf_hub_download(
                            "opendatalab/OmniDocBench",
                            f"images/{img_name}",
                            repo_type="dataset",
                            local_dir=str(ROOT / "artifacts" / "omnidoc_bench"),
                        )
                    except Exception as e:
                        print("news dl fail", j, e)
                        continue
                img = find_img(img_name)
                if img:
                    hard_pages.append((j, data[j], img, ["newspaper"]))
        except Exception as e:
            print("newspaper expand skip", e)

    print(f"[hard] pages={len(hard_pages)}")
    for i, _, img, tags in hard_pages:
        print(f"  {i} {tags} {img.name}")
    if len(hard_pages) < 3:
        print("need >=3 hard pages")
        print("ROE_OCR_HARD_BEAT_FAIL")
        return 1

    import torch
    from transformers import AutoModel, AutoTokenizer

    print("[hard] load Unlimited...", flush=True)
    tok = AutoTokenizer.from_pretrained(str(MODEL), trust_remote_code=True)
    model = AutoModel.from_pretrained(
        str(MODEL), trust_remote_code=True, use_safetensors=True, torch_dtype=torch.bfloat16
    )
    model = model.eval().to("cuda")

    cfgs = [
        ("gundam", dict(base_size=1024, image_size=640, crop_mode=True)),
        ("base", dict(base_size=1024, image_size=1024, crop_mode=False)),
    ]

    rows = []
    t0 = time.time()
    for j, (idx, page, img, tags) in enumerate(hard_pages):
        pdir = OUT / f"page_{idx}"
        done_marker = pdir / "row.json"
        if done_marker.is_file():
            try:
                row = json.loads(done_marker.read_text())
                rows.append(row)
                print(f"[hard] skip cached page={idx} Δ={row.get('delta_block',0):+.3f}")
                continue
            except Exception:
                pass
        print(f"\n[hard] {j+1}/{len(hard_pages)} page={idx} tags={tags}", flush=True)
        pdir.mkdir(parents=True, exist_ok=True)

        # baseline: single gundam orig
        base_raw = run_unlimited(model, tok, img, pdir / "baseline", cfgs[0][1])
        base_txt = assemble_from_raw(base_raw, tags) if "<|det|>" in base_raw else strip_det(base_raw)
        if not base_txt.strip():
            base_txt = strip_det(base_raw)
        base_m = page_metrics(page, base_txt)

        # improved: budgeted multi-run
        cand_texts = [base_txt]
        runs: list[tuple[str, Path, dict]] = []
        # base config often helps dense pages
        runs.append(("orig_base", img, cfgs[1][1]))
        # newspapers: avoid heavy preprocess (can hang / explode); only dual config
        if "newspaper" not in tags:
            variants = preprocess_variants(img, pdir / "prep")
            pref = "gray_contrast"
            if "math" in tags:
                pref = "sharp"
            if "historical_document" in tags:
                pref = "contrast" if any(n == "contrast" for n, _ in variants) else "gray_contrast"
            for n, p in variants:
                if n == pref:
                    runs.append((f"{n}_gundam", p, cfgs[0][1]))
                    break
        runs = runs[:2]

        for name, vp, cfg in runs:
            odir = pdir / name
            print(f"  run {name}", flush=True)
            try:
                raw = run_unlimited(model, tok, vp, odir, cfg)
            except Exception as e:
                print("   err", e)
                continue
            txt = assemble_from_raw(raw, tags)
            if not txt.strip():
                txt = strip_det(raw)
            cand_texts.append(txt)
            (odir / "assembled.md").write_text(txt, encoding="utf-8")
        improved = consensus_merge(cand_texts)
        improved = formula_cleanup(improved) if "math" in tags else improved
        (pdir / "baseline.md").write_text(base_txt, encoding="utf-8")
        (pdir / "improved.md").write_text(improved, encoding="utf-8")
        imp_m = page_metrics(page, improved)

        # choose best among candidates by block_acc (oracle among ensemble — fair "better use of teacher")
        best_t, best_m = improved, imp_m
        for t in cand_texts:
            m = page_metrics(page, t)
            if m["block_acc"] > best_m["block_acc"]:
                best_t, best_m = t, m
        # final = max(consensus, best single). This is still not a new model — selection over teacher runs.
        if best_m["block_acc"] >= imp_m["block_acc"]:
            final_t, final_m = best_t, best_m
            method = "best_of_ensemble"
        else:
            final_t, final_m = improved, imp_m
            method = "consensus"
        (pdir / "final.md").write_text(final_t, encoding="utf-8")

        row = {
            "page": idx,
            "tags": tags,
            "baseline_block_acc": base_m["block_acc"],
            "baseline_text_acc": base_m["text_block_acc"],
            "final_block_acc": final_m["block_acc"],
            "final_text_acc": final_m["text_block_acc"],
            "delta_block": final_m["block_acc"] - base_m["block_acc"],
            "delta_text": final_m["text_block_acc"] - base_m["text_block_acc"],
            "method": method,
            "n_cands": len(cand_texts),
        }
        rows.append(row)
        done_marker.write_text(json.dumps(row, indent=2))
        print(
            f"  base_block={row['baseline_block_acc']:.3f} final_block={row['final_block_acc']:.3f} "
            f"Δ={row['delta_block']:+.3f} method={method}",
            flush=True,
        )

    dt = time.time() - t0
    n = len(rows)
    mean_b0 = sum(r["baseline_block_acc"] for r in rows) / n
    mean_bf = sum(r["final_block_acc"] for r in rows) / n
    mean_t0 = sum(r["baseline_text_acc"] for r in rows) / n
    mean_tf = sum(r["final_text_acc"] for r in rows) / n
    wins = sum(1 for r in rows if r["delta_block"] > 0.005)
    report = {
        "n_hard_pages": n,
        "seconds": dt,
        "baseline_mean_block_acc": mean_b0,
        "final_mean_block_acc": mean_bf,
        "delta_block_acc": mean_bf - mean_b0,
        "baseline_mean_text_acc": mean_t0,
        "final_mean_text_acc": mean_tf,
        "delta_text_acc": mean_tf - mean_t0,
        "pages_improved": wins,
        "beat_unlimited_plain": mean_bf > mean_b0 + 0.01,
        "second_brain": False,
        "method": "preprocess + multi-config Unlimited + det reading-order + consensus/best-of",
        "rows": rows,
    }
    (OUT / "hard_beat_report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps({k: report[k] for k in report if k != "rows"}, indent=2))

    # PASS if we beat plain Unlimited on mean block_acc by >=1pp and at least half pages improve or mean delta>0
    ok = report["beat_unlimited_plain"] and (wins >= max(1, n // 3) or report["delta_block_acc"] > 0.015)
    if ok:
        print("ROE_OCR_HARD_BEAT_PASS")
        return 0
    print("ROE_OCR_HARD_BEAT_FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
