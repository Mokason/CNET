# ood_handle_v1 — leftover hops to typed skills

Status: **SLICE — NOT OPEN CHAT — BROADER CLAIMS WITHHELD**

Leftover `ood_no_skill` is handled by `cnet_ood_handle`. It does **not**
hardcode fact answers and does **not** call teacher / residual.

## Hops

| Skill | When | A value |
|---|---|---|
| `add_u32_v1` / `sub_u32_v1` / `mul_u32_v1` | two integers + plus/minus/times | computed |
| `wiki_inform_v1` | leftover content words + GET | year cue → year bind; else JSON `extract` |
| `held_model_v1` | leftover after other hops | held GGUF / HTTP chat / test hook |

Wikipedia URL is a fixed template from the remaining title. No search box.
No page summary beyond the bound extract. Fail closed on empty subject,
overflow, 404, disambiguation, or extract that does not contain the subject.

## Gate

```text
make cnet_ood
CNET_OOD_PASS
hardcoded_facts=0 residual=0 teacher=0 python=0 broader_claims=WITHHELD
```
