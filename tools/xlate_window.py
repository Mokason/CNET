#!/usr/bin/env python3
"""Token-string identity across teachers: dump and translate mining windows.

Everything in the mining pipeline is WINDOW-INDEX encoded (one-hot inputs
and output fields index into the window array, not the global vocab), so
auditing a base against a different-tokenizer oracle only requires a
window whose SLOTS mean the same text: slot i of the translated window is
the target vocab's id for the same surface string as slot i of the source
window. Feed the translated window to CNET_RECERT via CNET_WINDOW_FILE and
the index space lines up by construction.

Subcommands:
  dump <model.gguf> <window.txt> [-o out.tsv]
      id -> piece -> normalized surface string, from the model's own
      embedded vocab (tokenizer.ggml.tokens).
  translate <src.gguf> <window.txt> <dst.gguf> [-o out.txt]
      emit the target-vocab window (same slot order); slots whose surface
      string is not a single token in the target vocab are UNTRANSLATABLE
      and written as -1 (recert skips those units; coverage is reported).

Surface normalization bridges tokenizer families: SentencePiece "▁" and
GPT2 byte-level "Ġ" both mean a leading space; "Ċ" means newline.
"""

import argparse
import struct
import sys
from pathlib import Path

GGUF_MAGIC = b"GGUF"
T_U8, T_I8, T_U16, T_I16, T_U32, T_I32, T_F32, T_BOOL, T_STR, T_ARR, \
    T_U64, T_I64, T_F64 = range(13)
SCALAR_SIZE = {T_U8: 1, T_I8: 1, T_U16: 2, T_I16: 2, T_U32: 4, T_I32: 4,
               T_F32: 4, T_BOOL: 1, T_U64: 8, T_I64: 8, T_F64: 8}


def read_hf_tokens(path):
    """HF checkpoint dir (tokenizer.json): returns (model_type, [tokens])."""
    import json
    tj = Path(path) / "tokenizer.json"
    data = json.loads(tj.read_text())
    vocab = data["model"]["vocab"]
    mtype = data["model"].get("type", "unknown")
    id2tok = [None] * (max(vocab.values()) + 1)
    for piece, tid in vocab.items():
        id2tok[tid] = piece
    for added in data.get("added_tokens", []):
        tid = added.get("id")
        if tid is not None:
            if tid >= len(id2tok):
                id2tok.extend([None] * (tid + 1 - len(id2tok)))
            id2tok[tid] = added.get("content")
    return mtype, [t if t is not None else "" for t in id2tok]


def read_vocab(path):
    """GGUF file or HF checkpoint dir."""
    p = Path(path)
    if p.is_dir():
        return read_hf_tokens(p)
    return read_gguf_tokens(p)


def read_gguf_tokens(path):
    """Minimal GGUF KV scan: returns (tokenizer_model, [token strings])."""
    with open(path, "rb") as f:
        def u32():
            return struct.unpack("<I", f.read(4))[0]

        def u64():
            return struct.unpack("<Q", f.read(8))[0]

        def gstr():
            n = u64()
            return f.read(n)

        if f.read(4) != GGUF_MAGIC:
            raise ValueError(f"{path}: not a GGUF file")
        version = u32()
        if version < 2:
            raise ValueError(f"{path}: GGUF v{version} unsupported")
        _tensor_count = u64()
        kv_count = u64()

        def skip_value(t):
            if t == T_STR:
                gstr()
            elif t == T_ARR:
                et = u32()
                n = u64()
                if et == T_STR:
                    for _ in range(n):
                        gstr()
                elif et in SCALAR_SIZE:
                    f.seek(n * SCALAR_SIZE[et], 1)
                else:
                    raise ValueError(f"nested array type {et}")
            elif t in SCALAR_SIZE:
                f.seek(SCALAR_SIZE[t], 1)
            else:
                raise ValueError(f"kv type {t}")

        tok_model, tokens = "unknown", None
        for _ in range(kv_count):
            key = gstr().decode("utf-8", "replace")
            t = u32()
            if key == "tokenizer.ggml.model" and t == T_STR:
                tok_model = gstr().decode("utf-8", "replace")
            elif key == "tokenizer.ggml.tokens" and t == T_ARR:
                et = u32()
                n = u64()
                if et != T_STR:
                    raise ValueError("tokens array is not strings")
                tokens = [gstr().decode("utf-8", "replace")
                          for _ in range(n)]
            else:
                skip_value(t)
            if tokens is not None and tok_model != "unknown":
                break
        if tokens is None:
            raise ValueError(f"{path}: no tokenizer.ggml.tokens")
        return tok_model, tokens


