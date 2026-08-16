# grow_lobe_v1 — parallel train lobe

Status: **SLICE — SPEAKS AT TABLE SCALE — WITHHELD AS GENERAL MIND**

Beside Core / ROE-ASI / other ASI units. Ternary QAT student
(`cce_transformer_qat`). Intake is external teacher, user correction, or
verified tool only. Core Tier-A text is refused (anti-collapse).

English fuel: in-house table plus `cnet_grow_distill_held` (8B / hook
teacher text, tagged EXTERNAL). Distill is stored as a compact
Qwen/Bonsai chat turn (`<|im_start|>user/assistant`). The student is still
a tiny char LM (1×48d / T=48). Train stays FP on `train(1)` and on
`train_last` / `train_recent`; QAT only on a real second half of
`train(steps>1)`. Distill clips the assistant to ~96 chars. Teacher
done bar is recent-8 next-char acc; `acc1` is the newest line.
It can *eat* 8B prose. It cannot *be* 8B. `claimed_cert=0`.

```text
make cnet_grow
CNET_GROW_PASS
may_speak=1 claimed_cert=0 anti_collapse=1 ternary_qat=1
```
