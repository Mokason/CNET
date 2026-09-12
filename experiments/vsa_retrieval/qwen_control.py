"""Same prototype construction for Qwen; only raw-cache-supported slices."""
import os
os.environ["OPENBLAS_NUM_THREADS"] = "1"
import json
import time

import numpy as np

from retrieval import CACHE, ROOT, load_corpora, prototypes
from run import metrics, questions, sha256


def main():
    fixture = ROOT / "benchmarks/vsa_routing_arena_20260911"
    names, train, owners, tests, gold = load_corpora(fixture / "corpora.tsv")
    cache = ROOT / "var/arena_cache/qwen3-embedding-4b"
    docs = np.load(cache / "docs.f16.npy").astype(np.float32)
    if len(docs) != len(train):
        raise ValueError("Qwen document cache mismatch")
    blocks = {"test_sentences": (np.load(cache / "tests.f16.npy").astype(np.float32), gold),
              "questions": (np.load(cache / "questions.f16.npy").astype(np.float32), questions(fixture / "questions.tsv", names)[1])}
    report = {"results": {}, "cache_sha256": {n: sha256(cache / n) for n in ("docs.f16.npy", "tests.f16.npy", "questions.f16.npy")},
              "limitations": "Extra sets have centroid similarities only, not reusable query embeddings. Float16 raw-cache reconstruction; stable tie breaking. No online encoder latency claim."}
    base = {}
    for k in (1, 4, 8):
        start = time.perf_counter()
        p, _ = prototypes(docs, owners, len(names), k)
        row = {}
        for key, (q, gold) in blocks.items():
            scores = np.empty((len(q), len(names)), np.float32)
            for i in range(0, len(q), 64):
                sim = q[i:i+64] @ p.T
                scores[i:i+64] = sim.reshape(len(sim), len(names), k).max(axis=2)
            row[key] = metrics(scores, gold, [{int(g)} for g in gold])
            if k == 1:
                base[key] = scores
            else:
                row[key + "_mix"] = metrics((scores + base[key]) * 0.5, gold, [{int(g)} for g in gold])
        report["results"][f"prototype{k}"] = row
        print(k, json.dumps(row), f"elapsed {time.perf_counter()-start:.2f}s", flush=True)
    (CACHE / "qwen_control.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
