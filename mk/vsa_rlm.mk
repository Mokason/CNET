# mk/vsa_rlm.mk -- recurrent language model trainer (Gated DeltaNet / RWKV-7 / state-space mixers), C, CPU + OpenMP.
RLM_SRC := src/cnet_vsa_rlm.c src/cnet_vsa_rlm_score.c
.PHONY: cnet_vsa_rlm cnet_vsa_rlm_bench
# gate: double precision for the finite-difference gradient check
cnet_vsa_rlm_bench: cnet_vsa_rlm $(RLM_SRC) tests/test_cnet_vsa_rlm_bench.c tests/test_cnet_vsa_rlm_cli.sh include/cnet_vsa_rlm.h src/cnet_vsa_rlm_score.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -fopenmp-simd -DCNET_VSA_RLM_SIMD -Iinclude -DRLM_REAL=double -o $(BIN_DIR)/test_cnet_vsa_rlm_bench $(RLM_SRC) tests/test_cnet_vsa_rlm_bench.c $(LDFLAGS)
	@sh tests/test_cnet_vsa_rlm_cli.sh $(BIN_DIR)/cnet_vsa_rlm
	@./$(BIN_DIR)/test_cnet_vsa_rlm_bench | tee logs/cnet_vsa_rlm_bench.log
	@grep -q "CNET_VSA_RLM_BENCH_PASS" logs/cnet_vsa_rlm_bench.log
# trainer / generator tool: single precision, OpenMP
cnet_vsa_rlm: $(RLM_SRC) tools/cnet_vsa_rlm_train.c include/cnet_vsa_rlm.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -Werror -fopenmp -Iinclude -o $(BIN_DIR)/cnet_vsa_rlm $(RLM_SRC) tools/cnet_vsa_rlm_train.c $(LDFLAGS)
# synthetic associative-recall benchmark (memory writing and erasure), not a gate
.PHONY: cnet_vsa_rlm_recall
cnet_vsa_rlm_recall: $(RLM_SRC) tools/cnet_vsa_rlm_recall.c include/cnet_vsa_rlm.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -Werror -fopenmp -Iinclude -o $(BIN_DIR)/cnet_vsa_rlm_recall $(RLM_SRC) tools/cnet_vsa_rlm_recall.c $(LDFLAGS)
