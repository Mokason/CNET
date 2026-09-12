#!/usr/bin/env python3
"""Distil per-word context vectors from a transformer embedding model into the
lexicon's wide space (Model2Vec-style, applied offline; the runtime is unchanged).

  1. Tokenise the corpus dir exactly like the runtime (lowercase [a-z0-9_]+),
     get stem / stopword flag / key from `cnet_vsa_cli stem-words`, and pick
     the most frequent surface form of every stem as its representative.
  2. Embed each representative word once with the model (/v1/embeddings).
  3. PCA to --pca dims (centred), then a seeded random +-1/sqrt(k) projection
     into the 2048-d wide space, unit-normalised.
  4. Write the CNET_VSA_DISTILL file (magic DSTL, count, dim, key + float32[2048])
     that `cnet_vsa_cli lexicon-build --distilled` and `cnet_vsa_arena --distilled`
     blend into the Random-Indexing context before centring and PC removal.

The PCA matrix (keys + float16 rows) is also written so the DSTL file can be
regenerated from it with --from-pca and no model (that is what the arena gate
keeps under its tracked cache).
"""
import argparse, json, os, re, subprocess, sys, time, urllib.request
from collections import Counter, defaultdict
from pathlib import Path
import numpy as np

CLI = os.environ.get("CNET_CLI", "bin/cnet_vsa_cli")
DSTL_MAGIC = 0x4C545344
DIM = 2048


