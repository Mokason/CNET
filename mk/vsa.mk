# mk/vsa.mk -- CNET-VSA (hyperdimensional capsule router) targets: benches, CLI, lexicon, frozen arena.
# Moved out of the root Makefile on 2026-09-12 (makefile budget). Gates in the verify ladder: see mk/verify_tiers.mk.

.PHONY: cnet_vsa_delta_bench cnet_vsa_answer_bench cnet_vsa_bench cnet_vsa_simd_bench cnet_vsa_index_bench cnet_vsa_reason_bench cnet_vsa_doc_graft_bench cnet_vsa_gpu_bench cnet_vsa_device_bench cnet_vsa_text_bench cnet_vsa_capsule_swap_bench cnet_vsa_sleep_bench cnet_vsa_ast_bench cnet_vsa_story_bench cnet_vsa_compare_bench cnet_vsa_cli cnet_vsa_cli_bench cnet_vsa_calibration_bench cnet_vsa_arena_bench cnet_vsa_stem_bench cnet_vsa_lexicon_bench cnet_vsa_encoder_sweep_bench cnet_vsa_all_bench
cnet_vsa_bench: src/cnet_vsa.c src/cnet_vsa_memory.c src/cnet_vsa_bus.c tests/test_cnet_vsa_bench.c include/cnet_vsa.h include/cnet_vsa_memory.h include/cnet_vsa_bus.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_bench \
		src/cnet_vsa.c src/cnet_vsa_memory.c src/cnet_vsa_bus.c \
		tests/test_cnet_vsa_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_bench | tee logs/cnet_vsa_bench.log
	@grep -q "CNET_VSA_BENCH_PASS" logs/cnet_vsa_bench.log

cnet_vsa_simd_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c tests/test_cnet_vsa_simd_bench.c include/cnet_vsa.h include/cnet_vsa_bsc.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -mavx2 -mfma -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_simd_bench \
		src/cnet_vsa.c src/cnet_vsa_bsc.c \
		tests/test_cnet_vsa_simd_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_simd_bench | tee logs/cnet_vsa_simd_bench.log
	@grep -q "CNET_VSA_SIMD_BENCH_PASS" logs/cnet_vsa_simd_bench.log

cnet_vsa_index_bench: src/cnet_vsa.c src/cnet_vsa_index.c tests/test_cnet_vsa_index_bench.c include/cnet_vsa.h include/cnet_vsa_index.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_index_bench \
		src/cnet_vsa.c src/cnet_vsa_index.c \
		tests/test_cnet_vsa_index_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_index_bench | tee logs/cnet_vsa_index_bench.log
	@grep -q "CNET_VSA_INDEX_BENCH_PASS" logs/cnet_vsa_index_bench.log

cnet_vsa_reason_bench: src/cnet_vsa.c src/cnet_vsa_memory.c src/cnet_vsa_reason.c tests/test_cnet_vsa_reason_bench.c include/cnet_vsa.h include/cnet_vsa_memory.h include/cnet_vsa_reason.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_reason_bench \
		src/cnet_vsa.c src/cnet_vsa_memory.c src/cnet_vsa_reason.c \
		tests/test_cnet_vsa_reason_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_reason_bench | tee logs/cnet_vsa_reason_bench.log
	@grep -q "CNET_VSA_REASON_BENCH_PASS" logs/cnet_vsa_reason_bench.log

cnet_vsa_doc_graft_bench: src/cnet_vsa.c src/cnet_vsa_doc_graft.c tests/test_cnet_vsa_doc_graft_bench.c include/cnet_vsa.h include/cnet_vsa_doc_graft.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_doc_graft_bench \
		src/cnet_vsa.c src/cnet_vsa_doc_graft.c \
		tests/test_cnet_vsa_doc_graft_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_doc_graft_bench | tee logs/cnet_vsa_doc_graft_bench.log
	@grep -q "CNET_VSA_DOC_GRAFT_BENCH_PASS" logs/cnet_vsa_doc_graft_bench.log

cnet_vsa_gpu_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_gpu.hip tests/test_cnet_vsa_gpu_bench.cpp include/cnet_vsa.h include/cnet_vsa_bsc.h include/cnet_vsa_gpu.h
	@mkdir -p $(BIN_DIR) logs
	hipcc -O3 --offload-arch=gfx1201 -Iinclude \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_gpu.hip \
		tests/test_cnet_vsa_gpu_bench.cpp -o $(BIN_DIR)/test_cnet_vsa_gpu_bench -lm -lpthread
	@./$(BIN_DIR)/test_cnet_vsa_gpu_bench | tee logs/cnet_vsa_gpu_bench.log
	@grep -q "CNET_VSA_GPU_BENCH_PASS" logs/cnet_vsa_gpu_bench.log

