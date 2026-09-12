"""Check exported runtime scores against every evaluated query, plus refusals."""
import os
os.environ["OPENBLAS_NUM_THREADS"] = "1"
os.environ["TOKENIZERS_PARALLELISM"] = "false"
import argparse
import json
import resource
import sys
import time
from pathlib import Path

import numpy as np

from retrieval import CACHE, ROOT, load_corpora
from run import EXTRAS, metrics, questions
from runtime import Retriever


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--terms", type=int, choices=[256, 1024], default=256)
    ap.add_argument("--extra", action="store_true", help="score the older independent 773-question control")
    args = ap.parse_args()
    retriever = Retriever(CACHE / f"runtime{args.terms}")
    fixture = ROOT / "benchmarks/vsa_routing_arena_20260911"
    names, _, _, tests, _ = load_corpora(fixture / "corpora.tsv")
    if args.extra:
        name = "questions_indep_mistral_20260912.tsv"
        texts, gold, valid = questions(fixture / name, names)
        scores = np.array([retriever.scores(t) for t in texts])
        q = retriever.native.encode(texts, True)
        baseline = retriever.native.dense(q, retriever.cent, retriever.owner, len(names), retriever.norms)
        result = {"preset": retriever.receipt["preset"], "source": name,
                  "candidate": metrics(scores, gold, valid, baseline), "v32": metrics(baseline, gold, valid),
                  "qwen_published": json.loads((fixture / "results_qwen3-embedding-4b.json").read_text())["questions_extra:" + name],
                  "scope": "Additional previously exposed, partly overlapping writer control; not fresh evaluation."}
        (CACHE / f"runtime{args.terms}_extra.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result), flush=True)
        return
    blocks = {"test_sentences": tests, "questions": questions(fixture / "questions.tsv", names)[0]}
    blocks.update({"questions_extra:" + n: questions(fixture / n, names)[0] for n in EXTRAS})
    expected = np.load(CACHE / f"semantic{args.terms}_passage.npz")
    count, times, top_equal = 0, [], 0
    for key, texts in blocks.items():
        reference = expected[key]
        for i, text in enumerate(texts):
            start = time.perf_counter_ns()
            actual = retriever.scores(text)
            ranked = np.argsort(-actual, kind="stable")[:20]
            times.append((time.perf_counter_ns() - start) / 1000)
            np.testing.assert_allclose(actual, reference[i], atol=2e-7, rtol=1e-6)
            top_equal += int(np.array_equal(ranked, np.argsort(-reference[i], kind="stable")[:20]))
            count += 1
    if top_equal != count:
        raise RuntimeError("exported top-20 differs from evaluated scores")
    if retriever.query("") or retriever.query("the and a"):
        raise RuntimeError("empty query did not refuse")
    if "torch" in sys.modules or "transformers" in sys.modules:
        raise RuntimeError("neural dependency entered query runtime")
    # Verify corruption refusal without touching the real cache.
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        temp = Path(td)
        receipt = retriever.receipt
        (temp / "receipt.json").write_text(json.dumps(receipt))
        (temp / "index.npz").write_bytes(b"corrupt")
        try:
            Retriever(temp)
        except ValueError as e:
            if "digest mismatch" not in str(e):
                raise
        else:
            raise RuntimeError("corrupt runtime cache accepted")
    output = {"preset": retriever.receipt["preset"], "queries_equal": count, "top20_equal": top_equal,
              "p50_us": float(np.median(times)), "p95_us": float(np.percentile(times, 95)),
              "runtime_data_bytes": retriever.receipt["runtime_data_bytes"],
              "verification_peak_rss_mib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024,
              "checks": ["all exported scores and top-20 match", "empty queries refused", "corrupt cache refused", "no torch/transformers imported"],
              "latency_scope": "scores plus top-20; excludes verification assertions, loading, calibration, execution"}
    if output["p95_us"] > 1000 or output["runtime_data_bytes"] > 64 * 2**20:
        raise RuntimeError("runtime exceeds the experiment budget")
    (CACHE / f"runtime{args.terms}_verification.json").write_text(json.dumps(output, indent=2) + "\n")
    print("CNET_VSA_RETRIEVAL_RUNTIME_PASS", json.dumps(output), flush=True)


if __name__ == "__main__":
    main()
