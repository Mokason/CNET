# pack_english_basic — sealed elementary English for ROE

## Purpose

LOCAL CERT answers for basic vocabulary and grammar so ROE answers
English itself. Teacher may still tutor on gaps; drafts never auto-CERT.

## Install

Pack lives at:

- `artifacts/roe_daily_packs/pack_english_basic/` (runtime)
- `packs/pack_english_basic/` (tracked)

Routes in `ROUTES.jsonl` + `config/domain_routes.tsv`.

```bash
# after pull
cp -a packs/pack_english_basic artifacts/roe_daily_packs/
# or deploy package
systemctl --user restart cnetd.service
```

## Try

```bash
cnet-sock-ask "a or an"
cnet-sock-ask "past tense of go"
cnet-sock-ask "what is a noun"
cnet-sock-ask "days of the week"
```

Expect `SOURCE LOCAL` / pack_english_basic skills.

## Growth

1. Teacher draft on novel English miss → miss_log  
2. Human/reviewer → `gold/<sha>.txt` matching sealed text  
3. evolve gold_file promote → more LOCAL skills  

Never: raw Teacher essay auto-mint into CERT.