cnet_vsa_device_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_gpu.hip src/cnet_vsa_device.c tests/test_cnet_vsa_device_bench.cpp include/cnet_vsa.h include/cnet_vsa_bsc.h include/cnet_vsa_gpu.h include/cnet_vsa_device.h
	@mkdir -p $(BIN_DIR) logs
	hipcc -O3 --offload-arch=gfx1201 -Iinclude \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_gpu.hip src/cnet_vsa_device.c \
		tests/test_cnet_vsa_device_bench.cpp -o $(BIN_DIR)/test_cnet_vsa_device_bench -lm -lpthread
	@./$(BIN_DIR)/test_cnet_vsa_device_bench | tee logs/cnet_vsa_device_bench.log
	@grep -q "CNET_VSA_DEVICE_BENCH_PASS" logs/cnet_vsa_device_bench.log

cnet_vsa_text_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c tests/test_cnet_vsa_text_bench.c include/cnet_vsa.h include/cnet_vsa_bsc.h include/cnet_vsa_text.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_text_bench \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c \
		tests/test_cnet_vsa_text_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_text_bench | tee logs/cnet_vsa_text_bench.log
	@grep -q "CNET_VSA_TEXT_BENCH_PASS" logs/cnet_vsa_text_bench.log

cnet_vsa_capsule_swap_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_gpu.hip src/cnet_vsa_device.c src/cnet_vsa_capsule_swap.cpp tests/test_cnet_vsa_capsule_swap_bench.cpp include/cnet_vsa.h include/cnet_vsa_bsc.h include/cnet_vsa_gpu.h include/cnet_vsa_device.h include/cnet_vsa_capsule_swap.h
	@mkdir -p $(BIN_DIR) logs
	hipcc -O3 --offload-arch=gfx1201 -Iinclude \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_gpu.hip src/cnet_vsa_device.c src/cnet_vsa_capsule_swap.cpp \
		tests/test_cnet_vsa_capsule_swap_bench.cpp -o $(BIN_DIR)/test_cnet_vsa_capsule_swap_bench -lm -lpthread
	@./$(BIN_DIR)/test_cnet_vsa_capsule_swap_bench | tee logs/cnet_vsa_capsule_swap_bench.log
	@grep -q "CNET_VSA_CAPSULE_SWAP_BENCH_PASS" logs/cnet_vsa_capsule_swap_bench.log

cnet_vsa_sleep_bench: src/cnet_vsa.c src/cnet_vsa_sleep.c tests/test_cnet_vsa_sleep_bench.c include/cnet_vsa.h include/cnet_vsa_sleep.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_sleep_bench \
		src/cnet_vsa.c src/cnet_vsa_sleep.c \
		tests/test_cnet_vsa_sleep_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_sleep_bench | tee logs/cnet_vsa_sleep_bench.log
	@grep -q "CNET_VSA_SLEEP_BENCH_PASS" logs/cnet_vsa_sleep_bench.log

cnet_vsa_ast_bench: src/cnet_vsa.c src/cnet_vsa_ast.c tests/test_cnet_vsa_ast_bench.c include/cnet_vsa.h include/cnet_vsa_ast.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_ast_bench \
		src/cnet_vsa.c src/cnet_vsa_ast.c \
		tests/test_cnet_vsa_ast_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_ast_bench | tee logs/cnet_vsa_ast_bench.log
	@grep -q "CNET_VSA_AST_BENCH_PASS" logs/cnet_vsa_ast_bench.log

cnet_vsa_story_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c src/cnet_vsa_story.c tests/test_cnet_vsa_story_bench.c include/cnet_vsa.h include/cnet_vsa_story.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_story_bench \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c src/cnet_vsa_story.c \
		tests/test_cnet_vsa_story_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_story_bench | tee logs/cnet_vsa_story_bench.log
	@grep -q "CNET_VSA_STORY_BENCH_PASS" logs/cnet_vsa_story_bench.log

