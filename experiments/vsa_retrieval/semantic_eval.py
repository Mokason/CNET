"""Score compiled semantic indexes. No torch, model weights, or GPU at runtime."""
import os
os.environ["OPENBLAS_NUM_THREADS"] = "1"
os.environ["TOKENIZERS_PARALLELISM"] = "false"
import json
import time

import numpy as np
from tokenizers import Tokenizer

from retrieval import CACHE, ROOT, Native, PassageIndex, SemanticChunks, SparseIndex, load_corpora, prototypes, reciprocal_rank_fusion
from run import EXTRAS, latency, metrics, questions, sha256


def main():
    fixture = ROOT / "benchmarks/vsa_routing_arena_20260911"
    names, train, owners, test, test_gold = load_corpora(fixture / "corpora.tsv")
    nc = len(names)
    build = json.loads((CACHE / "semantic_build.json").read_text())
    if build["names"] != names or build["source_sha256"] != sha256(fixture / "corpora.tsv"):
        raise ValueError("semantic source mismatch")
    weights = np.load(CACHE / "semantic_cap_weights.f16.npy").astype(np.float32)
    idf = np.load(CACHE / "semantic_query_idf.npy")
    tokenizer_path = CACHE / "opensearch-doc-v3/tokenizer.json"
    tokenizer = Tokenizer.from_file(str(tokenizer_path))
    tokenizer.no_truncation()
    tokenizer.no_padding()
    native = Native()
    lex = ROOT / "var/arena_cache/cnet/arena_trained.lex"
    frozen = json.loads((fixture / "frozen_model.json").read_text())
    native.open(lex, frozen["lexicon_trained"]["digest"])
    cent, cent_owner = prototypes(native.encode(train), owners, nc, 1, native)
    norms = np.linalg.norm(cent.astype(np.float32), axis=1).astype(np.float32)
    terms = [native.terms(text) for text in train]
    extra_text, extra_owner, _ = questions(fixture / "questions_train_v3_all.tsv", names)
    lexical = SparseIndex(terms + [native.terms(t) for t in extra_text], list(owners) + list(extra_owner), nc)
    passage = PassageIndex(terms, owners, nc)
    blocks = {"test_sentences": (test, test_gold, [{int(g)} for g in test_gold]),
              "questions": questions(fixture / "questions.tsv", names)}
    blocks.update({"questions_extra:" + n: questions(fixture / n, names) for n in EXTRAS})
    base = dict(np.load(CACHE / "centroid.npz"))
    bm25 = dict(np.load(CACHE / "bm25_expanded.npz"))
    qtokens = {key: [tokenizer.encode(t).ids for t in texts] for key, (texts, _, _) in blocks.items()}
    content = {key: [native.terms(t) for t in texts] for key, (texts, _, _) in blocks.items()}
    sample = [t for texts, _, _ in blocks.values() for t in texts[::max(1, len(texts)//40)][:40]]
    results, resources = {}, {}
    chunks = np.load(CACHE / "semantic_chunks.npz")
    for label, keep in (("semantic256", 256), ("semantic1024", 1024), ("chunks128", 128), ("chunks256", 256)):
        start = time.perf_counter()
        if label.startswith("chunks"):
            index = SemanticChunks(chunks["terms"], chunks["weights"], chunks["owners"], idf, keep)
        else:
            index = SparseIndex.from_weights(weights, idf, keep)
        semantic = {key: np.array([index.score(q, native) for q in qs]) for key, qs in qtokens.items()}
        two = {key: reciprocal_rank_fusion(base[key], semantic[key]) for key in blocks}
        three = {key: reciprocal_rank_fusion(base[key], bm25[key], extra=semantic[key]) for key in blocks}
        rerank = {}
        for key, scores in three.items():
            out = scores * (0.8 * 61 / 3)
            for i, q in enumerate(content[key]):
                out[i] += 0.2 * passage.score(q, np.flatnonzero(scores[i] > 0), native)
            rerank[key] = out
        presets = {label: semantic, label + "_fusion": two, label + "_three": three, label + "_passage": rerank}
        for name, scored in presets.items():
            results[name] = {key: metrics(s, blocks[key][1], blocks[key][2], base[key]) for key, s in scored.items()}
            np.savez(CACHE / (name + ".npz"), **scored)
        def infer(text, mode):
            s = index.score(tokenizer.encode(text).ids, native)[None, :]
            if mode == 0:
                out = s[0]
            else:
                q = native.encode([text], True)
                d = native.dense(q, cent, cent_owner, nc, norms)
                if mode == 1:
                    out = reciprocal_rank_fusion(d, s)[0]
                else:
                    qt = native.terms(text)
                    b = lexical.score(qt, native)[None, :]
                    out = reciprocal_rank_fusion(d, b, extra=s)[0]
                    if mode == 3:
                        out = out * (0.8 * 61 / 3) + 0.2 * passage.score(qt, np.flatnonzero(out > 0), native)
            return np.argsort(-out, kind="stable")[:20]
        base_bytes = index.nbytes + tokenizer_path.stat().st_size
        for mode, name in enumerate(presets):
            size = base_bytes
            if mode:
                size += lex.stat().st_size + cent.nbytes + cent_owner.nbytes + norms.nbytes
            if mode >= 2:
                size += lexical.nbytes
            if mode == 3:
                size += passage.nbytes
            resources[name] = {"runtime_data_bytes": size, "latency": latency(lambda t: infer(t, mode), sample),
                               "within_64_mib": bool(size <= 64 * 2**20)}
        print(label, f"index {index.nbytes / 2**20:.2f} MiB, scoring {time.perf_counter()-start:.1f}s", flush=True)
        for name in presets:
            print(name, " ".join(f"{100 * results[name][key]['top1']:.2f}" for key in blocks), flush=True)
    report = {"results": results, "resources": resources, "model_build": build,
              "limitations": ["All model inference was offline on train-only source chunks.",
                              "Query scoring uses Rust WordPiece tokenization, fixed IDF folded into postings, and native sparse sums.",
                              "Prototype comparisons use the unchanged v3.2 table. No certified acceptance claim.",
                              "Semantic table max-pools chunks and prunes per capsule; this adapts the published model.",
                              "lenient1 in this diagnostic uses per-query alternates only; use strict metrics for comparisons."]}
    (CACHE / "semantic_results.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
