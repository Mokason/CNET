#!/usr/bin/env python3
"""Transformer side of the frozen routing arena (benchmarks/vsa_routing_arena_*).

Scores an embedding model on exactly the protocol the C side runs
(tools/cnet_vsa_arena.c): nearest corpus for held-out test sentences and for
teacher-written questions, leave-one-out separability against seeded negatives,
and alien-query statistics. Embeddings come from an OpenAI-style /v1/embeddings
endpoint (llama-server --embeddings) and are cached:

  var/arena_cache/<model>/docs.f16.npy        every train sentence embedding (large, untracked)
  var/arena_cache/<model>/{tests,questions,aliens}.f16.npy   query embeddings (untracked)
  <fixture>/cache/<model>/sims_tests.f16.npy  cosine of every test sentence vs every corpus centroid
  <fixture>/cache/<model>/sims_questions.f16.npy, sims_aliens.f16.npy
  <fixture>/cache/<model>/dists.npz           per-corpus sorted LOO / negative distances
  <fixture>/cache/<model>/meta.json           model, dims, prefixes, sha256 of inputs, timings

The tracked cache holds only similarity matrices and distance lists (a few MB per
model, independent of embedding width); --score-only reproduces every number from
them with no model. The untracked raw embeddings allow re-deriving them.
"""
import argparse, hashlib, json, math, os, sys, time, urllib.request
from pathlib import Path
import numpy as np

NNEG = 512
POLICIES = [(0.90, 0.95), (0.80, 0.90), (0.70, 0.90)]


