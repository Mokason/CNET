"""Opt-in candidate retrieval from verified NumPy caches; no neural query model."""
import hashlib
import json
from pathlib import Path

import numpy as np
from tokenizers import Tokenizer

from retrieval import ROOT, Native, PassageIndex, SparseIndex, reciprocal_rank_fusion


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def unpack_sparse(arrays, prefix, nc):
    index = SparseIndex.__new__(SparseIndex)
    types = {"keys": np.uint64, "offset": np.uint32, "owners": np.int32, "weights": np.float32}
    for name, dtype in types.items():
        a = arrays[prefix + name]
        if a.ndim != 1 or a.dtype != dtype:
            raise ValueError("invalid sparse cache array")
        setattr(index, name, np.ascontiguousarray(a))
    if len(index.offset) != len(index.keys) + 1 or index.offset[0] != 0 or index.offset[-1] != len(index.owners):
        raise ValueError("invalid sparse offsets")
    if len(index.weights) != len(index.owners) or np.any(np.diff(index.offset.astype(np.int64)) < 0):
        raise ValueError("invalid sparse postings")
    if np.any(index.keys[1:] <= index.keys[:-1]) or np.any(index.owners < 0) or np.any(index.owners >= nc):
        raise ValueError("invalid sparse vocabulary/owners")
    if not np.isfinite(index.weights).all() or np.any(index.weights < 0):
        raise ValueError("invalid sparse weights")
    index.nc = nc
    return index


class Retriever:
    def __init__(self, folder):
        folder = Path(folder)
        receipt = json.loads((folder / "receipt.json").read_text())
        lex = ROOT / "var/arena_cache/cnet/arena_trained.lex"
        for name in ("index.npz", "tokenizer.json"):
            if digest(folder / name) != receipt["sha256"][name]:
                raise ValueError(f"cache digest mismatch: {name}")
        if digest(lex) != receipt["sha256"]["lexicon"]:
            raise ValueError("frozen lexicon changed")
        self.native = Native()
        with np.load(folder / "index.npz", allow_pickle=False) as arrays:
            self.names = receipt["names"]
            nc = len(self.names)
            if not nc or len(set(self.names)) != nc:
                raise ValueError("invalid capsule names")
            self.cent = arrays["cent"]
            self.norms = arrays["norms"]
            if self.cent.dtype != np.int8 or self.cent.shape != (nc, self.native.dim):
                raise ValueError("invalid centroid cache")
            if self.norms.dtype != np.float32 or self.norms.shape != (nc,) or not np.isfinite(self.norms).all() or np.any(self.norms <= 0):
                raise ValueError("invalid centroid norms")
            self.owner = np.arange(nc, dtype=np.int32)
            self.lexical = unpack_sparse(arrays, "lexical_", nc)
            self.semantic = unpack_sparse(arrays, "semantic_", nc)
            self.passage = PassageIndex.__new__(PassageIndex)
            for name, dtype in (("terms", np.uint64), ("doc_offset", np.uint32), ("cap_offset", np.uint32)):
                a = arrays["passage_" + name]
                if a.ndim != 1 or a.dtype != dtype:
                    raise ValueError("invalid passage cache")
                setattr(self.passage, name, a)
            p = self.passage
            if not len(p.doc_offset) or len(p.cap_offset) != nc + 1 or p.doc_offset[0] != 0 or p.cap_offset[0] != 0:
                raise ValueError("invalid passage offsets")
            if p.doc_offset[-1] != len(p.terms) or p.cap_offset[-1] != len(p.doc_offset) - 1:
                raise ValueError("invalid passage bounds")
            if np.any(np.diff(p.doc_offset.astype(np.int64)) < 0) or np.any(np.diff(p.cap_offset.astype(np.int64)) < 0):
                raise ValueError("unordered passage offsets")
            p.nc = nc
        self.tokenizer = Tokenizer.from_file(str(folder / "tokenizer.json"))
        self.tokenizer.no_truncation()
        self.tokenizer.no_padding()
        self.native.open(lex, receipt["lexicon_digest"])
        self.receipt = receipt

    def scores(self, text):
        if not isinstance(text, str) or "\0" in text or len(text.encode()) > 4096:
            raise ValueError("query must be text without NUL, at most 4096 UTF-8 bytes")
        terms = self.native.terms(text)
        if not terms:
            return np.zeros(len(self.names), np.float32)
        q = self.native.encode([text], True)
        dense = self.native.dense(q, self.cent, self.owner, len(self.names), self.norms)
        lexical = self.lexical.score(terms, self.native)[None, :]
        semantic = self.semantic.score(self.tokenizer.encode(text).ids, self.native)[None, :]
        fused = reciprocal_rank_fusion(dense, lexical, extra=semantic)[0]
        candidates = np.flatnonzero(fused > 0)
        extra = self.passage.score(terms, candidates, self.native)
        return fused * (0.8 * 61 / 3) + extra * 0.2

    def query(self, text, k=5):
        if not 1 <= k <= 20:
            raise ValueError("k must be 1..20")
        scores = self.scores(text)
        top = np.argsort(-scores, kind="stable")[:k]
        return [{"capsule": self.names[i], "score": float(scores[i])} for i in top if scores[i] > 0]
