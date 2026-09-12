"""Compact retrieval primitives; NumPy is used only by the experiment harness."""
import ctypes as ct
import hashlib
import subprocess
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / "var/arena_cache/retrieval_20260912"


class Native:
    def __init__(self):
        sources = [ROOT / "src" / (n + ".c") for n in (
            "cnet_vsa", "cnet_vsa_bsc", "cnet_vsa_text", "cnet_vsa_lexicon",
            "cnet_vsa_memory", "cnet_vsa_ngram", "cnet_vsa_gen_capsule")]
        sources.append(Path(__file__).with_name("native.c"))
        headers = sorted((ROOT / "include").glob("*.h"))
        digest = hashlib.sha256(b"".join(p.read_bytes() for p in sources + headers)).hexdigest()[:16]
        CACHE.mkdir(parents=True, exist_ok=True)
        lib = CACHE / f"native-{digest}.so"
        if not lib.exists():
            subprocess.run(["gcc", "-std=c11", "-O3", "-march=native", "-Wall", "-Wextra", "-Werror",
                            "-fPIC", "-shared", "-D_GNU_SOURCE", "-DCNET_HAVE_CURL=0",
                            "-include", str(ROOT / "include/cnet_platform.h"),
                            "-I" + str(ROOT / "include"), *map(str, sources),
                            "-lm", "-pthread", "-o", str(lib)], check=True)
        self.lib = ct.CDLL(str(lib))
        self.dim = self.lib.vr_dim()
        self.lib.vr_open.argtypes = [ct.c_char_p, ct.c_uint64]
        self.lib.vr_encode.argtypes = [ct.c_char_p, ct.c_int, ct.c_void_p]
        self.lib.vr_terms.argtypes = [ct.c_char_p, ct.c_void_p, ct.c_int]
        self.lib.vr_quantize.argtypes = [ct.c_void_p, ct.c_int, ct.c_void_p]
        self.lib.vr_dense.argtypes = [ct.c_void_p, ct.c_int, ct.c_void_p, ct.c_void_p,
                                      ct.c_void_p, ct.c_int, ct.c_int, ct.c_void_p]
        self.lib.vr_sparse.argtypes = [ct.c_void_p, ct.c_int, ct.c_void_p, ct.c_void_p,
                                       ct.c_void_p, ct.c_int, ct.c_void_p]
        self.lib.vr_passage.argtypes = [ct.c_void_p, ct.c_int, ct.c_void_p, ct.c_int,
                                        ct.c_void_p, ct.c_void_p, ct.c_void_p, ct.c_int, ct.c_void_p]

    def open(self, path, expected):
        code = self.lib.vr_open(str(path).encode(), int(expected, 16))
        if code:
            raise ValueError(f"frozen lexicon refused: {path} (code {code})")

    def encode(self, texts, intent=False):
        out = np.empty((len(texts), self.dim), np.float32)
        for i, text in enumerate(texts):
            if self.lib.vr_encode(text.encode(), int(intent), out[i].ctypes.data):
                raise ValueError(f"cannot encode row {i}")
        return out.astype(np.int8) if intent else out

    def terms(self, text):
        out = np.empty(512, np.uint64)
        n = self.lib.vr_terms(text.encode(), out.ctypes.data, len(out))
        if n < 0:
            raise ValueError(f"tokenization refused ({n})")
        return out[:n].tolist()

    def quantize(self, vectors):
        vectors = np.ascontiguousarray(vectors, dtype=np.float32)
        if vectors.ndim != 2 or vectors.shape[1] != self.dim or not np.isfinite(vectors).all():
            raise ValueError("invalid wide vectors")
        out = np.empty(vectors.shape, np.int8)
        self.lib.vr_quantize(vectors.ctypes.data, len(vectors), out.ctypes.data)
        return out

    def dense(self, queries, p, owner, nc, norms=None):
        queries = np.ascontiguousarray(queries, dtype=np.int8)
        p = np.ascontiguousarray(p, dtype=np.int8)
        owner = np.ascontiguousarray(owner, dtype=np.int32)
        if queries.ndim != 2 or p.ndim != 2 or queries.shape[1] != self.dim or p.shape[1] != self.dim:
            raise ValueError("invalid q8 shape")
        if nc < 1 or len(owner) != len(p) or np.any(owner < 0) or np.any(owner >= nc):
            raise ValueError("invalid prototype owners")
        if norms is None:
            norms = np.linalg.norm(p.astype(np.float32), axis=1).astype(np.float32)
        norms = np.ascontiguousarray(norms, dtype=np.float32)
        if norms.shape != (len(p),) or not np.isfinite(norms).all():
            raise ValueError("invalid prototype norms")
        out = np.empty((len(queries), nc), np.float32)
        self.lib.vr_dense(queries.ctypes.data, len(queries), p.ctypes.data, norms.ctypes.data,
                          owner.ctypes.data, len(p), nc, out.ctypes.data)
        return out


