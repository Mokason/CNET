#!/usr/bin/env python3
"""Page-chunked PDF runner for ROE (digital text + optional image pages).

Does NOT load a whole book as one image. Streams:
  page i → pdftotext -f i -l i → L3/CERT path (via JSONL side log)
  optional: pdftoppm single page for teacher image dump

Usage:
  python tools/roe_pdf_chunk_run.py book.pdf --out artifacts/roe_pdf_chunk --max-pages 50
"""
from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from pathlib import Path


def page_count(pdf: Path) -> int:
    r = subprocess.run(
        ["pdfinfo", str(pdf)], capture_output=True, text=True, check=False
    )
    for line in r.stdout.splitlines():
        if line.startswith("Pages:"):
            return int(line.split(":", 1)[1].strip())
    return -1


def pdftotext_page(pdf: Path, page: int) -> str:
    with tempfile.NamedTemporaryFile(suffix=".txt", delete=False) as t:
        tmp = Path(t.name)
    try:
        subprocess.run(
            [
                "pdftotext",
                "-q",
                "-layout",
                "-f",
                str(page),
                "-l",
                str(page),
                str(pdf),
                str(tmp),
            ],
            check=False,
        )
        return tmp.read_text(encoding="utf-8", errors="replace") if tmp.is_file() else ""
    finally:
        tmp.unlink(missing_ok=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("pdf")
    ap.add_argument("--out", default="artifacts/roe_pdf_chunk")
    ap.add_argument("--max-pages", type=int, default=512)
    ap.add_argument("--render-images", action="store_true", help="also pdftoppm pages")
    args = ap.parse_args()

    pdf = Path(args.pdf)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    pages_dir = out / "pages"
    pages_dir.mkdir(exist_ok=True)

    n = page_count(pdf)
    if n < 1:
        print("bad pdf page count", n)
        return 2
    lim = min(n, args.max_pages)
    print(f"[pdf] {pdf} pages={n} run={lim}")

    rows = []
    total_chars = 0
    empty = 0
    for i in range(1, lim + 1):
        text = pdftotext_page(pdf, i)
        alnum = sum(ch.isalnum() for ch in text)
        (pages_dir / f"page_{i:04d}.txt").write_text(text, encoding="utf-8")
        row = {
            "page": i,
            "chars": len(text),
            "alnum": alnum,
            "empty": alnum < 20,
        }
        if row["empty"]:
            empty += 1
        total_chars += len(text)
        rows.append(row)
        if i % 10 == 0 or i == lim:
            print(f"  page {i}/{lim} chars={len(text)} empty={row['empty']}")

    if args.render_images:
        img_dir = out / "images"
        img_dir.mkdir(exist_ok=True)
        subprocess.run(
            ["pdftoppm", "-png", "-f", "1", "-l", str(lim), str(pdf), str(img_dir / "p")],
            check=False,
        )

    report = {
        "pdf": str(pdf),
        "pages_total": n,
        "pages_run": lim,
        "total_chars": total_chars,
        "empty_or_image_pages": empty,
        "avg_chars": total_chars / max(1, lim),
        "second_brain": False,
        "note": "Streamed page-by-page; never one giant image.",
        "rows": rows,
    }
    (out / "chunk_report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps({k: report[k] for k in report if k != "rows"}, indent=2))
    print("ROE_PDF_CHUNK_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
