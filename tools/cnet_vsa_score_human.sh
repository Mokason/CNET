#!/bin/bash
# Score the human-written post-freeze set against every CNET row and the Qwen3-Embedding-4B baseline, with the
# gate's frozen recipe, capsule-level alternates (alternates.tsv) and per-question alternates (gold|alt in the TSV).
#   1. fill benchmarks/vsa_routing_arena_20260911/questions_human_worksheet.md
#   2. python3 tools/cnet_vsa_human_worksheet.py            -> questions_human.tsv
#   3. serve Qwen3-Embedding-4B on :8097 (llama-server -m Qwen3-Embedding-4B-Q8_0.gguf --embeddings --pooling last --device ROCm0 --port 8097)
#   4. tools/cnet_vsa_score_human.sh
set -e
cd "$(dirname "$0")/.."
F=benchmarks/vsa_routing_arena_20260911; H=$F/questions_human.tsv; QWEN=${QWEN_URL:-http://127.0.0.1:8097/v1/embeddings}
[ -s $H ] || { echo "no $H (run tools/cnet_vsa_human_worksheet.py first)"; exit 1; }
curl -s -m 5 ${QWEN%/v1/embeddings}/health >/dev/null || { echo "Qwen server not reachable at $QWEN"; exit 1; }
mkdir -p logs var/arena_cache/cnet
python3 tools/cnet_vsa_lexicon_distill.py --from-pca --pca-cache $F/cache/qwen3-embedding-4b/vocab_pca256w.npz --out var/arena_cache/cnet/arena_distilled.dstl > /dev/null
FULL="--phrases 8192 --subwords 16384 --subword-min-n 3 --subword-max-n 5 --subword-max-words 40 --subword-weight 0.5 --lexicon-extra $F/questions_train_v2.tsv"
TRAIN="--train-questions $F/questions_train_v2.tsv --train-sentences 16 --train-lr 0.02 --train-epochs 8 --train-margin 0.10"
EVAL="--alternates $F/alternates.tsv --extra-questions $F/questions_fresh_20260912.tsv --extra-questions $F/questions_colloquial_eval.tsv --extra-questions $F/questions_contrast_eval.tsv --extra-questions $H"
./bin/cnet_vsa_arena --fixture $F --distilled var/arena_cache/cnet/arena_distilled.dstl --distill-alpha 0.5 --distill-beta 1.0 --distill-pcs 16 $FULL $TRAIN $EVAL --dump logs/vsa_human_dump.tsv | tee logs/vsa_human_arena.log | grep -E '^extra questions|^\| lex_trained'
python3 tools/cnet_vsa_arena_transformer.py --fixture $F --model qwen3-embedding-4b --url $QWEN --alternates $F/alternates.tsv --extra-questions $H | tail -3
python3 tools/cnet_vsa_arena_check.py $F | tee logs/vsa_human_check.log | grep -E 'questions_human|frozen model|CNET_VSA_ROUTING_ARENA'
python3 tools/cnet_vsa_human_breakdown.py $F logs/vsa_human_dump.tsv
