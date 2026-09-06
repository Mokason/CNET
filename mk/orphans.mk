# mk/orphans.mk -- targets for tests and tools that previously had none.
#
# Included from the end of the root Makefile, after every variable is defined.
# See mk/README.md for why this directory exists.

# =============================================================================
# PREVIOUSLY-ORPHANED TESTS, AND THE GATES THAT KEEP THEM WIRED
#
# Twenty-six files in tests/ had no rule to build them. That is invisible in a
# file this size, and it cost the project its single strongest claim: README.md
# and docs/ARCHITECTURE.md both cite `clgemm_unit` as the executable proof that
# the multi-GPU OpenCL forward is bit-identical, and `make clgemm_unit` answered
# "No rule to make target". cce_archive_test and cce_forest_test -- the two
# storage layers in the CCE architecture table -- were in the same state.
#
# `make orphan_tests` now fails if any test file is unbuildable, so this cannot
# silently recur. `make curl_guard` and `make platform_sweep` do the same for
# the two build breaks that preceded them.
# =============================================================================

# cce_aicimo_bridge.c was itself orphaned: not in $(CCE), not named by any rule,
# and the only definition of cce_aicimo_expand_context* that eight aicimo tests
# link against.
CCE_AICIMO_EXTRA := src/cce/cce_aicimo_bridge.c src/cce/cce_aicimo_role_slice.c

# --- the gate the docs already cite -------------------------------------------
# 360 GEMM calls against an exact CPU reference, single / dual / column-split,
# FP_CONTRACT OFF. Runs on CPU when no OpenCL device is present.
clgemm_unit: $(LIBCCE) tests/clgemm_unit.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ tests/clgemm_unit.c $(LIBCCE) $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/clgemm_unit > logs/clgemm_unit.log 2>&1

# --- CCE storage layers named in the architecture table -----------------------
cce_archive: $(LIBCCE) tests/cce_archive_test.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ tests/cce_archive_test.c $(LIBCCE) $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_archive > logs/cce_archive.log 2>&1

cce_forest: $(LIBCCE) tests/cce_forest_test.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ tests/cce_forest_test.c $(LIBCCE) $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_forest > logs/cce_forest.log 2>&1

cce_minimal: $(LIBCCE) tests/cce_minimal.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ tests/cce_minimal.c $(LIBCCE) $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_minimal > logs/cce_minimal.log 2>&1

forest_ls: $(LIBCCE) tests/forest_ls.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ tests/forest_ls.c $(LIBCCE) $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

qwythos_load: $(LIBCCE) tests/qwythos_load.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ tests/qwythos_load.c $(LIBCCE) $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

gguf_partial_convert: $(LIBCCE) tests/gguf_partial_convert.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ tests/gguf_partial_convert.c $(LIBCCE) $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

int8_to_trit: $(LIBCCE) tests/int8_to_trit.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ tests/int8_to_trit.c $(LIBCCE) $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

# --- mmap/fread byte-identity (hop-1 invariant) -------------------------------
# Built as C++ on purpose: the minimal GGUF parser inside it uses reference
# parameters, exactly as its own header comment documents. Needs a real GGUF to
# compare against; without mmap it prints an explicit SKIP rather than a pass.
mmap_read_identity: tests/test_mmap_read_identity.c
	@mkdir -p $(BIN_DIR) logs
	$(CXX) -x c++ -O2 -D_FILE_OFFSET_BITS=64 -DCNET_HAVE_CURL=$(CNET_HAVE_CURL) \
		-include include/cnet_platform.h -Iinclude \
		-o $(BIN_DIR)/test_mmap_read_identity tests/test_mmap_read_identity.c -lm
	./$(BIN_DIR)/test_mmap_read_identity > logs/mmap_read_identity.log 2>&1
	@grep -qE 'HOP1 INVARIANT OK|HOP1 SKIP' logs/mmap_read_identity.log

# --- core-linked studies ------------------------------------------------------
circuit_attention_study: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(TOPOLOGY) tests/circuit_attention_study.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(TOPOLOGY) tests/circuit_attention_study.c $(LDFLAGS)

cnet_evolve_dir_test: include/cnet_evolve_dir.h src/selfimprove/cnet_evolve_dir.c tests/test_cnet_evolve_dir.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Iinclude -o $(BIN_DIR)/test_cnet_evolve_dir \
		tests/test_cnet_evolve_dir.c src/selfimprove/cnet_evolve_dir.c $(LDFLAGS)
	./$(BIN_DIR)/test_cnet_evolve_dir > logs/cnet_evolve_dir_test.log 2>&1

