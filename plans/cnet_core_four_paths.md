# CORE four deep paths

Status: **IMPLEMENTED + BENCHED — NOT A MIND**

Philosophy: AGI-like CORE understands **given** info and values it
(prove vs abstain). Not all human knowledge. Not LLM parrot.

## Path 1 — Live waist

Only CERT/table RESULT or ABSTAIN.

- `cnet_path1_waist_decide` pure matrix
- `cnetd`: after RLM miss → `outside_table_abstain` (no ROE/LLM answer)
- OPEN_CHAT answers killed

**Bench:** `make cnet_path1_waist` → `result/bench_path1_waist.txt`

## Path 2 — Brick factory

Four-verb multi-domain. N+1 only after N serves 16/16 without LLM.

- 4 Bonsai Q1 domains (attn_q/k/v + ffn_gate)
- ~24 ms/brick, ~96 ms total

**Bench:** `make cnet_path2_factory` → `result/bench_path2_factory.txt`

## Path 3 — Miss-log → admit

propose (e-graph, admit_delta=0) → TABLE ≥0.95 → specialist_admit → serve

- Nibble miss-log → domain GGUF → full brick
- Propose never admits

**Bench:** `make cnet_path3_missadmit` → `result/bench_path3_missadmit.txt`

## Path 4 — Split / compose

- Split seed brick → lo/hi children
- Compose lo∘hi → new brick
- N+1 gate when previous unserved

**Bench:** `make cnet_path4_compose` → `result/bench_path4_compose.txt`

## Umbrella

```bash
make cnet_core_paths
# CNET_CORE_PATHS_PASS path1=1 path2=1 path3=1 path4=1 bus=1
```

## Law

| Allowed | Forbidden |
|---------|-----------|
| CERT prove | OPEN_CHAT leftover answer |
| Abstain outside table | ROE/LLM as product mouth |
| Teacher lease then leave | Residual auto-CERT |
| Propose from miss-log | Propose = admit |
