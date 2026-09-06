# Weight epochs and KV validity

Neural KV depends on the host weights that produced it. A tensor-delta MTK
swap therefore invalidates cached neural state; it is not the same operation
as replacing an independently certified capsule.

`cce_mtk_gguf_kv_flush` advances the model epoch, resets decode position and
clears dense HOT state, bound stream-index state and pager working state.
The [pager API](../include/cce/cce_kv_page.h) exposes epoch queries/advancement.
Cold pages from a different weight epoch must refuse rehydration.

| State | After host-weight change |
| --- | --- |
| Neural K/V and old cold neural pages | Invalid until recomputed for the new epoch |
| Stream-index working set | Reset with decode state |
| Raw conversation text | Can be used to prefill again |
| Independently certified capsules | Retain their own compatibility/coverage checks |

Re-prefill from retained text under the new weights when residual context is
needed. Background conversion of old neural pages is not the default policy
and must not bypass epoch checks. An epoch counter is a correctness boundary,
not authentication of a model artifact.

For capsule inventory reload and pinned core generations, use
[CAPSULE_CORE.md](CAPSULE_CORE.md), not this MTK-specific lifecycle.
