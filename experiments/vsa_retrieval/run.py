"""Reproduce and measure fixed compact retrieval presets on the frozen arena."""
import os
os.environ["OPENBLAS_NUM_THREADS"] = "1"
os.environ["OMP_NUM_THREADS"] = "1"
import argparse
import hashlib
import json
import time
from pathlib import Path

import numpy as np

from retrieval import CACHE, ROOT, Native, PassageIndex, SparseIndex, load_corpora, prototypes, reciprocal_rank_fusion

EXTRAS = ["questions_fresh_20260912.tsv", "questions_colloquial_eval.tsv",
          "questions_contrast_eval.tsv", "questions_eval_mistral_everyday.tsv",
          "questions_eval_gemma_everyday.tsv", "questions_indep_mistral_mid_20260912.tsv"]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def questions(path, names):
    ids = {name: i for i, name in enumerate(names)}
    texts, gold, alternatives = [], [], []
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        labels = fields[0].split("|")
        if len(fields) < 2 or not fields[1].strip() or any(n not in ids for n in labels):
            raise ValueError(f"invalid question in {path}")
        texts.append(fields[1])
        gold.append(ids[labels[0]])
        alternatives.append({ids[n] for n in labels})
    return texts, np.array(gold, np.int32), alternatives


def metrics(scores, gold, alternatives, baseline=None, qwen=None):
    # Stable capsule-id tie break; no duplicated prototype IDs can enter top-k.
    ranked = np.argsort(-scores, axis=1, kind="stable")
    hit = ranked[:, 0] == gold
    r = {"n": len(gold), "top1": float(hit.mean()),
         "top3": float((ranked[:, :3] == gold[:, None]).any(axis=1).mean()),
         "recall20": float((ranked[:, :20] == gold[:, None]).any(axis=1).mean()),
         "lenient1": float(np.mean([int(p) in a for p, a in zip(ranked[:, 0], alternatives)]))}
    for label, ref in (("v32", baseline), ("qwen", qwen)):
        if ref is not None:
            reference_hit = ref.argmax(axis=1) == gold
            r[f"wins_vs_{label}"] = int((hit & ~reference_hit).sum())
            r[f"losses_vs_{label}"] = int((~hit & reference_hit).sum())
    return r


