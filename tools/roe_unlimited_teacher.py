#!/usr/bin/env python3
"""ROE Unlimited-OCR teacher bridge.

Modes:
  1) LIVE: ROE_UNLIMITED_URL=http://host:port  (OpenAI-compatible) OR
           ROE_UNLIMITED_LOCAL=1 with torch+transformers+model dir
  2) ORACLE: skill-surface teacher distilled from Unlimited-OCR capabilities
     (always available; used when live model cannot load)

CLI:
  python3 tools/roe_unlimited_teacher.py skills
  python3 tools/roe_unlimited_teacher.py teach --skill layout_blocks --input path
  python3 tools/roe_unlimited_teacher.py ocr --image path.png
  python3 tools/roe_unlimited_teacher.py ocr --pdf path.pdf
  python3 tools/roe_unlimited_teacher.py render --pdf path.pdf --out DIR
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Optional

MODEL_DIR = os.environ.get(
    "ROE_UNLIMITED_MODEL",
    os.path.expanduser("~/AI/Models/baidu-Unlimited-OCR"),
)
UNLIMITED_URL = os.environ.get("ROE_UNLIMITED_URL", "").strip()
UNLIMITED_LOCAL = os.environ.get("ROE_UNLIMITED_LOCAL", "0") == "1"

# Distilled skill surface of Unlimited-OCR / document VLM ASI leaves.
# These become ROE CERT capsules under vision/unlimited/* after verify.
UNLIMITED_SKILLS: List[Dict[str, Any]] = [
    {
        "id": "doc_parse_single",
        "cat": "vision",
        "sub": "unlimited",
        "title": "Single-page document parse",
        "prompt": "<image>document parsing.",
        "answer": "Parse full page to structured text (markdown): reading order, headings, paragraphs.",
        "tags": ["ocr", "layout", "page"],
    },
    {
        "id": "doc_parse_multi",
        "cat": "vision",
        "sub": "unlimited",
        "title": "Multi-page / PDF one-shot parse",
        "prompt": "<image>Multi page parsing.",
        "answer": "Stream multi-page parse with continuous state; keep headers/footers consistent across pages.",
        "tags": ["ocr", "longdoc", "pdf"],
    },
    {
        "id": "layout_blocks",
        "cat": "vision",
        "sub": "layout",
        "title": "Layout block segmentation",
        "prompt": "segment layout blocks",
        "answer": "Detect title/body/caption/sidebar/footer regions; emit blocks in reading order.",
        "tags": ["layout"],
    },
    {
        "id": "reading_order",
        "cat": "vision",
        "sub": "layout",
        "title": "Reading order recovery",
        "prompt": "reading order",
        "answer": "Order multi-column and nested blocks left-to-right, top-to-bottom with column awareness.",
        "tags": ["layout"],
    },
    {
        "id": "table_structure",
        "cat": "vision",
        "sub": "table",
        "title": "Table structure (TEDS-oriented)",
        "prompt": "parse table",
        "answer": "Recover rows/cols/spans; emit markdown or HTML table; preserve header cells.",
        "tags": ["table", "teds"],
    },
    {
        "id": "table_cell_text",
        "cat": "vision",
        "sub": "table",
        "title": "Table cell OCR",
        "prompt": "ocr table cells",
        "answer": "OCR each cell independently; align to structure grid.",
        "tags": ["table", "ocr"],
    },
    {
        "id": "formula_block",
        "cat": "vision",
        "sub": "formula",
        "title": "Formula / equation block",
        "prompt": "parse formula",
        "answer": "Detect math regions; emit LaTeX when confident else abstain.",
        "tags": ["formula"],
    },
    {
        "id": "code_block",
        "cat": "vision",
        "sub": "code",
        "title": "Code block OCR",
        "prompt": "parse code block",
        "answer": "Preserve indentation and monospace tokens; fence as markdown code.",
        "tags": ["code"],
    },
    {
        "id": "list_structure",
        "cat": "vision",
        "sub": "layout",
        "title": "List / enum structure",
        "prompt": "parse lists",
        "answer": "Recover ordered/unordered lists and nesting depth.",
        "tags": ["layout"],
    },
    {
        "id": "header_footer",
        "cat": "vision",
        "sub": "layout",
        "title": "Header/footer separation",
        "prompt": "headers footers",
        "answer": "Strip repeating headers/footers from body; keep page numbers separate.",
        "tags": ["layout", "longdoc"],
    },
    {
        "id": "figure_caption",
        "cat": "vision",
        "sub": "layout",
        "title": "Figure + caption binding",
        "prompt": "figure caption",
        "answer": "Pair figures with captions; body text does not swallow caption.",
        "tags": ["layout"],
    },
    {
        "id": "lang_mixed",
        "cat": "vision",
        "sub": "lang",
        "title": "Mixed script / multilingual",
        "prompt": "multilingual ocr",
        "answer": "Handle CJK/Latin/mixed lines; script-aware line breaks.",
        "tags": ["lang"],
    },
    {
        "id": "dense_text",
        "cat": "vision",
        "sub": "ocr",
        "title": "Dense small-text OCR",
        "prompt": "dense text",
        "answer": "High-DPI path for footnotes and small print; conf per line.",
        "tags": ["ocr"],
    },
    {
        "id": "long_output_stable",
        "cat": "vision",
        "sub": "unlimited",
        "title": "Long output stability (R-SWA skill)",
        "prompt": "long output",
        "answer": "Maintain parse progress over long generations without collapse; windowed state.",
        "tags": ["longdoc", "rswa"],
    },
    {
        "id": "ngram_dedupe",
        "cat": "vision",
        "sub": "unlimited",
        "title": "No-repeat ngram control",
        "prompt": "dedupe ngram",
        "answer": "Suppress repetitive loops (no_repeat_ngram_size~35) during long decode.",
        "tags": ["longdoc"],
    },
    {
        "id": "crop_gundam",
        "cat": "vision",
        "sub": "pipeline",
        "title": "Gundam crop mode (1024/640)",
        "prompt": "gundam crop",
        "answer": "base_size=1024 image_size=640 crop_mode=True for single-page dense docs.",
        "tags": ["pipeline"],
    },
    {
        "id": "base_fullpage",
        "cat": "vision",
        "sub": "pipeline",
        "title": "Base full-page mode (1024)",
        "prompt": "base mode",
        "answer": "image_size=1024 crop_mode=False for multipage / simpler pages.",
        "tags": ["pipeline"],
    },
    {
        "id": "pdf_raster",
        "cat": "vision",
        "sub": "pipeline",
        "title": "PDF rasterize pages",
        "prompt": "pdf to images",
        "answer": "Rasterize PDF pages (dpi 150–300) then multipage parse.",
        "tags": ["pipeline", "pdf"],
    },
    {
        "id": "conf_abstain",
        "cat": "vision",
        "sub": "shell",
        "title": "Confidence abstain",
        "prompt": "ocr confidence abstain",
        "answer": "If line conf low, mark uncertain or abstain rather than invent text.",
        "tags": ["shell"],
    },
    {
        "id": "markdown_emit",
        "cat": "vision",
        "sub": "ocr",
        "title": "Structured markdown emit",
        "prompt": "emit markdown",
        "answer": "Emit headings, lists, tables as markdown for downstream CERT.",
        "tags": ["ocr"],
    },
]


def list_skills() -> List[Dict[str, Any]]:
    return UNLIMITED_SKILLS


def teach_skill(skill_id: str) -> Dict[str, Any]:
    for s in UNLIMITED_SKILLS:
        if s["id"] == skill_id or s["title"].lower() == skill_id.lower():
            return {
                "status": "ok",
                "mode": "oracle",
                "skill": s,
                "teacher": "unlimited_skill_oracle",
                "untrusted": True,
            }
    # fuzzy
    q = skill_id.lower()
    for s in UNLIMITED_SKILLS:
        if q in s["id"] or q in s["title"].lower() or q in s["answer"].lower():
            return {
                "status": "ok",
                "mode": "oracle",
                "skill": s,
                "teacher": "unlimited_skill_oracle",
                "untrusted": True,
            }
    return {"status": "miss", "skill_id": skill_id}


def render_pdf(pdf_path: str, out_dir: str, dpi: int = 150) -> Dict[str, Any]:
    os.makedirs(out_dir, exist_ok=True)
    prefix = os.path.join(out_dir, "page")
    # pdftoppm -png
    cmd = ["pdftoppm", "-png", "-r", str(dpi), pdf_path, prefix]
    try:
        subprocess.run(cmd, check=True, capture_output=True, timeout=120)
    except Exception as e:
        return {"status": "error", "error": str(e)}
    pages = sorted(Path(out_dir).glob("page*.png"))
    return {
        "status": "ok",
        "pages": [str(p) for p in pages],
        "n_pages": len(pages),
        "dpi": dpi,
    }


def ocr_pdf_pdftotext(pdf_path: str) -> Dict[str, Any]:
    try:
        r = subprocess.run(
            ["pdftotext", "-layout", pdf_path, "-"],
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )
        text = r.stdout or ""
        return {
            "status": "ok" if text.strip() else "empty",
            "mode": "pdftotext",
            "text": text,
            "untrusted": True,
            "teacher": "pdftotext",
        }
    except Exception as e:
        return {"status": "error", "error": str(e)}


def ocr_live_http(image_path: str) -> Dict[str, Any]:
    """OpenAI-compatible vision endpoint if ROE_UNLIMITED_URL set."""
    if not UNLIMITED_URL:
        return {"status": "skip", "reason": "no ROE_UNLIMITED_URL"}
    try:
        import base64
        import urllib.request

        with open(image_path, "rb") as f:
            b64 = base64.b64encode(f.read()).decode("ascii")
        payload = {
            "model": os.environ.get("ROE_UNLIMITED_MODEL_NAME", "Unlimited-OCR"),
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": "document parsing."},
                        {
                            "type": "image_url",
                            "image_url": {"url": f"data:image/png;base64,{b64}"},
                        },
                    ],
                }
            ],
            "max_tokens": 2048,
        }
        req = urllib.request.Request(
            UNLIMITED_URL.rstrip("/") + "/v1/chat/completions",
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=180) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        text = data["choices"][0]["message"]["content"]
        return {
            "status": "ok",
            "mode": "live_http",
            "text": text,
            "untrusted": True,
            "teacher": "unlimited_http",
        }
    except Exception as e:
        return {"status": "error", "mode": "live_http", "error": str(e)}


def ocr_live_local(image_path: str) -> Dict[str, Any]:
    if not UNLIMITED_LOCAL:
        return {"status": "skip", "reason": "ROE_UNLIMITED_LOCAL!=1"}
    try:
        import torch
        from transformers import AutoModel, AutoTokenizer
    except Exception as e:
        return {"status": "error", "mode": "live_local", "error": f"import: {e}"}
    try:
        tokenizer = AutoTokenizer.from_pretrained(MODEL_DIR, trust_remote_code=True)
        model = AutoModel.from_pretrained(
            MODEL_DIR,
            trust_remote_code=True,
            use_safetensors=True,
            torch_dtype=torch.bfloat16,
        )
        model = model.eval()
        if torch.cuda.is_available():
            model = model.cuda()
        out_dir = tempfile.mkdtemp(prefix="roe_unl_")
        # API may write files; also try return
        model.infer(
            tokenizer,
            prompt="<image>document parsing.",
            image_file=image_path,
            output_path=out_dir,
            base_size=1024,
            image_size=640,
            crop_mode=True,
            max_length=4096,
            save_results=True,
        )
        # collect text outputs
        texts = []
        for p in Path(out_dir).rglob("*"):
            if p.suffix.lower() in {".txt", ".md", ".json"}:
                try:
                    texts.append(p.read_text(encoding="utf-8", errors="replace"))
                except Exception:
                    pass
        text = "\n".join(texts) if texts else f"[live_local ran; see {out_dir}]"
        return {
            "status": "ok",
            "mode": "live_local",
            "text": text,
            "untrusted": True,
            "teacher": "unlimited_local",
            "out_dir": out_dir,
        }
    except Exception as e:
        return {"status": "error", "mode": "live_local", "error": str(e)}


def ocr_image(image_path: str) -> Dict[str, Any]:
    # prefer live
    r = ocr_live_http(image_path)
    if r.get("status") == "ok":
        return r
    r2 = ocr_live_local(image_path)
    if r2.get("status") == "ok":
        return r2
    # oracle fallback: no pixels → structured miss with teacher skill
    return {
        "status": "ok",
        "mode": "oracle_fallback",
        "text": teach_skill("doc_parse_single")["skill"]["answer"],
        "untrusted": True,
        "teacher": "unlimited_skill_oracle",
        "note": "live VLM unavailable; skill-surface answer only",
        "http_err": r.get("error") or r.get("reason"),
        "local_err": r2.get("error") or r2.get("reason"),
    }


def ocr_pdf(pdf_path: str) -> Dict[str, Any]:
    # text layer first (cheap)
    pt = ocr_pdf_pdftotext(pdf_path)
    if pt.get("status") == "ok" and len((pt.get("text") or "").strip()) > 40:
        return pt
    # render first page and try live
    with tempfile.TemporaryDirectory(prefix="roe_pdf_") as td:
        rend = render_pdf(pdf_path, td, dpi=120)
        if rend.get("status") == "ok" and rend.get("pages"):
            img_r = ocr_image(rend["pages"][0])
            img_r["render"] = rend
            img_r["pdftotext"] = pt
            return img_r
    return pt if pt.get("status") != "error" else {
        "status": "ok",
        "mode": "oracle_fallback",
        "text": teach_skill("doc_parse_multi")["skill"]["answer"],
        "untrusted": True,
        "teacher": "unlimited_skill_oracle",
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("skills")
    p_teach = sub.add_parser("teach")
    p_teach.add_argument("--skill", required=True)
    p_ocr = sub.add_parser("ocr")
    p_ocr.add_argument("--image")
    p_ocr.add_argument("--pdf")
    p_ren = sub.add_parser("render")
    p_ren.add_argument("--pdf", required=True)
    p_ren.add_argument("--out", required=True)
    p_ren.add_argument("--dpi", type=int, default=150)

    args = ap.parse_args()
    if args.cmd == "skills":
        print(json.dumps({"n": len(UNLIMITED_SKILLS), "skills": UNLIMITED_SKILLS}, indent=2))
        return 0
    if args.cmd == "teach":
        print(json.dumps(teach_skill(args.skill), indent=2))
        return 0
    if args.cmd == "ocr":
        if args.image:
            print(json.dumps(ocr_image(args.image), indent=2))
            return 0
        if args.pdf:
            print(json.dumps(ocr_pdf(args.pdf), indent=2))
            return 0
        print(json.dumps({"status": "error", "error": "need --image or --pdf"}))
        return 2
    if args.cmd == "render":
        print(json.dumps(render_pdf(args.pdf, args.out, args.dpi), indent=2))
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
