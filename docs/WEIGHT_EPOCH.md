# Weight epoch + neural KV vs text/CERT memory

## Law

| Store | Epoch-dependent? | On MTK `.tskill` apply/revert |
|-------|------------------|-------------------------------|
| Dense/HOT/WARM **neural KV** | **Yes** | Clear + `weight_epoch++` |
| COLD **neural** pages | **Yes** | Rehydrate only if `page.epoch == pager.epoch` |
| CERT packs / raw text | **No** | Survive; used to re-prefill |
| STM stream index | Working set | Cleared with HOT (decode restarts) |

Dot products between \(Q(W_1)\) and \(K(W_0)\) are forbidden by epoch gates.

## Flush path

`cce_mtk_gguf_kv_flush`:

1. `model->weight_epoch++`
2. `cur_pos = 0` + memset dense K/V if present
3. `cce_kv_pager_bump_weight_epoch` → clear HOT + stamp ledger

COLD ledger lines include `weight_epoch=N`.

## Re-encoding COLD after swap — policy

**Default: lazy on demand (re-prefill from text/CERT), not background neural rewrite.**

| Strategy | When | Why |
|----------|------|-----|
| **Lazy re-prefill** | Next generate needs context | Correct under new \(W\); pay once |
| **Background neural re-index** | Optional later | Risk of thrash; only if residual host stays active long |
| **CERT-first** | Always prefer | 0-token; no re-prefill |

```text
swap .tskill
  → epoch++ / flush HOT / invalidate neural COLD
  → conversation text + CERT still valid
  → next residual turn: prefill from text (lazy)
  → optional worker: may prefetch prefill of last N tokens (not rewrite old KVC files)
```

Do **not** background-rewrite old `.kvc` tensors under \(W_1\) by default — cheaper and safer to **re-prefill from epoch-invariant text**.

## API

```c
uint64_t cce_kv_pager_weight_epoch(const cce_kv_pager *p);
int cce_kv_pager_bump_weight_epoch(cce_kv_pager *p);
/* model->weight_epoch bumped in cce_mtk_gguf_kv_flush */
```