def sha256(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def load_fixture(fdir):
    corp = {}
    order = []
    for line in (fdir / "corpora.tsv").read_text().splitlines():
        name, role, sent = line.split("\t", 2)
        if name not in corp:
            corp[name] = {"train": [], "test": []}
            order.append(name)
        corp[name][role].append(sent)
    aliens = [l.strip() for l in (fdir / "queries_alien.txt").read_text().splitlines() if l.strip()]
    questions = []
    qf = fdir / "questions.tsv"
    if qf.exists():
        for line in qf.read_text().splitlines():
            name, q = line.split("\t", 1)
            if name in corp:
                questions.append((name, q))
    return order, corp, aliens, questions


def embed(url, texts, prefix, batch=32, timeout=900, checkpoint=None):
    """checkpoint: optional .npy path; partial rows are saved every 2000 texts and
    resumed, so a server restart does not restart an hour of embedding."""
    out = []
    start = 0
    if checkpoint and Path(checkpoint).exists():
        part = np.load(checkpoint)
        out = [row for row in part.astype(np.float32)]
        start = len(out)
        sys.stderr.write(f"  resuming from checkpoint at {start}/{len(texts)}\n")
    for i in range(start, len(texts), batch):
        chunk = [prefix + t for t in texts[i:i + batch]]
        req = urllib.request.Request(url, data=json.dumps({"input": chunk}).encode(),
                                     headers={"Content-Type": "application/json"})
        last = None
        for attempt in range(6):
            try:
                with urllib.request.urlopen(req, timeout=timeout) as r:
                    d = json.load(r)
                break
            except Exception as e:  # noqa
                last = e
                time.sleep(3)
        else:
            raise RuntimeError(f"embedding failed after retries: {last}")
        rows = sorted(d["data"], key=lambda x: x["index"])
        out.extend(x["embedding"] for x in rows)
        if (i // batch) % 40 == 0:
            sys.stderr.write(f"\r  embedded {len(out)}/{len(texts)}")
        if checkpoint and len(out) % 2000 < batch:
            np.save(checkpoint, np.asarray(out, dtype=np.float32))
    sys.stderr.write("\n")
    v = np.asarray(out, dtype=np.float32)
    if checkpoint and Path(checkpoint).exists():
        Path(checkpoint).unlink()
    v /= np.linalg.norm(v, axis=1, keepdims=True) + 1e-9
    return v


def xs(s):
    s ^= (s << 13) & 0xFFFFFFFFFFFFFFFF
    s ^= s >> 7
    s ^= (s << 17) & 0xFFFFFFFFFFFFFFFF
    return s


def separable(d_in, d_neg, t_in, t_neg):
    n, m = len(d_in), len(d_neg)
    k = max(1, min(n, math.ceil(t_in * n - 1e-9)))
    j = min(m - 1, int(math.floor((1 - t_neg) * m + 1e-9)))
    return d_in[k - 1] <= d_neg[j] - 1e-4


def topk(Q, cent, gold, k=3):
    sims = Q @ cent.T
    top = np.argsort(-sims, axis=1)[:, :k]
    srt = -np.sort(-sims, axis=1)
    return float((top[:, 0] == gold).mean()), float((top == gold[:, None]).any(axis=1).mean()), float((srt[:, 0] - srt[:, 1]).mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixture", default="benchmarks/vsa_routing_arena_20260911")
    ap.add_argument("--model", required=True, help="cache name, e.g. nomic-embed-text-v1.5")
    ap.add_argument("--url", default="", help="/v1/embeddings endpoint; omit with --score-only")
    ap.add_argument("--doc-prefix", default="")
    ap.add_argument("--query-prefix", default="")
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--score-only", action="store_true")
    ap.add_argument("--out", default="")
    ap.add_argument("--extra-questions", default="", help="name<TAB>question TSV written after the model freeze; scored as its own block and merged into results_<model>.json")
    ap.add_argument("--alternates", default="", help="capsule<TAB>alternate TSV of justified multiple valid targets: adds top1_lenient (same labels as the CNET side)")
    args = ap.parse_args()

    fdir = Path(args.fixture)
    order, corp, aliens, questions = load_fixture(fdir)
    alts = {}
    if args.alternates:
        for l in Path(args.alternates).read_text().splitlines():
            if not l or l.startswith("#"):
                continue
            f = l.split("\t")
            if len(f) >= 2 and f[0] in order and f[1] in order:
                alts.setdefault(order.index(f[0]), set()).add(order.index(f[1]))
    def lenient(top1, gold):
        return float(np.mean([t == g or t in alts.get(int(g), ()) for t, g in zip(top1, gold)]))
    small = fdir / "cache" / args.model
    big = Path("var/arena_cache") / args.model
    small.mkdir(parents=True, exist_ok=True)
    big.mkdir(parents=True, exist_ok=True)
    nc = len(order)
    t0 = time.time()
    timings = {}

    train_texts, train_owner = [], []
    for ci, name in enumerate(order):
        for s in corp[name]["train"]:
            train_texts.append(s)
            train_owner.append(ci)
    train_owner = np.asarray(train_owner)
    test_texts = [s for name in order for s in corp[name]["test"]]
    test_gold = np.repeat(np.arange(nc), 2)

    # ---- extra question set: embed (or reuse the cached sims), score against the cached centroids, merge ----
    if args.extra_questions:
        xp = Path(args.extra_questions)
        rows = [l.split("\t") for l in xp.read_text().splitlines() if l and not l.startswith("#") and "\t" in l]
        xq, xalt = [], []
        for f in rows:
            g = f[0].split("|")
            if g[0] in order:
                xq.append((g[0], f[1])); xalt.append({order.index(a) for a in g[1:] if a in order})
        tag = sha256(xp)[:12]
        sims_p = small / f"sims_extra_{tag}.f16.npy"
        if sims_p.exists() and not args.url:
            sims = np.load(sims_p).astype(np.float32)
        else:
            if not args.url:
                sys.exit(f"no cached sims for {xp.name} and no --url")
            docs_path = big / "docs.f16.npy"
            if not docs_path.exists():
                sys.exit("extra questions need the cached train-sentence embeddings (docs.f16.npy)")
            D = np.load(docs_path).astype(np.float32)
            cent = np.zeros((nc, D.shape[1]), dtype=np.float32)
            for ci in range(nc):
                cent[ci] = D[train_owner == ci].mean(axis=0)
            cent /= np.linalg.norm(cent, axis=1, keepdims=True) + 1e-9
            X = embed(args.url, [q for _, q in xq], args.query_prefix, args.batch)
            sims = (X @ cent.T).astype(np.float32)
            np.save(sims_p, sims.astype(np.float16))
        gold = np.asarray([order.index(n) for n, _ in xq])
        top = np.argsort(-sims, axis=1)[:, :3]
        srt = -np.sort(-sims, axis=1)
        len1 = float(np.mean([t == g or t in alts.get(int(g), ()) or t in xa for t, g, xa in zip(top[:, 0], gold, xalt)]))
        block = {"n": int(len(xq)), "top1": float((top[:, 0] == gold).mean()), "top3": float((top == gold[:, None]).any(axis=1).mean()),
                 "margin": float((srt[:, 0] - srt[:, 1]).mean()), "top1_lenient": len1, "source": str(xp), "sha256": sha256(xp)}
        out = Path(args.out) if args.out else fdir / f"results_{args.model}.json"
        res = json.loads(out.read_text()) if out.exists() else {"model": args.model}
        res[f"questions_extra:{xp.name}"] = block
        res.pop("questions_extra", None)
        out.write_text(json.dumps(res, indent=2) + "\n")
        # per-question dump for error analysis
        (big / f"extra_{tag}_top.tsv").write_text("".join(f"{n}\t{order[top[i,0]]}\t{order[top[i,1]]}\t{srt[i,0]:.4f}\t{srt[i,1]:.4f}\t{q}\n" for i, (n, q) in enumerate(xq)))
        print(json.dumps({"model": args.model, "questions_extra": block}, indent=1))
        return

    # ---- score-only from the tracked similarity cache (no model, no raw embeddings) ----
    if args.score_only and (small / "sims_tests.f16.npy").exists() and (small / "dists.npz").exists():
        meta = json.loads((small / "meta.json").read_text())
        res = {"model": args.model, "dims": meta.get("dims"), "corpora": nc,
               "doc_prefix": meta.get("doc_prefix", ""), "query_prefix": meta.get("query_prefix", ""),
               "fixture_sha256": {"corpora.tsv": sha256(fdir / "corpora.tsv")}}
        def score_sims(name, gold):
            sims = np.load(small / f"sims_{name}.f16.npy").astype(np.float32)
            top = np.argsort(-sims, axis=1)[:, :3]
            srt = -np.sort(-sims, axis=1)
            return {"n": int(sims.shape[0]), "top1": float((top[:, 0] == gold).mean()),
                    "top3": float((top == gold[:, None]).any(axis=1).mean()), "margin": float((srt[:, 0] - srt[:, 1]).mean()),
                    "top1_lenient": lenient(top[:, 0], gold)}
        res["test_sentences"] = score_sims("tests", test_gold)
        if questions and (small / "sims_questions.f16.npy").exists():
            res["questions"] = score_sims("questions", np.asarray([order.index(n) for n, _ in questions]))
            res["fixture_sha256"]["questions.tsv"] = sha256(fdir / "questions.tsv")
        dz = np.load(small / "dists.npz")
        sep = [0, 0, 0]
        for ci in range(nc):
            d_in, d_neg = dz[f"in_{ci}"], dz[f"neg_{ci}"]
            for p_, (ti, tn) in enumerate(POLICIES):
                if separable(d_in, d_neg, ti, tn):
                    sep[p_] += 1
        res["separability"] = {f"{ti:.2f}/{tn:.2f}": {"count": sep[p_], "fraction": sep[p_] / nc} for p_, (ti, tn) in enumerate(POLICIES)}
        res["null_sd"] = meta.get("null_sd"); res["in_domain_mean_cos"] = meta.get("in_domain_mean_cos")
        asims = np.load(small / "sims_aliens.f16.npy").astype(np.float32)
        res["aliens"] = {"n": int(asims.shape[0]), "best_cos_mean": float(asims.max(axis=1).mean()),
                         "z_mean": float(((asims.max(axis=1) - asims.mean(axis=1)) / (asims.std(axis=1) + 1e-9)).mean())}
        res["timings_s"] = meta.get("timings_s", {})
        out = Path(args.out) if args.out else fdir / f"results_{args.model}.json"
        if out.exists():
            prev = json.loads(out.read_text())
            for k in list(prev):
                if k.startswith("questions_extra"):
                    res[k] = prev[k]
        out.write_text(json.dumps(res, indent=2) + "\n")
        print(json.dumps({k: res[k] for k in ("model", "test_sentences", "questions", "separability", "aliens") if k in res}, indent=1))
        return

    docs_path = big / "docs.f16.npy"
    if args.score_only or docs_path.exists():
        D = np.load(docs_path).astype(np.float32) if docs_path.exists() else None
    else:
        D = None
    if D is None:
        if not args.url:
            sys.exit("no cached embeddings and no --url")
        t = time.time()
        D = embed(args.url, train_texts, args.doc_prefix, args.batch, checkpoint=str(big / "docs.partial.npy"))
        timings["train_embed_s"] = time.time() - t
        np.save(docs_path, D.astype(np.float16))

    # centroids of train sentences
    cent = np.zeros((nc, D.shape[1]), dtype=np.float32)
    for ci in range(nc):
        cent[ci] = D[train_owner == ci].mean(axis=0)
    cent /= np.linalg.norm(cent, axis=1, keepdims=True) + 1e-9

    def cached_or_embed(name, texts):
        p = big / f"{name}.f16.npy"
        legacy = small / f"{name}.f16.npy"
        if not p.exists() and legacy.exists():
            p = legacy
        if p.exists() and (args.score_only or not args.url):
            return np.load(p).astype(np.float32)
        if p.exists() and np.load(p).shape[0] == len(texts):
            return np.load(p).astype(np.float32)
        if not args.url:
            sys.exit(f"no cached {name} embeddings and no --url")
        t = time.time()
        v = embed(args.url, texts, args.query_prefix, args.batch)
        timings[f"{name}_embed_s"] = time.time() - t
        np.save(big / f"{name}.f16.npy", v.astype(np.float16))
        return v

    T = cached_or_embed("tests", test_texts)
    A = cached_or_embed("aliens", aliens)
    Qv = cached_or_embed("questions", [q for _, q in questions]) if questions else None
    q_gold = np.asarray([order.index(n) for n, _ in questions]) if questions else None

    res = {"model": args.model, "dims": int(D.shape[1]), "corpora": nc,
           "doc_prefix": args.doc_prefix, "query_prefix": args.query_prefix,
           "fixture_sha256": {"corpora.tsv": sha256(fdir / "corpora.tsv")}}
    t1, t3, mg = topk(T, cent, test_gold)
    res["test_sentences"] = {"n": int(len(test_texts)), "top1": t1, "top3": t3, "margin": mg}
    np.save(small / "sims_tests.f16.npy", (T @ cent.T).astype(np.float16))
    np.save(small / "sims_aliens.f16.npy", (A @ cent.T).astype(np.float16))
    if Qv is not None:
        t1, t3, mg = topk(Qv, cent, q_gold)
        res["questions"] = {"n": int(len(questions)), "top1": t1, "top3": t3, "margin": mg}
        res["fixture_sha256"]["questions.tsv"] = sha256(fdir / "questions.tsv")
        np.save(small / "sims_questions.f16.npy", (Qv @ cent.T).astype(np.float16))

    # separability: LOO of train sentences vs 512 seeded negatives
    sep = [0, 0, 0]
    d_in_all, d_neg_all = [], []
    null_sims, in_sims = [], []
    for ci in range(nc):
        idx = np.where(train_owner == ci)[0]
        V = D[idx]
        S = V.sum(axis=0)
        loo = S[None, :] - V
        loo /= np.linalg.norm(loo, axis=1, keepdims=True) + 1e-9
        d_in = np.sort(1.0 - np.einsum("ij,ij->i", V, loo))
        full = S / (np.linalg.norm(S) + 1e-9)
        s = (0x9E3779B97F4A7C15 ^ ((ci * 0xD1B54A32D192ED03) & 0xFFFFFFFFFFFFFFFF)) & 0xFFFFFFFFFFFFFFFF
        negs, guard = [], 0
        while len(negs) < NNEG and guard < NNEG * 20:
            s = xs(s); oc = s % nc; guard += 1
            if oc == ci:
                continue
            s = xs(s); li = s % len(corp[order[oc]]["train"])
            negs.append(int(np.where(train_owner == oc)[0][li]))
        nsim = D[negs] @ full
        d_neg = np.sort(1.0 - nsim)
        null_sims.append(nsim); in_sims.append(1.0 - d_in)
        d_in_all.append(d_in.astype(np.float32)); d_neg_all.append(d_neg.astype(np.float32))
        for p, (ti, tn) in enumerate(POLICIES):
            if separable(d_in, d_neg, ti, tn):
                sep[p] += 1
    np.savez_compressed(small / "dists.npz",
                        **{f"in_{i}": d for i, d in enumerate(d_in_all)},
                        **{f"neg_{i}": d for i, d in enumerate(d_neg_all)})
    res["separability"] = {f"{ti:.2f}/{tn:.2f}": {"count": sep[p], "fraction": sep[p] / nc}
                           for p, (ti, tn) in enumerate(POLICIES)}
    res["null_sd"] = float(np.concatenate(null_sims).std())
    res["in_domain_mean_cos"] = float(np.concatenate(in_sims).mean())

    # aliens: best cosine and z against the centroid population (what a margin gate sees)
    asims = A @ cent.T
    res["aliens"] = {"n": len(aliens),
                     "best_cos_mean": float(asims.max(axis=1).mean()),
                     "z_mean": float(((asims.max(axis=1) - asims.mean(axis=1)) / (asims.std(axis=1) + 1e-9)).mean())}
    res["timings_s"] = timings
    res["total_s"] = time.time() - t0
    meta = {k: v for k, v in res.items() if k != "separability"}
    (small / "meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    out = Path(args.out) if args.out else fdir / f"results_{args.model}.json"
    out.write_text(json.dumps(res, indent=2) + "\n")
    print(json.dumps({k: res[k] for k in ("model", "dims", "test_sentences", "questions", "separability", "null_sd", "aliens", "timings_s") if k in res}, indent=1))


if __name__ == "__main__":
    main()