def read_gguf_bpe(path):
    """Full GGUF tokenizer read for the BPE encoder: returns
    (tokens, merges, token_type). merges/token_type are [] if absent."""
    with open(path, "rb") as f:
        u32 = lambda: struct.unpack("<I", f.read(4))[0]
        u64 = lambda: struct.unpack("<Q", f.read(8))[0]
        gstr = lambda: f.read(u64())
        if f.read(4) != GGUF_MAGIC:
            raise ValueError(f"{path}: not a GGUF file")
        if u32() < 2:
            raise ValueError(f"{path}: GGUF version unsupported")
        u64()                       # tensor count
        kv_count = u64()
        tokens, merges, ttype = None, [], []
        for _ in range(kv_count):
            key = gstr().decode("utf-8", "replace")
            t = u32()
            if key == "tokenizer.ggml.tokens" and t == T_ARR:
                u32(); n = u64()
                tokens = [gstr().decode("utf-8", "replace") for _ in range(n)]
            elif key == "tokenizer.ggml.merges" and t == T_ARR:
                u32(); n = u64()
                merges = [gstr().decode("utf-8", "replace") for _ in range(n)]
            elif key == "tokenizer.ggml.token_type" and t == T_ARR:
                u32(); n = u64()
                import array
                a = array.array("i"); a.frombytes(f.read(n * 4)); ttype = list(a)
            elif t == T_STR:
                gstr()
            elif t == T_ARR:
                et = u32(); n = u64()
                if et == T_STR:
                    for _ in range(n):
                        gstr()
                elif et in SCALAR_SIZE:
                    f.seek(n * SCALAR_SIZE[et], 1)
                else:
                    raise ValueError(f"nested array type {et}")
            elif t in SCALAR_SIZE:
                f.seek(SCALAR_SIZE[t], 1)
            else:
                raise ValueError(f"kv type {t}")
        if tokens is None:
            raise ValueError(f"{path}: no tokenizer.ggml.tokens")
        return tokens, merges, ttype


class GemmaBpe:
    """Rank-ordered BPE for the GGUF's own tokenizer (gemma4: uniform scores,
    real merges, SentencePiece '▁' spaces, 256-token byte fallback). Encoding
    is deterministic and verified by round-tripping the shipped context files
    (`--verify`). Fit for natural prose; exotic text/code is not guaranteed
    (no pre-tokenization regex) — encode refuses on any byte it cannot map."""

    def __init__(self, model_path):
        self.tokens, merges, ttype = read_gguf_bpe(model_path)
        self.piece2id = {p: i for i, p in enumerate(self.tokens)}
        self.rank = {}
        for i, m in enumerate(merges):
            parts = m.split(" ")
            if len(parts) == 2:
                self.rank.setdefault((parts[0], parts[1]), i)
        self.byte_tok = {}
        for i, tt in enumerate(ttype):
            p = self.tokens[i]
            if tt == 6 and p.startswith("<0x") and p.endswith(">"):
                self.byte_tok[int(p[3:-1], 16)] = i
        self.bos = self.piece2id.get("<bos>", 2)

    def _bpe(self, sym):
        while True:
            best, bi = None, -1
            for j in range(len(sym) - 1):
                r = self.rank.get((sym[j], sym[j + 1]))
                if r is not None and (best is None or r < best):
                    best, bi = r, j
            if bi < 0:
                break
            sym[bi:bi + 2] = [sym[bi] + sym[bi + 1]]
        return sym

    def encode(self, text, add_bos=True):
        ids = [self.bos] if add_bos else []
        for piece in self._bpe(list(text.replace(" ", "▁"))):
            tid = self.piece2id.get(piece)
            if tid is not None:
                ids.append(tid)
            else:
                for b in piece.encode("utf-8"):
                    bt = self.byte_tok.get(b)
                    if bt is None:
                        raise ValueError(f"unmappable byte 0x{b:02x} in {piece!r}")
                    ids.append(bt)
        return ids

    def decode(self, ids, drop_special=True):
        out = []
        for i in ids:
            if drop_special and i == self.bos:
                continue
            out.append(self.tokens[i].replace("▁", " "))
        return "".join(out)


