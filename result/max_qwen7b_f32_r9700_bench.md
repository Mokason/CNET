# MAX benchmark rerun — 20260816-164842

**EXIT=0** · Qwen2.5-7B-Instruct · float32 · R9700 gpu:0 · max_batch=4 · max_len=4096

Workload: random chat, in≈128, out≈64, 48 prompts, concurrency 1/2/4, `--no-collect-gpu-stats`

| mc | ok/fail | req/s | out tok/s mean | out tok/s med | TTFT mean | TTFT med | TPOT med | ITL mean | duration s |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 48/0 | 0.1375 | 20.76 | 20.97 | 4268 | 4355 | 47.68 | 47.99 | 349 |
| 2 | 44/0 | 0.1202 | 7.53 | 7.65 | 8401 | 8275 | 130.68 | 132.20 | 366 |
| 4 | 40/0 | 0.1720 | 7.22 | 7.33 | 14654 | 14639 | 136.44 | 138.05 | 233 |

## Takeaway
- Best single-stream decode: **~21.0 tok/s** (mc=1)
- Higher concurrency still does not help decode rate (queueing / mem-bound float32)

Artifacts: `result/max_qwen7b_f32_r9700_20260816-164842_*`