def load_corpora(path):
    names, rows = [], []
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        name, role, text = line.split("\t", 2)
        if role not in {"train", "test"} or not text.strip():
            raise ValueError("unknown role or empty source")
        if name not in names:
            names.append(name)
        rows.append((name, role, text))
    if not names:
        raise ValueError("empty corpus")
    ids = {name: i for i, name in enumerate(names)}
    train = [text for name in names for n, r, text in rows if n == name and r == "train"]
    owner = np.array([ids[n] for name in names for n, r, _ in rows if n == name and r == "train"], np.int32)
    tests = [text for name in names for n, r, text in rows if n == name and r == "test"]
    gold = np.array([ids[n] for name in names for n, r, _ in rows if n == name and r == "test"], np.int32)
    if set(owner) != set(range(len(names))):
        raise ValueError("capsule has no train source")
    return names, train, owner, tests, gold


def prototypes(docs, owners, nc, k, native=None):
    """Train-only spherical k-means; k=1 preserves the original raw-vector sum."""
    if k < 1 or not np.isfinite(docs).all():
        raise ValueError("invalid prototype input")
    result, result_owner = [], []
    for ci in range(nc):
        raw = docs[owners == ci]
        if not len(raw):
            raise ValueError("capsule has no train vectors")
        if k == 1:
            centers = raw.sum(axis=0, keepdims=True)
        else:
            unit = raw / np.maximum(np.linalg.norm(raw, axis=1, keepdims=True), 1e-12)
            mean = unit.mean(axis=0)
            chosen = [int(np.argmax(unit @ mean))]
            best = unit @ unit[chosen[0]]
            for _ in range(1, min(k, len(raw))):
                distance = best.copy()
                distance[chosen] = np.inf
                chosen.append(int(np.argmin(distance)))
                best = np.maximum(best, unit @ unit[chosen[-1]])
            centers = unit[chosen].copy()
            for _ in range(8):
                assigned = (unit @ centers.T).argmax(axis=1)
                for j in range(len(centers)):
                    if np.any(assigned == j):
                        centers[j] = unit[assigned == j].sum(axis=0)
                centers /= np.maximum(np.linalg.norm(centers, axis=1, keepdims=True), 1e-12)
        result.extend(centers)
        result_owner.extend([ci] * len(centers))
    vectors = np.asarray(result, np.float32)
    if native is not None:
        vectors = native.quantize(vectors)
    else:
        vectors /= np.maximum(np.linalg.norm(vectors, axis=1, keepdims=True), 1e-12)
    return vectors, np.array(result_owner, np.int32)


class SparseIndex:
    """Capsule BM25 with precomputed postings; source text is not needed to score."""
    def __init__(self, documents, owners, nc):
        counts = [Counter() for _ in range(nc)]
        if len(documents) != len(owners) or nc < 1:
            raise ValueError("invalid sparse inputs")
        for terms, ci in zip(documents, owners):
            if ci < 0 or ci >= nc:
                raise ValueError("invalid sparse owner")
            counts[ci].update(terms)
        length = np.array([sum(c.values()) for c in counts], np.float32)
        avg = max(float(length.mean()), 1)
        postings = defaultdict(list)
        for ci, count in enumerate(counts):
            for term, tf in count.items():
                postings[int(term)].append((ci, tf))
        self.keys = np.array(sorted(postings), np.uint64)
        offset, ids, weights = [0], [], []
        for term in self.keys:
            entries = postings[int(term)]
            idf = np.log(1 + (nc - len(entries) + 0.5) / (len(entries) + 0.5))
            for ci, tf in entries:
                ids.append(ci)
                weights.append(idf * (tf * 2.2) / (tf + 1.2 * (0.25 + 0.75 * length[ci] / avg)))
            offset.append(len(ids))
        self.offset = np.array(offset, np.uint32)
        self.owners = np.array(ids, np.int32)
        self.weights = np.array(weights, np.float32)
        self.nc = nc

    @classmethod
    def from_weights(cls, matrix, idf, keep):
        """Fold fixed query IDF into positive document weights, then invert."""
        if matrix.ndim != 2 or idf.shape != (matrix.shape[1],) or keep < 1:
            raise ValueError("invalid semantic weight shape")
        if not np.isfinite(matrix).all() or not np.isfinite(idf).all() or np.any(matrix < 0) or np.any(idf < 0):
            raise ValueError("invalid semantic weights")
        top = np.argsort(-matrix, axis=1, kind="stable")[:, :keep]
        owners = np.repeat(np.arange(len(matrix), dtype=np.int32), top.shape[1])
        terms = top.ravel()
        weights = matrix[owners, terms] * idf[terms]
        return cls.from_entries(terms, owners, weights, len(matrix), matrix.shape[1])

    @classmethod
    def from_entries(cls, terms, owners, weights, nc, vocabulary_size):
        terms, owners, weights = np.asarray(terms), np.asarray(owners), np.asarray(weights)
        if terms.ndim != 1 or terms.shape != owners.shape or terms.shape != weights.shape or nc < 1:
            raise ValueError("invalid sparse entries")
        if np.any(terms < 0) or np.any(terms >= vocabulary_size) or np.any(owners < 0) or np.any(owners >= nc):
            raise ValueError("sparse entry outside vocabulary/corpus")
        if not np.isfinite(weights).all() or np.any(weights < 0):
            raise ValueError("invalid sparse entry weights")
        valid = weights > 0
        owners, terms, weights = owners[valid], terms[valid], weights[valid]
        order = np.argsort(terms, kind="stable")
        instance = cls.__new__(cls)
        instance.keys = np.arange(vocabulary_size, dtype=np.uint64)
        count = np.bincount(terms, minlength=vocabulary_size)
        instance.offset = np.concatenate(([0], np.cumsum(count))).astype(np.uint32)
        instance.owners = np.ascontiguousarray(owners[order], np.int32)
        instance.weights = np.ascontiguousarray(weights[order], np.float32)
        instance.nc = nc
        return instance

    @property
    def nbytes(self):
        return sum(a.nbytes for a in (self.keys, self.offset, self.owners, self.weights))

    def score(self, terms, native):
        terms = np.array(sorted(set(terms)), np.uint64)
        positions = np.searchsorted(self.keys, terms)
        valid = positions < len(self.keys)
        positions, terms = positions[valid], terms[valid]
        positions = np.ascontiguousarray(positions[self.keys[positions] == terms], np.uint32)
        out = np.empty(self.nc, np.float32)
        native.lib.vr_sparse(positions.ctypes.data, len(positions), self.offset.ctypes.data,
                             self.owners.ctypes.data, self.weights.ctypes.data, self.nc, out.ctypes.data)
        return out