def normalize(piece):
    """Common surface form across SentencePiece and GPT2 byte-level BPE."""
    return (piece.replace("▁", " ")   # SP word boundary
                 .replace("Ġ", " ")   # GPT2 leading space
                 .replace("Ċ", "\n"))  # GPT2 newline


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("dump")
    d.add_argument("model")
    d.add_argument("window")
    d.add_argument("-o", "--out")
    t = sub.add_parser("translate")
    t.add_argument("src_model")
    t.add_argument("window")
    t.add_argument("dst_model")
    t.add_argument("-o", "--out")
    e = sub.add_parser("encode", help="prose -> token-id list (BPE)")
    e.add_argument("model")
    e.add_argument("text", help="literal text, or @file to read a file")
    e.add_argument("-o", "--out", help="write ids one-per-line (else stdout)")
    e.add_argument("--no-bos", action="store_true", help="omit the leading <bos>")
    mc = sub.add_parser("mint-context",
                        help="prose file -> <name>.ids for the gap-lane context dir")
    mc.add_argument("model")
    mc.add_argument("textfile")
    mc.add_argument("name", help="context name (goal tag prefix); writes <dir>/<name>.ids")
    mc.add_argument("--dir", default="config/lane_contexts")
    mc.add_argument("--max-ctx", type=int, default=511,
                    help="reject if token count >= this (lane KV cap)")
    v = sub.add_parser("verify", help="round-trip the shipped contexts to prove the encoder")
    v.add_argument("model")
    v.add_argument("ids", nargs="+", help=".ids files to round-trip")
    args = ap.parse_args()

    if args.cmd == "encode":
        bpe = GemmaBpe(args.model)
        text = Path(args.text[1:]).read_text() if args.text.startswith("@") else args.text
        ids = bpe.encode(text, add_bos=not args.no_bos)
        body = "".join(f"{i}\n" for i in ids)
        if args.out:
            Path(args.out).write_text(body)
            print(f"wrote {args.out} ({len(ids)} ids)")
        else:
            sys.stdout.write(body)
        return 0

    if args.cmd == "mint-context":
        bpe = GemmaBpe(args.model)
        text = Path(args.textfile).read_text()
        ids = bpe.encode(text, add_bos=True)
        if len(ids) >= args.max_ctx:
            print(f"refused: {len(ids)} tokens >= max_ctx {args.max_ctx} "
                  f"(shorten the prose)", file=sys.stderr)
            return 1
        # self-check: the ids must decode+re-encode identically, else refuse
        if bpe.encode(bpe.decode(ids), add_bos=True) != ids:
            print("refused: context is not round-trip stable", file=sys.stderr)
            return 1
        out = Path(args.dir) / f"{args.name}.ids"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text("".join(f"{i}\n" for i in ids))
        print(f"wrote {out} ({len(ids)} tokens): {bpe.decode(ids)[:70]!r}...")
        return 0

    if args.cmd == "verify":
        bpe = GemmaBpe(args.model)
        allok = True
        for path in args.ids:
            gold = [int(x) for x in Path(path).read_text().split()]
            mine = bpe.encode(bpe.decode(gold), add_bos=(gold[:1] == [bpe.bos]))
            ok = mine == gold
            allok &= ok
            print(f"{'PASS' if ok else 'FAIL'} {path} "
                  f"({len(gold)} tokens){'' if ok else ' MISMATCH'}")
        print("ENCODER_VERIFY_" + ("PASS" if allok else "FAIL"))
        return 0 if allok else 1

    ids = [int(x.split()[0]) for x in Path(args.window).read_text().split()
           if x.strip()]

    if args.cmd == "dump":
        tok_model, tokens = read_vocab(args.model)
        out = Path(args.out or (args.window + ".tokens.tsv"))
        with out.open("w") as f:
            for tid in ids:
                piece = tokens[tid] if 0 <= tid < len(tokens) else "?"
                f.write(f"{tid}\t{piece}\t{normalize(piece)}\n")
        print(f"wrote {out} (tokenizer.ggml.model={tok_model}, "
              f"{len(ids)} slots)")
        return 0

    src_model, src_tokens = read_vocab(args.src_model)
    dst_model, dst_tokens = read_vocab(args.dst_model)
    dst_by_surface = {}
    for i, piece in enumerate(dst_tokens):
        dst_by_surface.setdefault(normalize(piece), i)

    out_ids, misses = [], []
    for tid in ids:
        surface = normalize(src_tokens[tid]) if 0 <= tid < len(src_tokens) \
            else None
        did = dst_by_surface.get(surface, -1) if surface is not None else -1
        out_ids.append(did)
        if did < 0:
            misses.append((tid, src_tokens[tid] if surface else "?"))

    out = Path(args.out or (args.window + f".xlate.txt"))
    out.write_text("".join(f"{d}\n" for d in out_ids))
    ok = len(out_ids) - len(misses)
    print(f"translated {ok}/{len(out_ids)} slots "
          f"({src_model} -> {dst_model}); untranslatable -> -1")
    if misses:
        print("untranslatable slots (unit skipped in cross-recert):",
              file=sys.stderr)
        for tid, piece in misses[:20]:
            print(f"  {tid}\t{piece!r}", file=sys.stderr)
        if len(misses) > 20:
            print(f"  ... +{len(misses) - 20} more", file=sys.stderr)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
