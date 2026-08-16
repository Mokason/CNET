#!/usr/bin/env python3
"""Peek GGUF tensors + Q1_0/F32 dequant samples (offline, not mouth)."""
from __future__ import annotations
import argparse, json, sys
from pathlib import Path
import numpy as np

GGML_TYPES = {0:"F32",1:"F16",2:"Q4_0",8:"Q8_0",12:"Q4_K",30:"BF16",34:"TQ1_0",41:"Q1_0"}
QK1_0, BLOCK_Q1_0 = 128, 18

def dequant_q1_0(raw, n_elem):
    b = np.ascontiguousarray(raw, dtype=np.uint8).reshape(-1)
    n_blocks = n_elem // QK1_0
    b = b[: n_blocks * BLOCK_Q1_0].reshape(n_blocks, BLOCK_Q1_0)
    d = b[:,0:2].copy().view(np.uint16).view(np.float16).astype(np.float32).reshape(-1)
    out = np.empty(n_elem, dtype=np.float32)
    for i in range(n_blocks):
        bits = np.unpackbits(b[i,2:], bitorder="little")
        out[i*QK1_0:(i+1)*QK1_0] = np.where(bits.astype(bool), d[i], -d[i])
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gguf", nargs="?", default="/home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf")
    ap.add_argument("--out", default="result/gguf_weight_peek")
    ap.add_argument("--prefer", default="blk.0.attn_q.weight,blk.0.attn_k.weight,blk.0.attn_v.weight,blk.0.ffn_gate.weight,token_embd.weight,output.weight,output_norm.weight,blk.0.attn_norm.weight")
    args = ap.parse_args()
    path, out_dir = Path(args.gguf), Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    from gguf import GGUFReader
    r = GGUFReader(str(path))
    rows, hist = [], {}
    for i,t in enumerate(r.tensors):
        ti = int(t.tensor_type.value) if hasattr(t.tensor_type,"value") else int(t.tensor_type)
        shape=[int(x) for x in t.shape]
        ne=1
        for d in shape: ne*=d
        nm=GGML_TYPES.get(ti,str(ti)); hist[nm]=hist.get(nm,0)+1
        rows.append({"idx":i,"name":t.name,"shape":shape,"n_elem":ne,"ggml_type":ti,"ggml_type_name":nm})
    (out_dir/"tensors.json").write_text(json.dumps({"path":str(path),"size_bytes":path.stat().st_size,"n_tensors":len(rows),"type_hist":hist,"tensors":rows},indent=2))
    by={t.name:t for t in r.tensors}
    samples=[]
    for name in [x.strip() for x in args.prefer.split(",") if x.strip()]:
        t=by.get(name)
        if not t:
            samples.append({"name":name,"error":"missing"}); continue
        ti=int(t.tensor_type.value) if hasattr(t.tensor_type,"value") else int(t.tensor_type)
        shape=[int(x) for x in t.shape]; ne=1
        for d in shape: ne*=d
        e={"name":name,"shape":shape,"ggml_type":ti,"ggml_type_name":GGML_TYPES.get(ti,str(ti)),"n_elem":ne}
        raw=np.asarray(t.data)
        try:
            if ti==0:
                dq=np.asarray(raw,dtype=np.float32).reshape(-1); e["mode"]="f32_direct"
            elif ti==41:
                dq=dequant_q1_0(raw,ne); e["mode"]="q1_0_dequant"
            else:
                e["error"]=f"unsupported {ti}"; samples.append(e); continue
            e.update(sample8=[float(x) for x in dq[:8]], mean=float(dq.mean()), std=float(dq.std()), min=float(dq.min()), max=float(dq.max()), absmean=float(np.mean(np.abs(dq))))
            samples.append(e); print("OK", name, e["mode"], e["sample8"][:4])
        except Exception as ex:
            e["error"]=f"{type(ex).__name__}: {ex}"; samples.append(e); print("FAIL", name, e["error"])
    (out_dir/"dequant_samples.json").write_text(json.dumps(samples,indent=2))
    md=[f"# GGUF weight peek — `{path.name}`","",f"- tensors: **{len(rows)}**",f"- type_hist: `{json.dumps(hist)}`","", "## Dequant samples",""]
    for s in samples:
        md.append(f"### `{s['name']}`")
        if s.get("error"): md.append(f"- ERROR {s['error']}")
        else:
            md.append(f"- `{s['mode']}` sample8=`{s['sample8']}`")
            md.append(f"- absmean={s['absmean']:.6g} mean={s['mean']:.6g} std={s['std']:.6g}")
        md.append("")
    md += ["## Law","","- Offline only — not mouth, not CERT.",""]
    (out_dir/"PEEK.md").write_text("\n".join(md))
    print(f"GGUF_PEEK_OK n={len(rows)} q1={sum(1 for s in samples if s.get('mode')=='q1_0_dequant')} f32={sum(1 for s in samples if s.get('mode')=='f32_direct')}")
    return 0
if __name__=="__main__":
    raise SystemExit(main())