def reciprocal_rank_fusion(dense, sparse, k=20, extra=None):
    if dense.shape != sparse.shape:
        raise ValueError("score shape mismatch")
    out = np.zeros_like(dense)
    branches = [(dense, False), (sparse, True)]
    if extra is not None:
        if extra.shape != dense.shape:
            raise ValueError("extra score shape mismatch")
        branches.append((extra, True))
    for scores, positive_only in branches:
        top = np.argsort(-scores, axis=1, kind="stable")[:, :k]
        for row in range(len(scores)):
            for rank, ci in enumerate(top[row]):
                if not positive_only or scores[row, ci] > 0:
                    out[row, ci] += 1 / (61 + rank)
    return out


class PassageIndex:
    def __init__(self, documents, owners, nc):
        if len(documents) != len(owners) or nc < 1 or any(c < 0 or c >= nc for c in owners):
            raise ValueError("invalid passage owners")
        grouped = [[] for _ in range(nc)]
        for terms, ci in zip(documents, owners):
            grouped[ci].append(terms)
        flat, offsets, caps = [], [0], [0]
        for docs in grouped:
            for terms in docs:
                flat.extend(terms)
                offsets.append(len(flat))
            caps.append(len(offsets) - 1)
        self.terms = np.array(flat, np.uint64)
        self.doc_offset = np.array(offsets, np.uint32)
        self.cap_offset = np.array(caps, np.uint32)
        self.nc = nc

    @property
    def nbytes(self):
        return self.terms.nbytes + self.doc_offset.nbytes + self.cap_offset.nbytes

    def score(self, query, candidates, native):
        q = np.ascontiguousarray(query, np.uint64)
        candidates = np.ascontiguousarray(candidates, np.int32)
        if q.ndim != 1 or candidates.ndim != 1 or np.any(candidates < 0) or np.any(candidates >= self.nc):
            raise ValueError("invalid passage query")
        out = np.empty(self.nc, np.float32)
        native.lib.vr_passage(q.ctypes.data, len(q), candidates.ctypes.data, len(candidates),
                              self.terms.ctypes.data, self.doc_offset.ctypes.data,
                              self.cap_offset.ctypes.data, self.nc, out.ctypes.data)
        return out


class SemanticChunks:
    """Sum within a semantic passage first, then max over its owning capsule."""
    def __init__(self, terms, weights, owners, idf, keep):
        if terms.shape != weights.shape or terms.ndim != 2 or len(owners) != len(terms) or keep < 1:
            raise ValueError("invalid semantic chunks")
        if not len(owners) or owners[0] != 0 or np.any(np.diff(owners) < 0) or np.any(np.diff(owners) > 1):
            raise ValueError("semantic chunks must cover capsules in order")
        terms = terms[:, :keep].astype(np.int32)
        if np.any(terms < 0) or np.any(terms >= len(idf)):
            raise ValueError("invalid semantic chunk term")
        weights = weights[:, :keep].astype(np.float32) * idf[terms]
        self.index = SparseIndex.from_entries(terms.ravel(), np.repeat(np.arange(len(terms)), terms.shape[1]),
                                               weights.ravel(), len(terms), len(idf))
        self.starts = np.concatenate(([0], np.flatnonzero(np.diff(owners)) + 1)).astype(np.int32)

    @property
    def nbytes(self):
        return self.index.nbytes + self.starts.nbytes

    def score(self, terms, native):
        return np.maximum.reduceat(self.index.score(terms, native), self.starts)