time_teacher_learn: $(GENERATE_FIFO_SRC) $(HYBRID_AI_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(LIBCCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/time_teacher_learn.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ \
		$(GENERATE_FIFO_SRC) $(HYBRID_AI_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(LIBCCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/time_teacher_learn.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

# --- aicimo scenario suite ----------------------------------------------------
# Thirteen files, one rule. $@ is the test name, so the source is tests/$@.c.
AICIMO_ORPHANS := aicimo_128k_rpg_test aicimo_8192_rpg_test aicimo_bridge_rpg_test \
	aicimo_complete_solution_test aicimo_final_solution_test aicimo_full_rpg_test \
	aicimo_moral_ambiguity_test aicimo_place_dilemma_test aicimo_quest_test \
	aicimo_rpg_dilemma_test aicimo_rpg_test aicimo_sandbox_quest_test \
	aicimo_uncertainty_rpg_test

$(AICIMO_ORPHANS): %: tests/%.c $(LIBCCE) $(CCE_AICIMO_EXTRA)
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -Iinclude -o $(BIN_DIR)/$@ \
		$(LIBCCE) $(CCE_AICIMO_EXTRA) tests/$@.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

.PHONY: aicimo_orphans
aicimo_orphans: $(AICIMO_ORPHANS)
	@echo AICIMO_ORPHANS_BUILT


# =============================================================================
# PREVIOUSLY-ORPHANED TOOLS
#
# tools/ had the same problem tests/ had: six files with no rule to build them,
# invisible in an 8,530-line Makefile. Four were live code that simply lost its
# target; two were superseded fixture generations and now live in attic/.
# `make orphan_tools` keeps the list at zero.
# =============================================================================

# Standalone: no project headers, links against libc and libm only.
governor_v4_ext: tools/governor_v4_ext.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ tools/governor_v4_ext.c $(LDFLAGS)

# CPU study: compares memorising n-grams with the tiny neural word model on
# whole held-out sentences. It reports observations only; ANTIPARROT_DONE is a
# completion marker, not a certification claim.
.PHONY: cnet_lm_antiparrot
cnet_lm_antiparrot: tools/cnet_lm_antiparrot.c src/cce/cce_wordlm.c \
		src/cce/cce_ngram.c include/cce/cce_wordlm.h include/cce/cce_ngram.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/$@ \
		tools/cnet_lm_antiparrot.c src/cce/cce_wordlm.c \
		src/cce/cce_ngram.c $(LDFLAGS)
	./$(BIN_DIR)/$@ | tee logs/cnet_lm_antiparrot.log
	@grep -q '^ANTIPARROT_DONE' logs/cnet_lm_antiparrot.log

# Small executable wrapper around the operator-curation self-test.
.PHONY: cnet_roe_gold_gate
cnet_roe_gold_gate: tools/cnet_roe_gold_gate.c src/cnet_roe_gold.c \
		include/cnet_roe_gold.h include/cnet_roe_gold_id.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/$@ \
		tools/cnet_roe_gold_gate.c src/cnet_roe_gold.c $(LDFLAGS)
	./$(BIN_DIR)/$@ | tee logs/cnet_roe_gold_gate.log

# Drives the compete runtime from the command line. Same link set as the
# chat1 suites, which are the other consumers of cnet_compete_runtime.c.
cnet_compete_run: include/cnet_compete_runtime.h src/compete/cnet_compete_runtime.c \
		tools/cnet_compete_run.c $(STRUCT_MINE_LINK)
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -Iinclude -o $(BIN_DIR)/$@ \
		$(STRUCT_MINE_LINK) $(CAPSULE_SRC) src/compete/cnet_compete_runtime.c \
		src/compete/cnet_compete_intent.c src/compete/cnet_compete_capsules.c \
		tools/cnet_compete_run.c \
		$(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread

# Both need the full serving stack, exactly as struct_mine_persist does.
STRUCT_MINE_LINK := $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(LIBCCE) $(CNET_CCE_ADAPTER) \
	$(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(REGISTRY_LORA) \
	$(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) \
	$(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) \
	$(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) \
	$(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) \
	src/soul_host.c $(ROUTE_LOG_SRC)

soul_residual_probe: json_toolcall_alphabet $(STRUCT_MINE_LINK) tools/soul_residual_probe.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ \
		$(STRUCT_MINE_LINK) tools/soul_residual_probe.c \
		$(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread

struct_mine_large_test: json_toolcall_alphabet $(STRUCT_MINE_LINK) tools/struct_mine_large_test.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ \
		$(STRUCT_MINE_LINK) tools/struct_mine_large_test.c \
		$(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread

.PHONY: orphan_tools
orphan_tools:
	@mkdir -p logs
	@sh tests/orphan_tools.sh 2>&1 | tee logs/orphan_tools.log
	@grep -q '^ORPHAN_TOOLS_PASS' logs/orphan_tools.log
