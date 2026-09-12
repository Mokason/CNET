"""Export experimental cache arrays; never writes a capsule or changes a gate."""
import os
os.environ["OPENBLAS_NUM_THREADS"] = "1"
import argparse
import json
import shutil

import numpy as np

from retrieval import CACHE, ROOT, Native, PassageIndex, SparseIndex, load_corpora, prototypes
from run import questions
from runtime import digest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--terms", type=int, choices=[256, 1024], default=256)
    args = ap.parse_args()
    fixture = ROOT / "benchmarks/vsa_routing_arena_20260911"
    folder = CACHE / f"runtime{args.terms}"
    folder.mkdir(parents=True, exist_ok=True)
    native = Native()
    lex = ROOT / "var/arena_cache/cnet/arena_trained.lex"
    frozen = json.loads((fixture / "frozen_model.json").read_text())
    native.open(lex, frozen["lexicon_trained"]["digest"])
    names, train, owners, _, _ = load_corpora(fixture / "corpora.tsv")
    build = json.loads((CACHE / "semantic_build.json").read_text())
    if build["names"] != names or build["source_sha256"] != digest(fixture / "corpora.tsv"):
        raise ValueError("semantic source mismatch")
    cent, _ = prototypes(native.encode(train), owners, len(names), 1, native)
    terms = [native.terms(t) for t in train]
    extra, extra_owner, _ = questions(fixture / "questions_train_v3_all.tsv", names)
    lexical = SparseIndex(terms + [native.terms(t) for t in extra], list(owners) + list(extra_owner), len(names))
    semantic = SparseIndex.from_weights(np.load(CACHE / "semantic_cap_weights.f16.npy").astype(np.float32),
                                        np.load(CACHE / "semantic_query_idf.npy"), args.terms)
    passage = PassageIndex(terms, owners, len(names))
    arrays = {"cent": cent, "norms": np.linalg.norm(cent.astype(np.float32), axis=1).astype(np.float32)}
    for prefix, index in (("lexical_", lexical), ("semantic_", semantic)):
        arrays.update({prefix + key: getattr(index, key) for key in ("keys", "offset", "owners", "weights")})
    arrays.update({"passage_" + key: getattr(passage, key) for key in ("terms", "doc_offset", "cap_offset")})
    np.savez(folder / "index.npz", **arrays)
    shutil.copyfile(CACHE / "opensearch-doc-v3/tokenizer.json", folder / "tokenizer.json")
    receipt = {"preset": f"semantic{args.terms}_passage", "names": names, "certified": False,
               "lexicon_digest": frozen["lexicon_trained"]["digest"], "model_revision": build["revision"],
               "sha256": {"index.npz": digest(folder / "index.npz"), "tokenizer.json": digest(folder / "tokenizer.json"), "lexicon": digest(lex)},
               "source_sha256": {str(p.relative_to(ROOT)): digest(p) for p in [fixture / "corpora.tsv", fixture / "questions_train_v3_all.tsv"]},
               "runtime_data_bytes": int(sum(a.nbytes for a in arrays.values()) + lex.stat().st_size + (folder / "tokenizer.json").stat().st_size + len(json.dumps(names).encode()) + len(names) * 4),
               "scope": "Uncalibrated retrieval candidates only. Production verifier, abstention, and execution are unchanged."}
    if receipt["runtime_data_bytes"] > 64 * 2**20:
        raise ValueError("runtime data exceeds 64 MiB experiment budget")
    (folder / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"EXPERIMENTAL_RUNTIME_EXPORTED {folder} {receipt['runtime_data_bytes']/2**20:.2f} MiB")


if __name__ == "__main__":
    main()