cnet_vsa_compare_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c src/cnet_vsa_story.c src/cnet_vsa_ngram.c src/cnet_vsa_hybrid.c tests/test_cnet_vsa_generation_comparison.c include/cnet_vsa_ngram.h include/cnet_vsa_hybrid.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_generation_comparison \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c \
		src/cnet_vsa_story.c src/cnet_vsa_ngram.c src/cnet_vsa_hybrid.c \
		tests/test_cnet_vsa_generation_comparison.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_generation_comparison | tee logs/cnet_vsa_compare_bench.log
	@grep -q "CNET_GENERATION_COMPARISON_PASS" logs/cnet_vsa_compare_bench.log

cnet_vsa_cli: src/cnet_vsa_evidence.c include/cnet_vsa_evidence.h tools/cnet_vsa_cli.c src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_gpu.hip src/cnet_vsa_device.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_capsule_swap.cpp src/cnet_vsa_sleep.c src/cnet_vsa_ast.c src/cnet_vsa_memory.c src/cnet_vsa_story.c src/cnet_vsa_ngram.c src/cnet_vsa_hybrid.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c
	@mkdir -p $(BIN_DIR)
	hipcc -O3 -march=native --offload-arch=gfx1201 -Iinclude -D_GNU_SOURCE -DCNET_HAVE_CURL=1 \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_gpu.hip src/cnet_vsa_device.c \
		src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_capsule_swap.cpp src/cnet_vsa_sleep.c \
		src/cnet_vsa_ast.c src/cnet_vsa_memory.c src/cnet_vsa_story.c \
		src/cnet_vsa_ngram.c src/cnet_vsa_hybrid.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c src/cnet_vsa_evidence.c tools/cnet_vsa_cli.c \
		-o $(BIN_DIR)/cnet_vsa_cli -lm -lpthread -lcurl

# Bounded signed-fact response operator; correctness and CLI refusal checks.
.PHONY: cnet_vsa_evidence_bench
cnet_vsa_evidence_bench: cnet_vsa_cli src/cnet_vsa_evidence.c tests/test_cnet_vsa_evidence.c include/cnet_vsa_evidence.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_evidence src/cnet_vsa_evidence.c tests/test_cnet_vsa_evidence.c
	@./$(BIN_DIR)/test_cnet_vsa_evidence
	@python3 tests/test_cnet_vsa_evidence_cli.py
	@python3 tests/test_cnet_vsa_evidence_property.py

cnet_vsa_cli_bench: cnet_vsa_cli tests/test_cnet_vsa_cli_bench.sh
	@mkdir -p logs
	@./tests/test_cnet_vsa_cli_bench.sh | tee logs/cnet_vsa_cli_bench.log
	@grep -q "CNET_VSA_CLI_BENCH_PASS" logs/cnet_vsa_cli_bench.log

cnet_vsa_gencap_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c tests/test_cnet_vsa_gencap_bench.c include/cnet_vsa_gen_capsule.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_gencap_bench \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c \
		src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c \
		tests/test_cnet_vsa_gencap_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_gencap_bench | tee logs/cnet_vsa_gencap_bench.log
	@grep -q "CNET_VSA_GENCAP_BENCH_PASS" logs/cnet_vsa_gencap_bench.log

cnet_vsa_router_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c tests/test_cnet_vsa_router_bench.c include/cnet_vsa_gen_capsule.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_router_bench \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c \
		src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c \
		tests/test_cnet_vsa_router_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_router_bench | tee logs/cnet_vsa_router_bench.log
	@grep -q "CNET_VSA_ROUTER_BENCH_PASS" logs/cnet_vsa_router_bench.log

cnet_vsa_calibration_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c tests/test_cnet_vsa_calibration_bench.c include/cnet_vsa_gen_capsule.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_calibration_bench \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c \
		src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c \
		tests/test_cnet_vsa_calibration_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_calibration_bench | tee logs/cnet_vsa_calibration_bench.log
	@grep -q "CNET_VSA_CALIBRATION_BENCH_PASS" logs/cnet_vsa_calibration_bench.log

cnet_vsa_arena_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c tests/test_cnet_vsa_arena_bench.c include/cnet_vsa_gen_capsule.h include/cnet_vsa_text.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_arena_bench \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c \
		src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c \
		tests/test_cnet_vsa_arena_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_arena_bench | tee logs/cnet_vsa_arena_bench.log
	@grep -q "CNET_VSA_ARENA_BENCH_PASS" logs/cnet_vsa_arena_bench.log