def latency(fn, texts):
    # Includes Python orchestration and result ranking, excludes index loading.
    for text in texts[:10]:
        fn(text)
    times = []
    for text in texts:
        t = time.perf_counter_ns()
        fn(text)
        times.append((time.perf_counter_ns() - t) / 1000)
    return {"n": len(times), "p50_us": float(np.median(times)),
            "p95_us": float(np.percentile(times, 95)), "max_us": float(max(times))}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixture", type=Path, default=ROOT / "benchmarks/vsa_routing_arena_20260911")
    ap.add_argument("--stage", choices=["baseline", "prototypes", "sparse", "all"], default="all")
    ap.add_argument("--out", type=Path, default=CACHE / "results.json")
    args = ap.parse_args()
    fixture = args.fixture.resolve()
    native = Native()
    lex = ROOT / "var/arena_cache/cnet/arena_trained.lex"
    frozen = json.loads((fixture / "frozen_model.json").read_text())
    native.open(lex, frozen["lexicon_trained"]["digest"])
    start = time.perf_counter()
    names, train, owners, test, test_gold = load_corpora(fixture / "corpora.tsv")
    nc = len(names)
    docvec = native.encode(train)
    print(f"Encoded {len(train)} allowed train sentences over {nc} capsules", flush=True)
    blocks = {"test_sentences": (test, test_gold, [{int(g)} for g in test_gold]),
              "questions": questions(fixture / "questions.tsv", names)}
    blocks.update({"questions_extra:" + name: questions(fixture / name, names) for name in EXTRAS})
    alts = {}
    ids = {name: i for i, name in enumerate(names)}
    for line in (fixture / "alternates.tsv").read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        a, b, *_ = line.split("\t")
        alts.setdefault(ids[a], set()).add(ids[b])
    for _, gold, valid in blocks.values():
        for g, a in zip(gold, valid):
            a.update(alts.get(int(g), ()))
    queries = {key: native.encode(texts, intent=True) for key, (texts, _, _) in blocks.items()}
    cent, cent_owner = prototypes(docvec, owners, nc, 1, native)
    norms = np.linalg.norm(cent.astype(np.float32), axis=1).astype(np.float32)
    base = {key: native.dense(q, cent, cent_owner, nc, norms) for key, q in queries.items()}
    cached_results = json.loads((fixture / "results_cnet.json").read_text())["encoders"]["lex_trained"]
    qwen = {}
    small = fixture / "cache/qwen3-embedding-4b"
    for key, (_, gold, valid) in blocks.items():
        r = metrics(base[key], gold, valid)
        for metric in ("top1", "top3"):
            if abs(r[metric] - cached_results[key][metric]) > 0.00011:
                raise RuntimeError(f"baseline mismatch {key} {metric}: {r[metric]} != {cached_results[key][metric]}")
        stem = "sims_tests" if key == "test_sentences" else "sims_questions"
        if key.startswith("questions_extra:"):
            stem = "sims_extra_" + sha256(fixture / key.split(":", 1)[1])[:12]
        qwen[key] = np.load(small / (stem + ".f16.npy")).astype(np.float32)
        if qwen[key].shape != base[key].shape:
            raise ValueError("transformer cache shape mismatch")
    print("CNET_VSA_RETRIEVAL_BASELINE_PASS: eight v3.2 blocks reproduce", flush=True)
    sample = []
    for texts, _, _ in blocks.values():
        sample.extend(texts[::max(1, len(texts) // 40)][:40])
    presets = {"centroid": base}
    resources = {"centroid": {"index_bytes": cent.nbytes + cent_owner.nbytes + norms.nbytes,
                               "lexicon_bytes": lex.stat().st_size}}
    native_cent = lambda text: np.argsort(-native.dense(native.encode([text], True), cent, cent_owner, nc, norms)[0], kind="stable")[:20]
    resources["centroid"]["latency"] = latency(native_cent, sample)
    if args.stage in {"prototypes", "all"}:
        for k in (4, 8):
            t = time.perf_counter()
            p, po = prototypes(docvec, owners, nc, k, native)
            pn = np.linalg.norm(p.astype(np.float32), axis=1).astype(np.float32)
            build_s = time.perf_counter() - t
            scored = {key: native.dense(q, p, po, nc, pn) for key, q in queries.items()}
            label = f"prototype{k}"
            presets[label] = scored
            presets[label + "_mix"] = {key: (base[key] + scored[key]) * 0.5 for key in base}
            size = p.nbytes + po.nbytes + pn.nbytes
            resources[label] = {"index_bytes": size, "lexicon_bytes": lex.stat().st_size, "build_s": build_s,
                "latency": latency(lambda text: np.argsort(-native.dense(native.encode([text], True), p, po, nc, pn)[0], kind="stable")[:20], sample)}
            resources[label + "_mix"] = {"index_bytes": size + resources["centroid"]["index_bytes"], "lexicon_bytes": lex.stat().st_size,
                "latency": latency(lambda text: np.argsort(-(native.dense(native.encode([text], True), p, po, nc, pn)[0] + native.dense(native.encode([text], True), cent, cent_owner, nc, norms)[0]), kind="stable")[:20], sample)}
            print(f"Scored {label} ({size / 2**20:.2f} MiB, build {build_s:.2f}s)", flush=True)
    if args.stage in {"sparse", "all"}:
        doc_terms = [native.terms(text) for text in train]
        passage = PassageIndex(doc_terms, owners, nc)
        query_terms = {key: [native.terms(text) for text in texts] for key, (texts, _, _) in blocks.items()}
        for suffix, training in (("corpus", None), ("expanded", "questions_train_v3_all.tsv")):
            t = time.perf_counter()
            expanded, expanded_owner = [], []
            if training:
                texts, expanded_owner, _ = questions(fixture / training, names)
                expanded = [native.terms(text) for text in texts]
            index = SparseIndex(doc_terms + expanded, list(owners) + list(expanded_owner), nc)
            build_s = time.perf_counter() - t
            scored = {key: np.array([index.score(q, native) for q in ts]) for key, ts in query_terms.items()}
            label = "bm25_" + suffix
            presets[label] = scored
            presets["fusion_" + suffix] = {key: reciprocal_rank_fusion(base[key], scored[key]) for key in base}
            resources[label] = {"index_bytes": index.nbytes, "lexicon_bytes": 0, "build_s": build_s,
                "latency": latency(lambda text: np.argsort(-index.score(native.terms(text), native), kind="stable")[:20], sample)}
            def fused_query(text):
                dense = native.dense(native.encode([text], True), cent, cent_owner, nc, norms)
                sparse = index.score(native.terms(text), native)[None, :]
                return np.argsort(-reciprocal_rank_fusion(dense, sparse)[0], kind="stable")[:20]
            resources["fusion_" + suffix] = {"index_bytes": index.nbytes + resources["centroid"]["index_bytes"],
                "lexicon_bytes": lex.stat().st_size, "latency": latency(fused_query, sample)}
            fused = presets["fusion_" + suffix]
            def rescore(text):
                q = native.encode([text], True)
                qt = native.terms(text)
                d = native.dense(q, cent, cent_owner, nc, norms)
                f = reciprocal_rank_fusion(d, index.score(qt, native)[None, :])[0]
                candidates = np.flatnonzero(f > 0).astype(np.int32)
                extra = passage.score(qt, candidates, native)
                return np.argsort(-(0.8 * f * (61 / 2) + 0.2 * extra), kind="stable")[:20]
            reranked = {}
            for key, fs in fused.items():
                score = fs.copy() * (0.8 * 61 / 2)
                for i, qt in enumerate(query_terms[key]):
                    candidates = np.flatnonzero(fs[i] > 0).astype(np.int32)
                    score[i] += 0.2 * passage.score(qt, candidates, native)
                reranked[key] = score
            presets["passage_" + suffix] = reranked
            resources["passage_" + suffix] = {"index_bytes": index.nbytes + passage.nbytes + resources["centroid"]["index_bytes"],
                "lexicon_bytes": lex.stat().st_size, "latency": latency(rescore, sample)}
            print(f"Scored {label} ({index.nbytes / 2**20:.2f} MiB, build {build_s:.2f}s)", flush=True)
    results = {label: {key: metrics(scores, blocks[key][1], blocks[key][2], base[key], qwen[key])
                      for key, scores in blockscores.items()} for label, blockscores in presets.items()}
    results["qwen_centroid"] = {key: metrics(scores, blocks[key][1], blocks[key][2], base[key]) for key, scores in qwen.items()}
    for label, blockscores in presets.items():
        np.savez(CACHE / (label + ".npz"), **blockscores)
    inputs = {str(p.relative_to(ROOT)): sha256(p) for p in [fixture / "corpora.tsv", fixture / "questions.tsv",
              fixture / "questions_train_v3_all.tsv", fixture / "frozen_model.json", lex, *[fixture / n for n in EXTRAS]]}
    report = {"protocol": "plans/cnet_vsa_retrieval_20260912.md", "inputs_sha256": inputs,
              "stage": args.stage, "corpora": nc, "train_sentences": len(train), "results": results,
              "resources": resources, "elapsed_s": time.perf_counter() - start,
              "limitations": ["Previously exposed synthetic sets; not a fresh test.", "No certified router or wrong-accept claim.",
                              "Latency includes Python orchestration, encoding/tokenization and ranking; excludes loading and certification.",
                              "Sparse-only runtime needs no lexicon; Python benchmark process retains the loaded lexicon."]}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n")
    labels = list(blocks)
    print("preset " + " ".join(k.split(":")[-1].replace("questions_", "")[:16] for k in labels), flush=True)
    for label, rows in results.items():
        print(label + " " + " ".join(f"{100 * rows[key]['top1']:.2f}" for key in labels), flush=True)
    print(f"Results: {args.out}", flush=True)


if __name__ == "__main__":
    main()
