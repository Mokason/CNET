"""Build a document-only semantic cache on AMD/ROCm; never encodes eval text.

Run using a local ROCm PyTorch environment. Production query scoring does not
import PyTorch/Transformers or load the model. See pinned model card for pooling.
"""
import os
os.environ["TOKENIZERS_PARALLELISM"] = "false"
os.environ["OPENBLAS_NUM_THREADS"] = "1"
import hashlib
import json
import time

import numpy as np
import torch
from huggingface_hub import snapshot_download
from transformers import AutoModelForMaskedLM, AutoTokenizer

from retrieval import CACHE, ROOT, load_corpora

MODEL = "opensearch-project/opensearch-neural-sparse-encoding-doc-v3-distill"
REVISION = "babf71f3c48695e2e53a978208e8aba48335e3c0"


def main():
    if not torch.version.hip or not torch.cuda.is_available():
        raise RuntimeError("this offline builder requires AMD/ROCm PyTorch")
    torch.set_num_threads(2)
    folder = CACHE / "opensearch-doc-v3"
    snapshot_download(MODEL, revision=REVISION, local_dir=str(folder), max_workers=2,
                      allow_patterns=["model.safetensors", "config.json", "tokenizer.json", "tokenizer_config.json",
                                      "special_tokens_map.json", "vocab.txt", "idf.json", "README.md"])
    tok = AutoTokenizer.from_pretrained(folder, local_files_only=True, trust_remote_code=False)
    model = AutoModelForMaskedLM.from_pretrained(folder, local_files_only=True, trust_remote_code=False).eval()
    idf_data = json.loads((folder / "idf.json").read_text())
    idf = np.zeros(tok.vocab_size, np.float32)
    vocab = tok.get_vocab()
    for term, value in idf_data.items():
        if term not in vocab:
            raise ValueError("IDF vocabulary mismatch")
        idf[vocab[term]] = value
    def pooling(logits, mask):
        values = (logits * mask.unsqueeze(-1)).max(dim=1).values
        values = torch.log1p(torch.log1p(torch.relu(values)))
        values[:, tok.all_special_ids] = 0
        return values
    # Known example from the publisher's model card, before any corpus run.
    with torch.inference_mode():
        doc = tok(["Currently New York is rainy."], return_tensors="pt", return_token_type_ids=False)
        values = pooling(model(**doc).logits, doc["attention_mask"])[0].numpy()
        ids = list(set(tok.encode("What's the weather in ny now?")))
        smoke = float((values[ids] * idf[ids]).sum())
        if abs(smoke - 11.1105) > 0.02:
            raise RuntimeError(f"publisher example mismatch: {smoke} vs 11.1105")
    print(f"SEMANTIC_MODEL_REFERENCE_PASS score={smoke:.5f}", flush=True)
    source = ROOT / "benchmarks/vsa_routing_arena_20260911/corpora.tsv"
    names, texts, owners, _, _ = load_corpora(source)
    chunks, chunk_owner = [], []
    current, previous = [], -1
    for text, owner in zip(texts, owners):
        if owner != previous and current:
            chunks.append(current); chunk_owner.append(previous); current = []
        previous = int(owner)
        for token in tok.encode(text, add_special_tokens=False):
            current.append(token)
            if len(current) == 254:
                chunks.append(current); chunk_owner.append(previous); current = []
    if current:
        chunks.append(current); chunk_owner.append(previous)
    print(f"Compiling {len(chunks)} train-only chunks; {len(names)} capsules on ROCm device 0", flush=True)
    model = model.to(device="cuda:0", dtype=torch.float16)
    weights = np.zeros((len(names), tok.vocab_size), np.float32)
    chunk_terms = np.empty((len(chunks), 256), np.uint16)
    chunk_weights = np.empty((len(chunks), 256), np.float16)
    start = time.perf_counter()
    with torch.inference_mode():
        for begin in range(0, len(chunks), 16):
            batch = [{"input_ids": [tok.cls_token_id] + ids + [tok.sep_token_id]} for ids in chunks[begin:begin+16]]
            feature = tok.pad(batch, padding=True, return_tensors="pt")
            feature = {k: v.to("cuda:0") for k, v in feature.items()}
            values = pooling(model(**feature).logits, feature["attention_mask"]).float().cpu().numpy()
            top = np.argsort(-values, axis=1, kind="stable")[:, :256]
            chunk_terms[begin:begin+len(values)] = top
            chunk_weights[begin:begin+len(values)] = np.take_along_axis(values, top, axis=1)
            for ci, v in zip(chunk_owner[begin:begin+16], values):
                np.maximum(weights[ci], v, out=weights[ci])
            if begin % 512 == 0:
                print(f"compiled {min(begin+16,len(chunks))}/{len(chunks)}, {time.perf_counter()-start:.1f}s", flush=True)
    np.save(CACHE / "semantic_cap_weights.f16.npy", weights.astype(np.float16))
    np.save(CACHE / "semantic_query_idf.npy", idf)
    np.savez(CACHE / "semantic_chunks.npz", terms=chunk_terms, weights=chunk_weights,
             owners=np.array(chunk_owner, np.int32))
    receipt = {"model": MODEL, "revision": REVISION, "publisher_reference_score": smoke,
               "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(), "names": names,
               "chunks": len(chunks), "chunk_tokens": 254, "precision": "float16 ROCm; float32 max accumulation",
               "elapsed_s": time.perf_counter() - start, "torch": torch.__version__, "hip": torch.version.hip,
               "files_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.iterdir() if p.is_file()}}
    (CACHE / "semantic_build.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"SEMANTIC_INDEX_BUILD_PASS {receipt['elapsed_s']:.1f}s", flush=True)


if __name__ == "__main__":
    main()
