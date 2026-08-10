#!/usr/bin/env python3
"""Watch dual-GPU OmniDoc infer; when preds stabilize near 1651, run official eval."""
from __future__ import annotations

import json
import os
import subprocess
import time
from pathlib import Path

ROOT = Path("/home/marble/AI/CNET")
PRED = ROOT / "artifacts/omnidoc_full/predictions_unlimited"
GT_N = 1651
STATUS = ROOT / "artifacts/omnidoc_full/sota_watch_status.json"
LOG = ROOT / "logs/roe_omnidoc_sota_watch.log"


def log(msg: str) -> None:
    line = time.strftime("%H:%M:%S") + " " + msg
    print(line, flush=True)
    with open(LOG, "a") as f:
        f.write(line + "\n")


def count_preds() -> int:
    if not PRED.is_dir():
        return 0
    return sum(1 for p in PRED.glob("*.md") if p.stat().st_size > 10)


def workers_alive() -> bool:
    r = subprocess.run(["pgrep", "-f", "roe_omnidoc_dual_gpu_infer.py --worker"], capture_output=True)
    return r.returncode == 0


def run_official() -> dict:
    env = os.environ.copy()
    env["PATH"] = "/usr/local/bin:" + env.get("PATH", "")
    env["CDM_SAVE_VIS"] = "0"
    env["PYTHONPATH"] = str(ROOT / "third_party/OmniDocBench")
    cfg = ROOT / "artifacts/omnidoc_full/end2end_unlimited_full_cdm.yaml"
    out_log = ROOT / "artifacts/omnidoc_full/official_full_eval.log"
    log(f"starting official eval → {out_log}")
    p = subprocess.run(
        [str(ROOT / ".venv-unlimited-ocr/bin/python"), str(ROOT / "third_party/OmniDocBench/pdf_validation.py"), "--config", str(cfg)],
        cwd=str(ROOT),
        env=env,
        capture_output=True,
        text=True,
        timeout=7200,
    )
    out_log.write_text((p.stdout or "") + "\n" + (p.stderr or ""))
    metric = ROOT / "result/predictions_unlimited_quick_match_metric_result.json"
    rep = {"returncode": p.returncode, "metric_exists": metric.is_file()}
    if metric.is_file():
        m = json.loads(metric.read_text())
        text_ed = m["text_block"]["all"]["Edit_dist"]["ALL_page_avg"]
        form_ed = m["display_formula"]["all"]["Edit_dist"]["ALL_page_avg"]
        teds = m["table"]["all"]["TEDS"]["all"]
        text_s = (1 - text_ed) * 100
        form_s = (1 - form_ed) * 100
        teds_s = teds * 100
        rep.update(
            {
                "n_preds": count_preds(),
                "text_score": text_s,
                "table_TEDS_100": teds_s,
                "formula_edit_proxy": form_s,
                "Overall_proxy_no_CDM": (text_s + teds_s + form_s) / 3,
                "CDM": "UNAVAILABLE",
                "sota_claim": "WITHHELD_NO_CDM_OR_FULL_PROTOCOL",
            }
        )
        (ROOT / "artifacts/omnidoc_full/OFFICIAL_FULL_REPORT.json").write_text(json.dumps(rep, indent=2))
    return rep


def main() -> int:
    LOG.parent.mkdir(parents=True, exist_ok=True)
    stable = 0
    last = -1
    log("watch start")
    for tick in range(600):  # up to ~10h at 60s
        n = count_preds()
        alive = workers_alive()
        STATUS.write_text(json.dumps({"preds": n, "workers_alive": alive, "t": time.time()}, indent=2))
        if n != last:
            log(f"preds={n}/~{GT_N} workers_alive={alive}")
            last = n
            stable = 0
        else:
            stable += 1
        # done if enough preds and workers finished, or preds==1651 and stable
        if n >= GT_N - 5 and (not alive or stable >= 3):
            log(f"infer complete-ish preds={n}")
            try:
                rep = run_official()
                log("official: " + json.dumps(rep))
            except Exception as e:
                log(f"official failed: {e}")
            print("ROE_OMNIDOC_SOTA_WATCH_DONE")
            return 0
        if not alive and n > 40 and stable >= 5:
            log(f"workers dead early preds={n} — run partial official")
            try:
                # rewrite subset config dynamically
                data = json.loads((ROOT / "artifacts/omnidoc_bench/OmniDocBench.json").read_text())
                stems = {p.stem for p in PRED.glob("*.md")}
                filt = [p for p in data if Path(p["page_info"]["image_path"]).stem in stems]
                sub = ROOT / "artifacts/omnidoc_full/OmniDocBench_partial.json"
                sub.write_text(json.dumps(filt, ensure_ascii=False))
                cfg = (ROOT / "artifacts/omnidoc_full/end2end_unlimited_full_cdm.yaml").read_text()
                cfg2 = cfg.replace(
                    "OmniDocBench.json",
                    "OmniDocBench_partial.json",
                ).replace(
                    "artifacts/omnidoc_bench/OmniDocBench_partial.json",
                    "artifacts/omnidoc_full/OmniDocBench_partial.json",
                )
                # force subset path
                cfg2 = """end2end_eval:
  metrics:
    text_block:
      metric: [Edit_dist]
    display_formula:
      metric: [Edit_dist]
      cdm_workers: 8
    table:
      metric: [TEDS, Edit_dist]
      teds_workers: 8
    reading_order:
      metric: [Edit_dist]
  dataset:
    dataset_name: end2end_dataset
    ground_truth:
      data_path: /home/marble/AI/CNET/artifacts/omnidoc_full/OmniDocBench_partial.json
    prediction:
      data_path: /home/marble/AI/CNET/artifacts/omnidoc_full/predictions_unlimited
    match_method: quick_match
    match_workers: 8
    quick_match_polygon_timeout_sec: 300
    match_timeout_sec: 420
    timeout_fallback_max_chunk_span: 10
    timeout_fallback_order_penalty: 0.10
"""
                (ROOT / "artifacts/omnidoc_full/end2end_unlimited_partial.yaml").write_text(cfg2)
                env = os.environ.copy()
                env["PYTHONPATH"] = str(ROOT / "third_party/OmniDocBench")
                subprocess.run(
                    [
                        str(ROOT / ".venv-unlimited-ocr/bin/python"),
                        str(ROOT / "third_party/OmniDocBench/pdf_validation.py"),
                        "--config",
                        str(ROOT / "artifacts/omnidoc_full/end2end_unlimited_partial.yaml"),
                    ],
                    cwd=str(ROOT),
                    env=env,
                    timeout=7200,
                )
            except Exception as e:
                log(str(e))
            print("ROE_OMNIDOC_SOTA_WATCH_PARTIAL")
            return 0
        time.sleep(60)
    log("timeout watch")
    return 1


if __name__ == "__main__":
    from pathlib import Path
    raise SystemExit(main())
