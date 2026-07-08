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
    args = ap.parse_args()

    ids = [int(x.split()[0]) for x in Path(args.window).read_text().split()
           if x.strip()]

    if args.cmd == "dump":
        tok_model, tokens = read_gguf_tokens(args.model)
        out = Path(args.out or (args.window + ".tokens.tsv"))
        with out.open("w") as f:
            for tid in ids:
                piece = tokens[tid] if 0 <= tid < len(tokens) else "?"
                f.write(f"{tid}\t{piece}\t{normalize(piece)}\n")
        print(f"wrote {out} (tokenizer.ggml.model={tok_model}, "
              f"{len(ids)} slots)")
        return 0

    src_model, src_tokens = read_gguf_tokens(args.src_model)
    dst_model, dst_tokens = read_gguf_tokens(args.dst_model)
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
