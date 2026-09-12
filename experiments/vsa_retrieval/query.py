"""Print experimental retrieval candidates. This command never executes them."""
import os
os.environ["OPENBLAS_NUM_THREADS"] = "1"
os.environ["TOKENIZERS_PARALLELISM"] = "false"
import argparse
import json
import resource
import sys
import time
from pathlib import Path

from retrieval import CACHE
from runtime import Retriever


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("query", nargs="?")
    ap.add_argument("--index", type=Path, default=CACHE / "runtime256")
    ap.add_argument("--k", type=int, default=5)
    args = ap.parse_args()
    retriever = Retriever(args.index)
    if "torch" in sys.modules or "transformers" in sys.modules:
        raise RuntimeError("neural model imported in query runtime")
    inputs = [args.query] if args.query is not None else sys.stdin
    for text in inputs:
        start = time.perf_counter_ns()
        candidates = retriever.query(text.rstrip("\n"), args.k)
        elapsed_us = (time.perf_counter_ns() - start) / 1000
        print(json.dumps({"preset": retriever.receipt["preset"], "certified": False,
                          "candidates": candidates, "query_us": elapsed_us,
                          "runtime_data_mib": retriever.receipt["runtime_data_bytes"] / 2**20,
                          "process_peak_rss_mib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024}), flush=True)


if __name__ == "__main__":
    main()
