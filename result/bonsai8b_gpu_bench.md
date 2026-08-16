# Bonsai-8B GPU benchmark

- Model: `Bonsai-8B.gguf` (local stand-in for ~7B-class Bonsai; no prism 7B file present)
- Endpoint: `http://127.0.0.1:8081/v1/chat/completions`
- Serve: `ROCm llama-server -ngl 99 port 8081 (ROCR_VISIBLE_DEVICES=1)`
- Quality: pong + `2+2→4` OK

| mc | ok/fail | req/s | out tok/s mean | out tok/s med | TPOT med ms | latency med s | wall s |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | 24/0 | 1.512 | 49.90 | 49.87 | 20.05 | 0.678 | 15.9 |
| 2 | 20/0 | 3.052 | 49.00 | 49.24 | 20.31 | 0.650 | 6.6 |
| 4 | 20/0 | 3.087 | 48.49 | 48.87 | 20.46 | 1.294 | 6.5 |

JSON: `/home/marble/AI/CNET/result/bonsai8b_gpu_bench.json`