def embed(url, texts, prefix, batch=32):
    out = []
    for i in range(0, len(texts), batch):
        chunk = [prefix + t for t in texts[i:i + batch]]
        req = urllib.request.Request(url, data=json.dumps({"input": chunk}).encode(),
                                     headers={"Content-Type": "application/json"})
        last = None
        for attempt in range(8):
            try:
                with urllib.request.urlopen(req, timeout=600) as r:
                    d = json.load(r)
                break
            except Exception as e:  # noqa
                last = e
                time.sleep(5)
        else:
            raise RuntimeError(f"embedding failed: {last}")
        out.extend(x["embedding"] for x in sorted(d["data"], key=lambda x: x["index"]))
        if (i // batch) % 20 == 0:
            sys.stderr.write(f"\r  embedded {len(out)}/{len(texts)}")
    sys.stderr.write("\n")
    v = np.asarray(out, dtype=np.float32)
    v /= np.linalg.norm(v, axis=1, keepdims=True) + 1e-9
    return v


def vocabulary(corpus_dir, min_count):
    counts = Counter()
    for cf in Path(corpus_dir).glob("*_corpus.txt"):
        for line in cf.read_text().splitlines():
            for tok in re.findall(r"[a-z0-9_]+", line.lower()):
                counts[tok] += 1
    toks = sorted(counts)
    res = subprocess.run([CLI, "stem-words"], input="\n".join(toks) + "\n", capture_output=True, text=True)
    by_key = defaultdict(lambda: {"count": 0, "surfaces": Counter(), "stem": ""})
    for line in res.stdout.splitlines():
        tok, stem, stop, key = line.split("\t")
        if stop == "1":
            continue
        e = by_key[key]
        e["count"] += counts[tok]
        e["surfaces"][tok] += counts[tok]
        e["stem"] = stem
    rows = []
    for key, e in by_key.items():
        if e["count"] >= min_count:
            surf = e["surfaces"].most_common(1)[0][0]
            rows.append((key, e["stem"], surf, e["count"]))
    rows.sort(key=lambda r: -r[3])
    return rows


def write_dstl(path, keys, vectors):
    with open(path, "wb") as f:
        f.write(np.array([DSTL_MAGIC, len(keys), DIM], dtype=np.uint32).tobytes())
        for k, v in zip(keys, vectors):
            f.write(np.array([k], dtype=np.uint64).tobytes())
            f.write(v.astype(np.float32).tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus-dir", default="var/arena_cache/cnet/train")
    ap.add_argument("--url", default="", help="/v1/embeddings; omit with --from-pca")
    ap.add_argument("--prefix", default="", help="prefix for single words (Qwen3: none; nomic: 'search_document: ')")
    ap.add_argument("--pca", type=int, default=256)
    ap.add_argument("--seed", type=int, default=20260911)
    ap.add_argument("--min-count", type=int, default=2)
    ap.add_argument("--max-vocab", type=int, default=16384)
    ap.add_argument("--out", required=True, help="DSTL file for lexicon-build --distilled")
    ap.add_argument("--pca-cache", default="", help="npz with keys, surfaces, pca rows (write, or read with --from-pca)")
    ap.add_argument("--from-pca", action="store_true")
    ap.add_argument("--whiten", action="store_true", help="scale PCA rows by 1/singular value: isotropic distilled space")
    ap.add_argument("--raw-cache", default="", help="npy of raw word embeddings (untracked)")
    ap.add_argument("--phrases", default="", help="vocab dump TSV from lexicon-build --vocab-dump: embed the P rows (adjacent word pairs) too")
    ap.add_argument("--phrase-cache", default="", help="npz with phrase keys + projected rows (write, or read with --from-pca)")
    ap.add_argument("--basis-cache", default="", help="npz with mu, S, Vt of the word PCA (written when computing; needed to project phrases)")
    ap.add_argument("--phrase-raw-cache", default="", help="npy of raw phrase embeddings (untracked)")
    args = ap.parse_args()

    if args.from_pca:
        z = np.load(args.pca_cache, allow_pickle=False)
        keys, P = z["keys"], z["pca"].astype(np.float32)
        info = json.loads(str(z["info"]))
        print(f"from pca cache: {len(keys)} words, pca {P.shape[1]}, model {info.get('model')}")
    else:
        vocab = vocabulary(args.corpus_dir, args.min_count)[: args.max_vocab]
        keys = np.asarray([int(k, 16) for k, _, _, _ in vocab], dtype=np.uint64)
        surfaces = [s for _, _, s, _ in vocab]
        print(f"vocabulary: {len(vocab)} stems from {args.corpus_dir}; representatives e.g. {surfaces[:6]}")
        raw = None
        if args.raw_cache and Path(args.raw_cache).exists():
            raw = np.load(args.raw_cache).astype(np.float32)
            if raw.shape[0] != len(surfaces):
                raw = None
        if raw is None:
            if not args.url:
                sys.exit("need --url (or --from-pca)")
            t0 = time.time()
            raw = embed(args.url, surfaces, args.prefix)
            print(f"embedded {len(surfaces)} words in {time.time()-t0:.1f}s ({raw.shape[1]}-d)")
            if args.raw_cache:
                Path(args.raw_cache).parent.mkdir(parents=True, exist_ok=True)
                np.save(args.raw_cache, raw.astype(np.float16))
        mu = raw.mean(axis=0, keepdims=True)
        X = raw - mu
        U, S, Vt = np.linalg.svd(X, full_matrices=False)
        k = min(args.pca, Vt.shape[0])
        P = (X @ Vt[:k].T).astype(np.float32)
        if args.whiten:
            P = (P / (S[:k][None, :] + 1e-6) * S[:k].mean()).astype(np.float32)
        explained = float((S[:k] ** 2).sum() / (S ** 2).sum())
        print(f"pca {k}: explained variance {explained:.3f}")
        info = {"model": args.url, "prefix": args.prefix, "pca": k, "explained": explained, "seed": args.seed, "whiten": bool(args.whiten),
                "raw_dims": int(raw.shape[1]), "words": int(len(keys))}
        if args.pca_cache:
            Path(args.pca_cache).parent.mkdir(parents=True, exist_ok=True)
            np.savez_compressed(args.pca_cache, keys=keys, pca=P.astype(np.float16),
                                surfaces=np.asarray(surfaces), info=np.asarray(json.dumps(info)))

        if args.basis_cache:
            Path(args.basis_cache).parent.mkdir(parents=True, exist_ok=True)
            np.savez_compressed(args.basis_cache, mu=mu.astype(np.float32), S=S[:k].astype(np.float32), Vt=Vt[:k].astype(np.float32))

    # phrases: adjacent word pairs from the C vocabulary dump, embedded as "<surface1> <surface2>"
    # and projected with the WORD basis (same PCA, same whitening, same random projection) so a
    # phrase vector lives in the same distilled space as its words
    pkeys, PP = None, None
    if args.from_pca and args.phrase_cache and Path(args.phrase_cache).exists():
        zp = np.load(args.phrase_cache, allow_pickle=False)
        pkeys, PP = zp["keys"], zp["pca"].astype(np.float32)
        print(f"from phrase cache: {len(pkeys)} phrases")
    elif args.phrases:
        if not args.basis_cache or not Path(args.basis_cache).exists():
            sys.exit("--phrases needs --basis-cache (mu, S, Vt of the word PCA)")
        zb = np.load(args.basis_cache, allow_pickle=False)
        mu_b, S_b, Vt_b = zb["mu"].astype(np.float32), zb["S"].astype(np.float32), zb["Vt"].astype(np.float32)
        # word key -> representative surface (most frequent form), from the same tokeniser
        surf_of = {}
        for key, _, surf, _ in vocabulary(args.corpus_dir, 1):
            surf_of[int(key, 16)] = surf
        stem_of = {}
        rows = []
        for line in Path(args.phrases).read_text().splitlines()[1:]:
            f = line.split("\t")
            if f[0] == "W":
                stem_of[int(f[1], 16)] = f[2]
            elif f[0] == "P":
                a, b = int(f[2], 16), int(f[3], 16)
                sa, sb = surf_of.get(a, stem_of.get(a)), surf_of.get(b, stem_of.get(b))
                if sa and sb:
                    rows.append((int(f[1], 16), sa + " " + sb))
        pkeys = np.asarray([r[0] for r in rows], dtype=np.uint64)
        texts = [r[1] for r in rows]
        print(f"phrases: {len(rows)} from {args.phrases}; e.g. {texts[:6]}")
        praw = None
        if args.phrase_raw_cache and Path(args.phrase_raw_cache).exists():
            praw = np.load(args.phrase_raw_cache).astype(np.float32)
            if praw.shape[0] != len(texts):
                praw = None
        if praw is None:
            if not args.url:
                sys.exit("need --url to embed phrases")
            t0 = time.time()
            praw = embed(args.url, texts, args.prefix)
            print(f"embedded {len(texts)} phrases in {time.time()-t0:.1f}s")
            if args.phrase_raw_cache:
                Path(args.phrase_raw_cache).parent.mkdir(parents=True, exist_ok=True)
                np.save(args.phrase_raw_cache, praw.astype(np.float16))
        PP = ((praw - mu_b) @ Vt_b.T).astype(np.float32)
        if args.whiten or (args.from_pca and json.loads(str(np.load(args.pca_cache, allow_pickle=False)["info"])).get("whiten")):
            PP = (PP / (S_b[None, :] + 1e-6) * S_b.mean()).astype(np.float32)
        if args.phrase_cache:
            Path(args.phrase_cache).parent.mkdir(parents=True, exist_ok=True)
            np.savez_compressed(args.phrase_cache, keys=pkeys, pca=PP.astype(np.float16), texts=np.asarray(texts),
                                info=np.asarray(json.dumps({"phrases": int(len(pkeys)), "basis": "word pca", "source": args.phrases})))

    rng = np.random.default_rng(args.seed)
    R = rng.choice([-1.0, 1.0], size=(P.shape[1], DIM)).astype(np.float32) / np.sqrt(P.shape[1])
    V = P @ R
    V /= np.linalg.norm(V, axis=1, keepdims=True) + 1e-9
    if PP is not None and len(pkeys):
        VP = PP @ R
        VP /= np.linalg.norm(VP, axis=1, keepdims=True) + 1e-9
        keys = np.concatenate([keys, pkeys])
        V = np.concatenate([V, VP])
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    write_dstl(args.out, keys, V)
    # quasi-orthogonality check of the projected space
    idx = rng.choice(len(keys), size=min(400, len(keys)), replace=False)
    sub = V[idx]
    c = sub @ sub.T
    off = c[~np.eye(len(idx), dtype=bool)]
    print(f"wrote {args.out}: {len(keys)} keys x {DIM}; pairwise cosine of random words mean {off.mean():.4f} sd {off.std():.4f}")


if __name__ == "__main__":
    main()