# ---- frozen routing arena: CNET encoders vs transformer embeddings on one protocol ----
ARENA_DIR := benchmarks/vsa_routing_arena_20260911
bin/cnet_vsa_arena: tools/cnet_vsa_arena.c src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c include/cnet_vsa_lexicon.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/cnet_vsa_arena \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c \
		src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c tools/cnet_vsa_arena.c $(LDFLAGS) -pthread

# Reproduces the CNET side from the frozen fixture, re-scores every cached
# transformer from its small cache (no model needed), verifies fixture hashes
# and compares all numbers to expected.json.
# The distilled row blends a transformer word context into the lexicon. Its
# input is regenerated from the tracked PCA cache (no model needed; seeded
# projection), so the gate stays reproducible offline.
ARENA_DISTILL_PCA := $(ARENA_DIR)/cache/qwen3-embedding-4b/vocab_pca256w.npz
ARENA_DISTILL := var/arena_cache/cnet/arena_distilled.dstl
# lex_full: learned phrases (8192, Random Indexing only: distilled phrase vectors measured worse) and
# learned subword backoff (specific n-grams only, half weight); lex_trained: the same table after a
# supervised pass on questions_train.tsv (teacher-written, disjoint from the frozen evaluation questions).
# --lexicon-extra: the training-register questions also feed the lex_full BUILD (vocabulary + phrases of everyday phrasing;
# the RI/distilled rows and the trainer's sentence pairs stay on the pure corpora). Never evaluation text.
ARENA_FULL_FLAGS := --phrases 8192 --subwords 16384 --subword-min-n 3 --subword-max-n 5 --subword-max-words 40 --subword-weight 0.5 --lexicon-extra $(ARENA_DIR)/questions_train_v2.tsv
ARENA_TRAIN_FLAGS := --train-questions $(ARENA_DIR)/questions_train_v2.tsv --train-sentences 16 --train-lr 0.02 --train-epochs 8 --train-margin 0.10
# Post-freeze sets (never trained on): the 88-question regression set, colloquial and contrast evaluation sets from held-out
# paraphrase families; alternates.tsv = justified multiple valid targets, applied to every system (top1_lenient).
ARENA_EVAL_FLAGS := --alternates $(ARENA_DIR)/alternates.tsv --extra-questions $(ARENA_DIR)/questions_fresh_20260912.tsv --extra-questions $(ARENA_DIR)/questions_colloquial_eval.tsv --extra-questions $(ARENA_DIR)/questions_contrast_eval.tsv --extra-questions $(ARENA_DIR)/questions_eval_mistral_everyday.tsv --extra-questions $(ARENA_DIR)/questions_eval_gemma_everyday.tsv --extra-questions $(ARENA_DIR)/questions_indep_mistral_mid_20260912.tsv
vsa_routing_arena: bin/cnet_vsa_arena tools/cnet_vsa_arena_check.py tools/cnet_vsa_arena_transformer.py tools/cnet_vsa_lexicon_distill.py
	@mkdir -p logs var/arena_cache/cnet
	@python3 tools/cnet_vsa_lexicon_distill.py --from-pca --pca-cache $(ARENA_DISTILL_PCA) --out $(ARENA_DISTILL) | tee logs/vsa_routing_arena.log
	@./$(BIN_DIR)/cnet_vsa_arena --fixture $(ARENA_DIR) --distilled $(ARENA_DISTILL) --distill-alpha 0.5 --distill-beta 1.0 --distill-pcs 16 $(ARENA_FULL_FLAGS) $(ARENA_TRAIN_FLAGS) $(ARENA_EVAL_FLAGS) | tee -a logs/vsa_routing_arena.log
	@for m in $$(ls -d $(ARENA_DIR)/cache/*/ 2>/dev/null); do \
		python3 tools/cnet_vsa_arena_transformer.py --fixture $(ARENA_DIR) --model $$(basename $$m) --score-only > /dev/null || exit 1; done
	@python3 tools/cnet_vsa_arena_check.py $(ARENA_DIR) | tee -a logs/vsa_routing_arena.log
	@grep -qE "CNET_VSA_ROUTING_ARENA_(PASS|FROZEN)" logs/vsa_routing_arena.log

# Regenerates one transformer cache from a served model, e.g.
#   make vsa_routing_arena_embed MODEL=nomic-embed-text-v1.5 URL=http://127.0.0.1:8090/v1/embeddings DOC_PREFIX='search_document: ' QUERY_PREFIX='search_query: '
vsa_routing_arena_embed:
	python3 tools/cnet_vsa_arena_transformer.py --fixture $(ARENA_DIR) --model $(MODEL) --url $(URL) --doc-prefix "$(DOC_PREFIX)" --query-prefix "$(QUERY_PREFIX)"

.PHONY: vsa_routing_arena vsa_routing_arena_embed

cnet_vsa_lexicon_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c tests/test_cnet_vsa_lexicon_bench.c include/cnet_vsa_lexicon.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_lexicon_bench \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c \
		src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c \
		tests/test_cnet_vsa_lexicon_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_lexicon_bench | tee logs/cnet_vsa_lexicon_bench.log
	@grep -q "CNET_VSA_LEXICON_BENCH_PASS" logs/cnet_vsa_lexicon_bench.log

cnet_vsa_stem_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c tests/test_cnet_vsa_stem_bench.c include/cnet_vsa_text.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_stem_bench \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c \
		tests/test_cnet_vsa_stem_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_stem_bench | tee logs/cnet_vsa_stem_bench.log
	@grep -q "CNET_VSA_STEM_BENCH_PASS" logs/cnet_vsa_stem_bench.log

cnet_vsa_encoder_sweep_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c tests/test_cnet_vsa_encoder_sweep_bench.c include/cnet_vsa_gen_capsule.h include/cnet_vsa_text.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_encoder_sweep_bench \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c \
		src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c \
		tests/test_cnet_vsa_encoder_sweep_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_encoder_sweep_bench | tee logs/cnet_vsa_encoder_sweep_bench.log
	@grep -qE "CNET_VSA_ENCODER_SWEEP_BENCH_(PASS|SKIP)" logs/cnet_vsa_encoder_sweep_bench.log

.PHONY: cnet_vsa_q8_bench
cnet_vsa_q8_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c tests/test_cnet_vsa_q8_bench.c include/cnet_vsa_gen_capsule.h include/cnet_vsa_text.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_q8_bench \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c \
		src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c tests/test_cnet_vsa_q8_bench.c \
		-Wl,--wrap=cnet_vsa_text_q8_norm $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_q8_bench | tee logs/cnet_vsa_q8_bench.log
	@grep -q "CNET_VSA_Q8_BENCH_PASS" logs/cnet_vsa_q8_bench.log

cnet_vsa_answer_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c tests/test_cnet_vsa_answer_bench.c include/cnet_vsa_gen_capsule.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_answer_bench \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c \
		src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c \
		tests/test_cnet_vsa_answer_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_answer_bench | tee logs/cnet_vsa_answer_bench.log
	@grep -q "CNET_VSA_ANSWER_BENCH_PASS" logs/cnet_vsa_answer_bench.log

cnet_vsa_delta_bench: src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c tests/test_cnet_vsa_delta_bench.c include/cnet_vsa_delta.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/test_cnet_vsa_delta_bench \
		src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_lexicon.c src/cnet_vsa_memory.c \
		src/cnet_vsa_ngram.c src/cnet_vsa_delta.c src/cnet_vsa_gen_capsule.c \
		tests/test_cnet_vsa_delta_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_vsa_delta_bench | tee logs/cnet_vsa_delta_bench.log
	@grep -q "CNET_VSA_DELTA_BENCH_PASS" logs/cnet_vsa_delta_bench.log

cnet_vsa_all_bench: cnet_vsa_evidence_bench cnet_vsa_bench cnet_vsa_simd_bench cnet_vsa_index_bench cnet_vsa_reason_bench cnet_vsa_doc_graft_bench cnet_vsa_gpu_bench cnet_vsa_device_bench cnet_vsa_text_bench cnet_vsa_capsule_swap_bench cnet_vsa_sleep_bench cnet_vsa_ast_bench cnet_vsa_story_bench cnet_vsa_compare_bench cnet_vsa_gencap_bench cnet_vsa_router_bench cnet_vsa_calibration_bench cnet_vsa_arena_bench cnet_vsa_stem_bench cnet_vsa_lexicon_bench cnet_vsa_encoder_sweep_bench cnet_vsa_cli_bench cnet_vsa_q8_bench cnet_vsa_answer_bench cnet_vsa_delta_bench
	@echo "\n================================================================="
	@echo " ALL CNET-VSA BENCHMARKS (CLI INCLUDED) PASSED"
	@echo "================================================================="
