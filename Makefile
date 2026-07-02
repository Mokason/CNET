CC := gcc
# -march=native: measured 1.25x on the training loops, bit-identical weights
# (FP contraction stays off under -std=c11). Drop it for portable binaries.
# -mno-avx is REQUIRED with -march=native on this toolchain: MinGW gcc 15.2
# emits aligned 256-bit moves for by-value structs >= 32 bytes (Port is 56)
# on a 16-byte-aligned Windows stack -> segfault. -mstackrealign does not fix
# it; disabling AVX does.
CFLAGS := -std=c11 -Wall -Wextra -pedantic -O3 -march=native -mno-avx
LDFLAGS := -lm
MCP_LDFLAGS :=
ifeq ($(OS),Windows_NT)
MCP_LDFLAGS := -lwininet
endif

# OpenMP support for multi-threaded studies inside a single exe (MinGW GCC).
# Follows strict rules: default(none) on all pragmas, OMP_NUM_THREADS=1 for
# isolating races, etc. Only study targets get -fopenmp.
OMPFLAGS := -fopenmp

SRC := src/nn.c
# Router split (SRP).
# Router sources in src/router/ (filenames without router_ prefix for cleanliness)
# Contract sources in src/contract/ (filenames without contract_ prefix)
ROUTER := src/router/artifacts.c src/router/attention.c src/router/dag_exec.c src/router/dag_plan.c src/router/registry.c src/router/route.c
CONSOLIDATE := src/consolidate.c
PLAN_TABLE := src/plan_table.c
TOPOLOGY := src/topology.c
HYPERBOLIC := src/hyperbolic.c
APP := src/main.c
TEST := tests/test_nn.c
OOB_TEST := tests/test_encode_oob.c
CONTRACT_TEST := tests/test_contract.c
COMPOSE_TEST := tests/test_composition.c
ROUTER_TEST := tests/test_router.c
ROUTE_DEMO := tests/route_demo.c
DAG_TEST := tests/test_dag.c
DAG_DEMO := tests/dag_demo.c
CHUNK_TEST := tests/test_consolidate.c
CHUNK_DEMO := tests/chunk_demo.c
HETERO_DEMO := tests/hetero_demo.c
SPLIT_DEMO := tests/split_demo.c
CONTRACT := src/contract/contract.c src/contract/unit.c
CCE_TENSOR  := src/cce/cce_tensor.c
CCE_BLOCK   := src/cce/cce_block.c
CCE_CASCADE := src/cce/cce_cascade.c
CCE_ARCHIVE := src/cce/cce_archive.c
CCE_FOREST  := src/cce/cce_forest.c
CCE_ROUTER  := src/cce/cce_router.c
CCE_LEARN   := src/cce/cce_learn.c
CCE_PATCH   := src/cce/cce_block_patch.c
CCE_GPU     := src/cce/cce_gpu.c
CCE_WORDLM  := src/cce/cce_wordlm.c
CCE_PERCEPTUAL := src/cce/cce_perceptual_leaf.c
CCE_MODEL := src/cce/cce_model.c
CCE_MODEL_IO := src/cce/cce_model_io.c
CCE_DATASET := src/cce/cce_dataset.c
CCE_AUTOGRAD := src/cce/cce_autograd.c src/cce/cce_autograd_ops.c
CCE_SAFETENSORS := src/cce/cce_safetensors.c
CCE_GGUF := src/cce/cce_gguf.c

# Build directory for all executables to avoid polluting the root with endless .exe junk.
# Same philosophy as the fixed-temp cleanup for .cce / logs.
BIN_DIR := bin
EXE_EXT := $(if $(filter Windows_NT,$(OS)),.exe,)

# Auto-create bin/ on make invocation (prevents "No such file or directory" for -o bin/...)
# POSIX sh syntax: make runs recipes/$(shell) through sh even on Windows (Git Bash).
$(shell mkdir -p $(BIN_DIR) logs)

# CUDA support is strictly optional.
# Build with: make CCE_USE_CUDA=1
# You must have the CUDA Toolkit installed and nvcc in PATH.
# This does NOT make CUDA mandatory at runtime.
ifeq ($(CCE_USE_CUDA),1)
  NVCC := $(shell which nvcc 2>/dev/null)
  ifneq ($(NVCC),)
    HAVE_CUDA := 1
  endif
endif

ifdef HAVE_CUDA
  CCE_CUDA_OBJ := src/cce/cce_cuda.o
  CUDA_CFLAGS  := -DC CE_HAVE_CUDA
  CUDA_LDFLAGS := -lcudart -lcublas
else
  CCE_CUDA_OBJ :=
  CUDA_CFLAGS  :=
  CUDA_LDFLAGS :=
endif

CCE_ABI     := src/cce/cce_abi.c
CCE_DETECT  := src/cce/cce_detect.c
CCE_SSM     := src/cce/cce_ssm.c
CCE_ST_LLAMA := src/cce/cce_st_llama.c
CCE_SPECGRAPH := src/cce/cce_specgraph.c
CCE_WSTORE  := src/cce/cce_weight_store.c
CCE_TIERRT  := src/cce/cce_tier_runtime.c
CCE_SIMILAR := src/cce/cce_similar.c
CCE := $(CCE_TENSOR) $(CCE_BLOCK) $(CCE_CASCADE) $(CCE_ARCHIVE) $(CCE_FOREST) $(CCE_ROUTER) $(CCE_LEARN) $(CCE_PATCH) $(CCE_GPU) $(CCE_ABI) $(CCE_CUDA_OBJ) $(CCE_PERCEPTUAL) $(CCE_WORDLM) $(CCE_MODEL) $(CCE_MODEL_IO) $(CCE_DATASET) $(CCE_AUTOGRAD) $(CCE_SAFETENSORS) $(CCE_GGUF) $(CCE_DETECT) $(CCE_SSM) $(CCE_ST_LLAMA) $(CCE_SPECGRAPH) $(CCE_WSTORE) $(CCE_TIERRT) $(CCE_SIMILAR)
SCAN := src/scan.c
CERTIFY_TEST := tests/test_certify.c
CERTIFY_DEMO := tests/certify_demo.c
PROPERTY := src/property.c
PROPERTY_TEST := tests/test_property.c
PROPERTY_DEMO := tests/property_demo.c
COVERAGE := src/contract/coverage.c
COVERAGE_TEST := tests/test_coverage.c
CONFORMAL := src/contract/conformal.c
CONFORMAL_TEST := tests/test_conformal.c
LOGICGATE := src/logic_gate_net.c
LOGICGATE_TEST := tests/test_logic_gate.c
DECIMAL_DEMO := tests/decimal_demo.c
DECIMAL_TEST := tests/test_decimal.c
CIRCUIT_TEST := tests/test_circuit.c
CIRCUIT_DEMO := tests/circuit_demo.c
CAPACITY_STUDY := tests/capacity_study.c
CAPACITY_DEMO := tests/capacity_demo.c
LIBRARY := src/library.c
LIBRARY_TEST := tests/test_library.c
MARGIN_STUDY := tests/margin_study.c
FUZZY_STUDY := tests/fuzzy_study.c
STOCHASTIC_STUDY := tests/stochastic_study.c
RESIDUE_TEST := tests/test_residue.c
RESIDUE_STUDY := tests/residue_study.c
PROPOSAL_SIDECAR := src/proposal_sidecar.c
PROPOSAL_SIDECAR_TEST := tests/test_proposal_sidecar.c
BELOW_BEAM_TEST := tests/test_below_beam_recovery.c
BELOWBEAM_CHARS_TEST := tests/test_belowbeam_chars.c
BELOWBEAM_CHARS_STUDY := tests/belowbeam_chars_study.c
STRUCT_PREF_TEST := tests/test_structural_pref.c
STRUCT_PREF_B0_TEST := tests/test_structural_pref_b0.c
STRUCT_PREF_STUDY := tests/structural_pref_study.c
EXPR_TEST := tests/test_expr.c
FASTPATH := src/fastpath.c
FASTPATH_TEST := tests/test_fastpath.c
THROUGHPUT_STUDY := tests/throughput_study.c
LIFECYCLE_BENCH := tests/lifecycle_bench.c
PROBE_OVERHEAD_BENCH := tests/probe_overhead_bench.c
DGATE_BENCH := tests/distillation_gate_bench.c
CMPND_BENCH := tests/benchmark_compounding_loop.c
JSONSTORY_DEMO := tests/jsonstory_demo.c
PDF_SRC := src/pdf/inflate.c src/pdf/pdf_extract.c src/pdf/font_decode.c
CORPUS_SRC := src/corpus/corpus_split.c src/corpus/corpus_store.c
PDF_TEST := tests/test_pdf.c
PDFLEARN_DEMO := tests/pdflearn_demo.c
RETRIEVAL_SRC := src/corpus/retrieval.c
COMPOUND_DEMO := tests/compound_demo.c
TILEMEM_SRC := src/corpus/tile_memory.c
TILEMEM_TEST := tests/test_tile_memory.c
GRADUATE_SRC := src/corpus/graduate.c
GRADUATE_TEST := tests/test_graduate.c
ACQUIRE_SRC := src/acquire.c
ACQUIRE_TEST := tests/test_acquire.c
FONTDECODE_TEST := tests/test_font_decode.c
TFIDF_TEST := tests/test_tfidf.c
SYNONYMS_SRC := src/corpus/synonyms.c
SYNONYMS_TEST := tests/test_synonyms.c
TILEINDEX_TEST := tests/test_tile_index.c
CONSOLIDATE_TEST := tests/test_tile_consolidate.c

.PHONY: all run test verify verify-long legacy_test compose route dag hetero split chunk certify property coverage conformal logicgate decimal circuit study capacity library margin fuzzy stochastic fastpath throughput residue expr attention attention_study lifecycle_bench lbench proposal_sidecar probe_overhead belowbeam_chars struct_pref dgate_bench compounding_bench cce_smoke cce_train_bench cce_view forest_view wordlm wordlm_bitnet cce_dll cce_safetensors_test cce_gguf_test cce_model_test cce_autograd_test endgate jsonstory pdftest pdflearn compound tiermem_test graduate fontdecode tfidf synonyms tileindex consolidate clean

all: nn_demo

nn_demo: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(APP) include/nn.h include/router.h include/plan_table.h include/contract/contract.h include/scan.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(APP) $(LDFLAGS)

# Generate compile-time const data from the committed .txt files.
# This is the start of replacing runtime text parsing for weights/contracts.
# Run: make freeze
# It produces include/generated.h (or stdout). The generated data can be
# used via btn_init_frozen / btn_init_committed for zero-I/O committed primitives.
freeze: nn_demo
	./$(BIN_DIR)/nn_demo --freeze > include/generated.h
	@echo "Generated include/generated.h from committed txt files."
	@echo "You can now #include it and use btn_init_committed(btn, \"hex_value\"); etc."

test_nn: $(SRC) $(TEST) include/nn.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(TEST) $(LDFLAGS)

# Includes src/nn.c directly to reach the static encoder, so it is NOT
# compiled together with $(SRC) (that would duplicate symbols).
test_encode_oob: $(SRC) $(OOB_TEST) include/nn.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(OOB_TEST) $(LDFLAGS)

# Composes two independently-frozen primitives loaded from disk.
test_composition: $(SRC) $(COMPOSE_TEST) include/nn.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(COMPOSE_TEST) $(LDFLAGS)

test_contract: $(SRC) $(CONTRACT_TEST) include/nn.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(CONTRACT_TEST) $(LDFLAGS)

# router.c depends on contract.c (registry_save synthesizes + persists
# contracts), which in turn needs plan_table.c -- so every target that links
# router.c links those two as well.
test_router: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(ROUTER_TEST) include/nn.h include/router.h include/plan_table.h include/contract/contract.h include/scan.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(ROUTER_TEST) $(LDFLAGS)

test_dag: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(DAG_TEST) include/nn.h include/router.h include/plan_table.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(DAG_TEST) $(LDFLAGS)

# Contract-graph topology audit (Betti-0/1, "what to mint next", dedup).
# Pure observability over the registry; zero authority. See src/topology.c.
test_topology: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(TOPOLOGY) tests/test_topology.c include/nn.h include/router.h include/topology.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(TOPOLOGY) tests/test_topology.c $(LDFLAGS)
	./$(BIN_DIR)/test_topology

# Hyperbolic (Poincaré-ball) embedding for generalizing routing priors across
# SIMILAR tasks (advisory only). See src/hyperbolic.c.
test_hyperbolic: $(SRC) $(HYPERBOLIC) tests/test_hyperbolic.c include/nn.h include/hyperbolic.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(HYPERBOLIC) tests/test_hyperbolic.c $(LDFLAGS)
	./$(BIN_DIR)/test_hyperbolic

# Frozen-library load regression: every committed primitive must load with valid
# ports + nonzero dims (guards the btn_init_frozen lr=0 bug). See test_frozen_library.c.
test_frozen_library: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) tests/test_frozen_library.c include/nn.h include/contract/contract.h include/generated.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) tests/test_frozen_library.c $(LDFLAGS)
	./$(BIN_DIR)/test_frozen_library

# Topology JSON invariants: golden summary of the live audit + artifact schema.
test_topology_json: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(TOPOLOGY) tests/test_topology_json.c include/nn.h include/router.h include/topology.h include/generated.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(TOPOLOGY) tests/test_topology_json.c $(LDFLAGS)
	./$(BIN_DIR)/test_topology_json

test_consolidate: $(SRC) $(ROUTER) $(CONSOLIDATE) $(PLAN_TABLE) $(CONTRACT) $(CHUNK_TEST) include/nn.h include/router.h include/consolidate.h include/plan_table.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(CONSOLIDATE) $(PLAN_TABLE) $(CONTRACT) $(CHUNK_TEST) $(LDFLAGS)

# library_evolve: re-plan a task list, distill proven plans into certified
# chunks, dedup by contract, law-guard with rollback. Self-contained TDD.
test_library: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(PROPERTY) $(LIBRARY) $(LIBRARY_TEST) include/nn.h include/router.h include/plan_table.h include/consolidate.h include/contract/contract.h include/property.h include/library.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(PROPERTY) $(LIBRARY) $(LIBRARY_TEST) $(LDFLAGS)

# Builds and runs the library_evolve test directly.
library: test_library
	./$(BIN_DIR)/test_library

# Unit files: weights + contract as ONE sealed binary artifact (.cnu) —
# binary f64 weights + bit-packed canonical exemplars + FNV seal; round-trip
# gated by digest identity, tamper refused, smaller than the text pair.
contract_unit: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) tests/test_unit.c include/nn.h include/router.h include/contract/contract.h include/contract/unit.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) tests/test_unit.c $(LDFLAGS)
	./$(BIN_DIR)/contract_unit > logs/contract_unit.log 2>&1 || echo "test exited non-zero (see log)"

# Gap-triggered acquisition loop: gap ledger sidecar + oracle mining ->
# train -> certify (PROOF/SAMPLE) -> seal .cnu -> register -> replan.
# Link set mirrors the `coverage` target (+ acquire).
acquire: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(ACQUIRE_TEST) include/nn.h include/router.h include/contract/contract.h include/contract/coverage.h include/contract/unit.h include/acquire.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(ACQUIRE_TEST) $(LDFLAGS)
	./$(BIN_DIR)/acquire > logs/acquire.log 2>&1 || echo "test exited non-zero (see log)"

# Contract security + efficiency: content digests, certification cache,
# sealed (tamper-evident) contract files, certificate-to-weights binding.
contract_secure: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) tests/test_contract_secure.c include/nn.h include/router.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) tests/test_contract_secure.c $(LDFLAGS)
	./$(BIN_DIR)/contract_secure > logs/contract_secure.log 2>&1 || echo "test exited non-zero (see log)"

test_certify: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(CERTIFY_TEST) include/nn.h include/router.h include/plan_table.h include/consolidate.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(CERTIFY_TEST) $(LDFLAGS)

# Auto-discovers and runs a chain over real frozen primitives.
route_demo: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(TOPOLOGY) $(ROUTE_DEMO) include/nn.h include/router.h include/plan_table.h include/contract/contract.h include/topology.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(TOPOLOGY) $(ROUTE_DEMO) $(LDFLAGS)

# Auto-discovers and runs a branching DAG over real frozen primitives.
dag_demo: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(DAG_DEMO) include/nn.h include/router.h include/plan_table.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(DAG_DEMO) $(LDFLAGS)

# Auto-discovers and runs a heterogeneous multi-input DAG.
hetero_demo: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(HETERO_DEMO) include/nn.h include/router.h include/plan_table.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(HETERO_DEMO) $(LDFLAGS)

# Auto-discovers and runs a DAG that selects a multi-output primitive's port.
split_demo: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SPLIT_DEMO) include/nn.h include/router.h include/plan_table.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SPLIT_DEMO) $(LDFLAGS)

# Distills proven plans into single chunk primitives and replans through them.
chunk_demo: $(SRC) $(ROUTER) $(CONSOLIDATE) $(PLAN_TABLE) $(CONTRACT) $(CHUNK_DEMO) include/nn.h include/router.h include/consolidate.h include/plan_table.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(CONSOLIDATE) $(PLAN_TABLE) $(CONTRACT) $(CHUNK_DEMO) $(LDFLAGS)

test_property: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(PROPERTY_TEST) include/nn.h include/router.h include/plan_table.h include/contract/contract.h include/property.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(PROPERTY_TEST) $(LDFLAGS)

# Hermetic tag-safety checks plus exhaustive sweeps over the committed
# frozen decimal weights (regenerate those with make decimal).
test_decimal: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(DECIMAL_TEST) include/nn.h include/router.h include/plan_table.h include/contract/contract.h include/property.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(DECIMAL_TEST) $(LDFLAGS)

# Shared nodes, multi-root circuits, reachability pruning, circuit chunks.
test_circuit: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(CIRCUIT_TEST) include/nn.h include/router.h include/plan_table.h include/consolidate.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(CIRCUIT_TEST) $(LDFLAGS)

# The fast lane: an independent route executor (packed weights, reused scratch,
# batched matmul, per-handoff snap) checked against route_execute. Loads the
# committed frozen hex_value/increment weights.
test_fastpath: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(FASTPATH) $(FASTPATH_TEST) include/nn.h include/router.h include/fastpath.h include/plan_table.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(FASTPATH) $(FASTPATH_TEST) $(LDFLAGS)

# The mod-k residue domain: delta certifies exactly + a short scan matches
# ground truth (the correctness gate). residue_common.h holds shared statics.
# (Full residue_study was planned but never implemented; only the test gate exists.)
test_residue: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(RESIDUE_TEST) include/nn.h include/router.h include/contract/contract.h include/plan_table.h include/scan.h tests/residue_common.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(RESIDUE_TEST) $(LDFLAGS)

# v5.0 Proposal Sidecar (SHADOW_ONLY): a scripted proposer + strict-validation
# harness over the residue fixture, proving the imagination lane has zero
# authority (no registry/stats/planner footprint). Hermetic; part of make test.
# Shares residue_common.h, so -Wno-unused-function like test_residue.
test_proposal_sidecar: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(PROPOSAL_SIDECAR) $(PROPOSAL_SIDECAR_TEST) include/nn.h include/router.h include/contract/contract.h include/plan_table.h include/scan.h include/proposal_sidecar.h tests/residue_common.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(PROPOSAL_SIDECAR) $(PROPOSAL_SIDECAR_TEST) $(LDFLAGS)

# v5.1 Below-Beam Recovery (SHADOW_ONLY, detect + report): a shadow probe that
# detects the planner's beam blind spot (a correct producer ranked below the
# beam cutoff) and reports it WITHOUT acting. Zero authority; Delta-4 carries
# forward. Shares residue_common.h, so -Wno-unused-function like test_residue.
test_below_beam_recovery: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(PROPOSAL_SIDECAR) $(BELOW_BEAM_TEST) include/nn.h include/router.h include/contract/contract.h include/plan_table.h include/scan.h include/proposal_sidecar.h tests/residue_common.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(PROPOSAL_SIDECAR) $(BELOW_BEAM_TEST) $(LDFLAGS)

# Below-Beam Failure-Mode Characterization -- thin hermetic ANCHOR (the (b)
# part, in make test). One deterministic fixture per cause (cold-start +
# rank-poisoning) asserting the v5.1 probe detects the blind spot + the
# no-authority/Delta-4 invariants. Mirrors test_below_beam_recovery. Shares
# belowbeam_chars_common.h (-> residue_common.h), so -Wno-unused-function.
test_belowbeam_chars: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(PROPOSAL_SIDECAR) $(BELOWBEAM_CHARS_TEST) include/nn.h include/router.h include/contract/contract.h include/plan_table.h include/scan.h include/proposal_sidecar.h tests/residue_common.h tests/belowbeam_chars_common.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(PROPOSAL_SIDECAR) $(BELOWBEAM_CHARS_TEST) $(LDFLAGS)

# The budgeted Below-Beam Characterization SWEEP study: builds blind spots from
# the two causes (cold-start, rank-poisoning), sweeps each cause's knob over
# beams {2,4,8}, reuses the v5.1 probe, and reports the reliability-margin
# boundary + the rank-gap distribution (printed table + artifacts/belowbeam_chars/
# belowbeam_chars.csv). NOT part of make test. Shares belowbeam_chars_common.h.
belowbeam_chars_study: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(PROPOSAL_SIDECAR) $(BELOWBEAM_CHARS_STUDY) include/nn.h include/router.h include/contract/contract.h include/plan_table.h include/scan.h include/proposal_sidecar.h tests/residue_common.h tests/belowbeam_chars_common.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(PROPOSAL_SIDECAR) $(BELOWBEAM_CHARS_STUDY) $(LDFLAGS)

belowbeam_chars: belowbeam_chars_study
	./$(BIN_DIR)/belowbeam_chars_study

# Structural Preference Sidecar -- Derivation Lock (SHADOW_ONLY): thin hermetic
# ANCHOR. Among already-valid single-root structures, re-derive the same dag_plan
# task under candidate-order + search-depth perturbations and check whether the
# same canonical plan digest reappears (attractor -> D=1/residual 0; lure ->
# D>=2/residual>0); asserts the production registry + BTN counters stay
# byte-identical (perturbations touch only COPIES). Mirrors
# test_below_beam_recovery; shares structural_pref_common.h, so
# -Wno-unused-function. Part of make test (wired into test_all).
test_structural_pref: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(STRUCT_PREF_TEST) include/nn.h include/router.h include/plan_table.h include/contract/contract.h include/scan.h tests/structural_pref_common.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(STRUCT_PREF_TEST) $(LDFLAGS)

# Structural Preference B0 -- the opt-in ORDER_ONLY gate-probe ANCHOR. Freezes
# A's structural ranking into a CircuitRankArtifact and consumes it through the
# EXISTING v2.2 ORDER_ONLY path as an opt-in additive traversal bias; checks
# whether the lure's selected producer flips alt_a->alt_b (a FALSIFIABLE probe),
# the full set stays reachable, zero authority, and the production reg + BTN
# counters stay byte-identical (default runtime stays attention_mode=OFF).
# TEST-ONLY: no src/ edits. Shares structural_pref_common.h -> -Wno-unused-function.
test_structural_pref_b0: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(STRUCT_PREF_B0_TEST) include/nn.h include/router.h include/plan_table.h include/contract/contract.h include/scan.h tests/structural_pref_common.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(STRUCT_PREF_B0_TEST) $(LDFLAGS)

# The budgeted Structural Preference (Derivation Lock) SWEEP study: runs the
# full perturbation grid (K candidate-order perms x beam {1,2,4,8,0} x memo
# {off,on}) over both fixtures, reports baseline_digest/K/R/D/residual/score +
# the no-authority flags, and writes a per-cell CSV under artifacts/structural_pref/.
# NOT part of make test. Shares structural_pref_common.h.
structural_pref_study: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(STRUCT_PREF_STUDY) include/nn.h include/router.h include/plan_table.h include/contract/contract.h tests/structural_pref_common.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(STRUCT_PREF_STUDY) $(LDFLAGS)

struct_pref: structural_pref_study
	./$(BIN_DIR)/structural_pref_study

# v4.4 Fourth Domain Transfer (4x4 block + 4-domain mixed on frozen frontier surface)
glyph_habitat: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CCE) tests/glyph_habitat.c src/contract/text_add.c src/contract/text_add_compound.c src/contract/text_add_abstain.c src/contract/perceptual_query.c src/contract/narrative_diffusion.c src/contract/narrative_branching.c src/contract/interactive_agent.c src/contract/mcp_utils.c src/contract/mcp_memory.c src/contract/mcp_wiki.c src/contract/mcp_web_search.c src/contract/mcp_file_read.c src/contract/mcp_calculator.c src/contract/mcp_summarizer.c src/contract/mcp_file_write.c src/agent_memory.c src/contract/book_concept.c include/nn.h include/router.h include/plan_table.h include/contract/contract.h include/contract/text_add.h include/contract/text_add_compound.h include/contract/text_add_abstain.h include/contract/perceptual_query.h include/contract/narrative_diffusion.h include/contract/narrative_branching.h include/contract/interactive_agent.h include/contract/mcp_wiki.h include/contract/mcp_web_search.h include/contract/mcp_file_read.h include/contract/mcp_calculator.h include/contract/mcp_summarizer.h include/contract/mcp_file_write.h include/agent_memory.h include/contract/book_concept.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CCE) tests/glyph_habitat.c src/contract/text_add.c src/contract/text_add_compound.c src/contract/text_add_abstain.c src/contract/perceptual_query.c src/contract/narrative_diffusion.c src/contract/narrative_branching.c src/contract/interactive_agent.c src/contract/mcp_utils.c src/contract/mcp_memory.c src/contract/mcp_wiki.c src/contract/mcp_web_search.c src/contract/mcp_file_read.c src/contract/mcp_calculator.c src/contract/mcp_summarizer.c src/contract/mcp_file_write.c src/agent_memory.c src/contract/book_concept.c src/cnet_lm.c src/contract/anti_repeat.c $(LDFLAGS) $(MCP_LDFLAGS)
# Note: for full contract-based decimal response in glyph_habitat (dec_full_add composition),
# run `make decimal` (or decimal_demo) first to generate dec_value_weights.txt + dec_full_add_weights.txt.

# Build tool: dedicated build/ folder similar to tests/
# Focuses on the "build" agent features (artifact construction, etc.)
build_tool: build/build.c src/contract/mcp_utils.c src/contract/mcp_memory.c src/contract/mcp_file_write.c src/contract/mcp_web_search.c src/contract/mcp_file_read.c src/contract/mcp_wiki.c src/agent_memory.c src/contract/book_concept.c include/nn.h include/router.h include/plan_table.h include/contract/contract.h include/agent_memory.h include/contract/mcp_file_write.h include/contract/mcp_web_search.h include/contract/mcp_file_read.h include/contract/mcp_wiki.h include/contract/book_concept.h
	$(CC) $(CFLAGS) -Ibuild/include -o build_tool $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) build/build.c src/contract/mcp_utils.c src/contract/mcp_memory.c src/contract/mcp_file_write.c src/contract/mcp_web_search.c src/contract/mcp_file_read.c src/contract/mcp_wiki.c src/agent_memory.c src/contract/book_concept.c src/cnet_lm.c src/contract/anti_repeat.c $(LDFLAGS) $(MCP_LDFLAGS)

build: build_tool
	./$(BIN_DIR)/build_tool --build default

# Regression: attention_retrieve_top_k stack overflow for reg->count>64.
# Standalone (no scan dep) so it builds independently of the aggregate suite.
test_attention_overflow: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) tests/test_attention_overflow.c include/nn.h include/router.h include/plan_table.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) tests/test_attention_overflow.c $(LDFLAGS)

# Planner scale study: collision-factor sweep over the REAL dag_plan_circuit.
# Measures search expansions / wall-clock / plan-found vs library breadth and
# the per-type collision factor, under default-beam / exhaustive / memo-off.
# Budgeted; NOT part of make test.
planner_scale_study: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) tests/planner_scale_study.c include/nn.h include/router.h include/plan_table.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) tests/planner_scale_study.c $(LDFLAGS)

# Compositional generalization study for the residue domain (data-efficiency gap,
# length extrapolation, extensibility). Budgeted; NOT part of make test.
# Uses the same common helpers as the gate. Run with OMP_NUM_THREADS for held-out speed.
residue_study: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(RESIDUE_STUDY) include/nn.h include/router.h include/contract/contract.h include/plan_table.h include/scan.h tests/residue_common.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(RESIDUE_STUDY) $(LDFLAGS)

residue: residue_study
	./$(BIN_DIR)/residue_study

# Attention Planner telemetry study (v0.5) — builds separately, not part of normal test.
# Writes logs/attention-planner-v0.5/ with CSV + summary. No side effects on evidence or files.
ATTENTION_STUDY := tests/attention_study.c
attention_study: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(ATTENTION_STUDY) include/nn.h include/router.h include/contract/contract.h include/plan_table.h include/scan.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(ATTENTION_STUDY) $(LDFLAGS)

attention: attention_study
	./$(BIN_DIR)/attention_study

# Spine overhead benchmark: ns/route_plan with lifecycle OFF vs ON over a
# scaled registry. Budgeted; NOT part of make test.
lifecycle_bench: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(LIFECYCLE_BENCH) include/nn.h include/router.h include/plan_table.h include/contract/contract.h include/scan.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(LIFECYCLE_BENCH) $(LDFLAGS)

lbench: lifecycle_bench
	./$(BIN_DIR)/lifecycle_bench

# v5.1 probe-overhead micro-benchmark: the below-beam probe's cost as a multiple
# of one route+execute cycle (deployment-shaped denominator). Shadow-only & opt-in
# -- this number informs a FUTURE activation decision, not v5.1 correctness.
# Shares residue_common.h, so -Wno-unused-function. Budgeted; NOT part of make test.
probe_overhead_bench: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(PROPOSAL_SIDECAR) $(PROBE_OVERHEAD_BENCH) include/nn.h include/router.h include/contract/contract.h include/plan_table.h include/scan.h include/proposal_sidecar.h tests/residue_common.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(PROPOSAL_SIDECAR) $(PROBE_OVERHEAD_BENCH) $(LDFLAGS)

probe_overhead: probe_overhead_bench
	./$(BIN_DIR)/probe_overhead_bench

# EVIDENCE_CLEAR gate overhead + structural_canonical_digest sweep cost.
# Measures the delta between legacy library_evolve and gated library_evolve_gated
# (with evidence seeded so both mint the same chunk) and the per-call cost of
# structural_canonical_digest on a small lure fixture.
# Budgeted; NOT part of make test.
dgate_bench: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(LIBRARY) $(DGATE_BENCH)
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(LIBRARY) $(DGATE_BENCH) $(LDFLAGS)
	./$(BIN_DIR)/dgate_bench

# Decimal Ladder compounding benchmark: Run A (raw) vs Run B (after gated Tier-2 chunk).
# Budgeted; NOT part of make test.
compounding_bench: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(LIBRARY) $(CMPND_BENCH)
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(LIBRARY) $(CMPND_BENCH) $(LDFLAGS)
	./$(BIN_DIR)/compounding_bench

# Recursive expression evaluator: bounded 3-slot stack step (expr_step) certifies
# (best-effort under budget); hand-built chains evaluate RPN-style token programs matching GT.
# expr_common.h holds the statics (patterned on residue). Integrated into `make test`.
test_expr: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(EXPR_TEST) include/nn.h include/router.h include/contract/contract.h include/plan_table.h include/scan.h tests/expr_common.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(EXPR_TEST) $(LDFLAGS)

# Checks equational laws over real primitives; catches a broken retrain.
property_demo: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(PROPERTY_DEMO) include/nn.h include/router.h include/plan_table.h include/contract/contract.h include/property.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(PROPERTY_DEMO) $(LDFLAGS)

# Regenerates frozen weights, then checks the round-trip laws.
property: nn_demo property_demo
	./$(BIN_DIR)/nn_demo
	./$(BIN_DIR)/property_demo

# Trains the decimal domain (second domain: new primitives + tags only,
# zero core changes), then discovers, refuses, ripples, consolidates,
# certifies and checks laws over it.
decimal_demo: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(PROPERTY) $(DECIMAL_DEMO) include/nn.h include/router.h include/plan_table.h include/consolidate.h include/contract/contract.h include/property.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(PROPERTY) $(DECIMAL_DEMO) $(LDFLAGS)

# Trains + freezes the decimal primitives and runs all six acts. Act 3
# loads hex_value_weights.txt (committed; regenerate with make run).
decimal: decimal_demo
	./$(BIN_DIR)/decimal_demo

# The hierarchical dec_add2: a certified 2-chunk circuit verified on all
# 20000 additions; --flat <width> <epochs> [shuffled] attempts the flat
# student at a study-chosen config.
capacity_demo: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(CAPACITY_DEMO) include/nn.h include/router.h include/plan_table.h include/consolidate.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(CAPACITY_DEMO) $(LDFLAGS)

capacity: capacity_demo
	./$(BIN_DIR)/capacity_demo

# The chunk-capacity measurement harness. Spends a compute budget by
# design -- NOT part of make test. Optional arg = seconds per run.
capacity_study: $(SRC) $(CAPACITY_STUDY) include/nn.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(CAPACITY_STUDY) $(LDFLAGS)

study: capacity_study
	./$(BIN_DIR)/capacity_study

# Per-port margin + correctness divergence through composition (the discrete-
# fabric instrument). Default = verified baseline + depth pass; --stress maps
# where margin first crowds the band. Loads the committed frozen weights.
margin_study: $(SRC) $(MARGIN_STUDY) include/nn.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(MARGIN_STUDY) $(LDFLAGS)

# Is the 7-seg confident-wrong catchable? Model-free input floor + K-leaf
# ensemble disagreement on the accepted-wrong cases. Links only src/nn.c.
sevenseg_floor_study: $(SRC) tests/sevenseg_floor_study.c include/nn.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) tests/sevenseg_floor_study.c $(LDFLAGS)

sevenseg: sevenseg_floor_study
	./$(BIN_DIR)/sevenseg_floor_study

margin: margin_study
	./$(BIN_DIR)/margin_study

# Information-loss probe: does margin separate fundamental crowding (the
# interface can't carry the concept) from architectural underfitting? Trains a
# width x resolution grid; budgeted, NOT part of make test.
fuzzy_study: $(SRC) $(FUZZY_STUDY) include/nn.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(FUZZY_STUDY) $(LDFLAGS)

fuzzy: fuzzy_study
	./$(BIN_DIR)/fuzzy_study

# Stochastic probe: a graded world (sigmoid label noise) where neither width nor
# resolution recovers -- the third wall. Budgeted, NOT part of make test.
stochastic_study: $(SRC) $(STOCHASTIC_STUDY) include/nn.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(STOCHASTIC_STUDY) $(LDFLAGS)

stochastic: stochastic_study
	./$(BIN_DIR)/stochastic_study

# Throughput: how fast can the frozen route push samples, and which lever buys
# it? Oracle (route_execute) vs the fast lane at each precision, faithfulness +
# margin reported. Budgeted study, NOT part of make test. Loads committed
# hex_value/increment weights (run ./nn_demo first).
throughput_study: $(SRC) $(ROUTER) $(CONSOLIDATE) $(PLAN_TABLE) $(CONTRACT) $(FASTPATH) $(THROUGHPUT_STUDY) include/nn.h include/router.h include/consolidate.h include/fastpath.h include/plan_table.h include/contract/contract.h
	$(CC) $(CFLAGS) $(OMPFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(CONSOLIDATE) $(PLAN_TABLE) $(CONTRACT) $(FASTPATH) $(THROUGHPUT_STUDY) $(LDFLAGS) $(OMPFLAGS)

throughput: throughput_study
	./$(BIN_DIR)/throughput_study

# Circuits over real frozen primitives: shared executions, discovered
# ripple-carry, the multi-output circuit chunk, and the measured pruning
# benchmark. --stretch additionally attempts the 20000-sample dec_add2.
circuit_demo: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(CIRCUIT_DEMO) include/nn.h include/router.h include/plan_table.h include/consolidate.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(CIRCUIT_DEMO) $(LDFLAGS)

# Runs the five circuit acts over the committed hex + decimal weights.
circuit: circuit_demo
	./$(BIN_DIR)/circuit_demo

# Certifies primitives against their contracts; imposters are refused.
certify_demo: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(CERTIFY_DEMO) include/nn.h include/router.h include/plan_table.h include/consolidate.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(CERTIFY_DEMO) $(LDFLAGS)

# Regenerates frozen weights + contracts, then certifies and plans with
# the require_certified knob.
certify: nn_demo certify_demo
	./$(BIN_DIR)/nn_demo
	./$(BIN_DIR)/certify_demo

run: nn_demo
	./$(BIN_DIR)/nn_demo

# Standalone proof-vs-sample coverage layer test + benchmark (also wired into
# test_all). Builds tests/test_coverage.c against the new src/contract/coverage.c.
coverage: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(COVERAGE_TEST) include/nn.h include/router.h include/contract/contract.h include/property.h include/contract/coverage.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(COVERAGE_TEST) $(LDFLAGS)
	./$(BIN_DIR)/coverage

# Standalone conformal reject-option test + benchmark (also wired into test_all).
conformal: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CONSOLIDATE) $(SCAN) $(CONFORMAL) $(CONFORMAL_TEST) include/nn.h include/router.h include/contract/contract.h include/contract/conformal.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CONSOLIDATE) $(SCAN) $(CONFORMAL) $(CONFORMAL_TEST) $(LDFLAGS)
	./$(BIN_DIR)/conformal

# Reusable sentence-terminator primitive: a BTN with a typed/tagged contract
# (last_word -> sentence_end). btn_certify exact-proves the decidable per-word
# end-rule; split conformal gives a distribution-free abstention guarantee for
# the rest. One frozen primitive, reused by two generators.
endgate: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CONSOLIDATE) $(SCAN) $(CONFORMAL) tests/endgate_demo.c include/nn.h include/router.h include/contract/contract.h include/contract/conformal.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CONSOLIDATE) $(SCAN) $(CONFORMAL) tests/endgate_demo.c $(LDFLAGS)
	./$(BIN_DIR)/endgate

# PDF -> dynamic corpus -> replay-trained word-LM. Module unit tests:
pdftest: $(PDF_SRC) $(CORPUS_SRC) $(PDF_TEST) include/pdf/inflate.h include/pdf/pdf_extract.h include/corpus/corpus_split.h include/corpus/corpus_store.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(PDF_SRC) $(CORPUS_SRC) $(PDF_TEST) $(LDFLAGS)
	./$(BIN_DIR)/pdftest

pdflearn: $(PDF_SRC) $(CORPUS_SRC) $(CCE_WORDLM) $(PDFLEARN_DEMO) include/pdf/pdf_extract.h include/pdf/inflate.h include/corpus/corpus_split.h include/corpus/corpus_store.h include/cce/cce_wordlm.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(PDF_SRC) $(CORPUS_SRC) $(CCE_WORDLM) $(PDFLEARN_DEMO) $(LDFLAGS)
	./$(BIN_DIR)/pdflearn

# Compounding retrieval memory: a growing context->continuation store so learning
# a new book reuses prior memory (O(book) ingest) instead of retraining (O(sum)).
compound: $(RETRIEVAL_SRC) $(CORPUS_SRC) $(PDF_SRC) $(CCE_WORDLM) $(COMPOUND_DEMO) include/corpus/retrieval.h include/corpus/corpus_split.h include/corpus/corpus_store.h include/pdf/pdf_extract.h include/cce/cce_wordlm.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(RETRIEVAL_SRC) $(CORPUS_SRC) $(PDF_SRC) $(CCE_WORDLM) $(COMPOUND_DEMO) $(LDFLAGS)
	./$(BIN_DIR)/compound

# Production tiered tile memory (AICIMO-adapted): fuzzy-dedup tiles, HOT cap + WARM
# disk spill (bounded RAM / unbounded disk), persistent, contract-graduation hook.
tiermem_test: $(TILEMEM_SRC) $(SYNONYMS_SRC) $(CORPUS_SRC) $(PDF_SRC) $(TILEMEM_TEST) include/corpus/tile_memory.h include/corpus/synonyms.h include/corpus/corpus_split.h include/pdf/pdf_extract.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(TILEMEM_SRC) $(SYNONYMS_SRC) $(CORPUS_SRC) $(PDF_SRC) $(TILEMEM_TEST) $(LDFLAGS)
	./$(BIN_DIR)/tiermem_test

# Extraction quality: font /Differences recovery + English-likeness quality filter.
fontdecode: $(PDF_SRC) $(CORPUS_SRC) $(TILEMEM_SRC) $(SYNONYMS_SRC) $(FONTDECODE_TEST) include/pdf/font_decode.h include/pdf/pdf_extract.h include/corpus/corpus_split.h include/corpus/tile_memory.h include/corpus/synonyms.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(PDF_SRC) $(CORPUS_SRC) $(TILEMEM_SRC) $(SYNONYMS_SRC) $(FONTDECODE_TEST) $(LDFLAGS)
	./$(BIN_DIR)/fontdecode

# Lever 2: idf-weighted (TF-IDF) tile-memory retrieval (fixes the rare-term miss).
tfidf: $(TILEMEM_SRC) $(SYNONYMS_SRC) $(CORPUS_SRC) $(PDF_SRC) $(TFIDF_TEST) include/corpus/tile_memory.h include/corpus/synonyms.h include/corpus/corpus_split.h include/pdf/pdf_extract.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(TILEMEM_SRC) $(SYNONYMS_SRC) $(CORPUS_SRC) $(PDF_SRC) $(TFIDF_TEST) $(LDFLAGS)
	./$(BIN_DIR)/tfidf

# Lever 3: corpus PPMI synonym map + opt-in query expansion. Unit tests + book bench.
synonyms: $(SYNONYMS_SRC) $(TILEMEM_SRC) $(CORPUS_SRC) $(PDF_SRC) $(SYNONYMS_TEST) include/corpus/synonyms.h include/corpus/tile_memory.h include/corpus/corpus_split.h include/pdf/pdf_extract.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SYNONYMS_SRC) $(TILEMEM_SRC) $(CORPUS_SRC) $(PDF_SRC) $(SYNONYMS_TEST) $(LDFLAGS)
	./$(BIN_DIR)/synonyms

# Lever 5: unified inverted index over tiles. Identity (index==linear) + forced-WARM bench.
tileindex: $(TILEMEM_SRC) $(SYNONYMS_SRC) $(CORPUS_SRC) $(PDF_SRC) $(TILEINDEX_TEST) include/corpus/tile_memory.h include/corpus/synonyms.h include/corpus/corpus_split.h include/pdf/pdf_extract.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(TILEMEM_SRC) $(SYNONYMS_SRC) $(CORPUS_SRC) $(PDF_SRC) $(TILEINDEX_TEST) $(LDFLAGS)
	./$(BIN_DIR)/tileindex

# Lever 6: opt-in PPMI semantic consolidation (merge paraphrase tiles).
consolidate: $(TILEMEM_SRC) $(SYNONYMS_SRC) $(CORPUS_SRC) $(PDF_SRC) $(CONSOLIDATE_TEST) include/corpus/tile_memory.h include/corpus/synonyms.h include/corpus/corpus_split.h include/pdf/pdf_extract.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(TILEMEM_SRC) $(SYNONYMS_SRC) $(CORPUS_SRC) $(PDF_SRC) $(CONSOLIDATE_TEST) $(LDFLAGS)
	./$(BIN_DIR)/consolidate

# Graduation vertical slice: mine a near-deterministic regularity -> certify two
# positionally-tagged units -> register -> evict tiles -> router auto-composes them.
graduate: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CONSOLIDATE) $(SCAN) $(TILEMEM_SRC) $(SYNONYMS_SRC) $(GRADUATE_SRC) $(CORPUS_SRC) $(PDF_SRC) $(GRADUATE_TEST) include/corpus/graduate.h include/corpus/tile_memory.h include/corpus/synonyms.h include/router.h include/contract/contract.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CONSOLIDATE) $(SCAN) $(TILEMEM_SRC) $(SYNONYMS_SRC) $(GRADUATE_SRC) $(CORPUS_SRC) $(PDF_SRC) $(GRADUATE_TEST) $(LDFLAGS)
	./$(BIN_DIR)/graduate

# JSON-as-contract over TinyStories: generate {"title","characters","story"} with
# an LM-generated story + parse it back losslessly. Escape/un-escape decisions are
# PROVEN (btn_certify_exhaustive); characters name-gate is certified + conformal.
jsonstory: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(CONFORMAL) $(CCE_WORDLM) $(JSONSTORY_DEMO) include/nn.h include/contract/contract.h include/contract/coverage.h include/contract/conformal.h include/cce/cce_wordlm.h tests/json_validate.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(CONFORMAL) $(CCE_WORDLM) $(JSONSTORY_DEMO) $(LDFLAGS)
	./$(BIN_DIR)/jsonstory

# Standalone learnable boolean circuit (DLGN) test + benchmark. The module is
# self-contained (stdlib + libm only), so this links just the module + its test.
logicgate: $(LOGICGATE) $(LOGICGATE_TEST) include/logic_gate_net.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $(BIN_DIR)/$@ $(LOGICGATE) $(LOGICGATE_TEST) $(LDFLAGS)
	./$(BIN_DIR)/logicgate

# Consolidated single-exe test runner (only 1 exe instead of many).
# test_all.c pulls in all the run_test_xxx functions from the individual test_*.c files.
test_all: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(FASTPATH) $(LIBRARY) $(PROPOSAL_SIDECAR) $(COVERAGE) $(CONFORMAL) $(LOGICGATE) \
          $(TILEMEM_SRC) $(SYNONYMS_SRC) $(CORPUS_SRC) $(PDF_SRC) $(GRADUATE_SRC) \
          tests/test_all.c tests/test_nn.c tests/test_encode_oob.c tests/test_composition.c \
          tests/test_contract.c tests/test_router.c tests/test_dag.c tests/test_consolidate.c \
          tests/test_certify.c tests/test_property.c tests/test_decimal.c tests/test_circuit.c \
          tests/test_library.c tests/test_fastpath.c tests/test_residue.c tests/test_expr.c tests/test_lifecycle.c \
          tests/test_proposal_sidecar.c tests/test_below_beam_recovery.c tests/test_belowbeam_chars.c \
          tests/test_structural_pref.c tests/test_structural_pref_b0.c tests/test_structural_pref_adversarial.c tests/test_structural_pref_adversarial_circuit.c \
          tests/test_distillation_gate.c tests/test_expansion.c tests/test_mcp_security.c tests/test_coverage.c tests/test_conformal.c tests/test_logic_gate.c \
          $(TFIDF_TEST) $(TILEMEM_TEST) $(GRADUATE_TEST) $(FONTDECODE_TEST) $(PDF_TEST) $(SYNONYMS_TEST) $(TILEINDEX_TEST) $(CONSOLIDATE_TEST) \
          src/contract/mcp_utils.c src/contract/mcp_memory.c src/contract/mcp_wiki.c src/contract/mcp_web_search.c \
          src/contract/mcp_file_read.c src/contract/mcp_file_write.c \
          include/nn.h include/router.h include/plan_table.h include/contract/contract.h include/scan.h include/property.h include/contract/coverage.h include/contract/conformal.h include/logic_gate_net.h \
          include/consolidate.h include/fastpath.h include/library.h include/proposal_sidecar.h tests/belowbeam_chars_common.h \
          tests/structural_pref_common.h \
          include/corpus/tile_memory.h include/corpus/synonyms.h include/corpus/corpus_split.h include/corpus/corpus_store.h include/corpus/graduate.h \
          include/pdf/pdf_extract.h include/pdf/inflate.h include/pdf/font_decode.h
	$(CC) $(CFLAGS) -Wno-unused-function -DTEST_ALL -o test_all \
	      $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(FASTPATH) $(LIBRARY) $(PROPOSAL_SIDECAR) $(COVERAGE) $(CONFORMAL) $(LOGICGATE) \
	      $(TILEMEM_SRC) $(SYNONYMS_SRC) $(CORPUS_SRC) $(PDF_SRC) $(GRADUATE_SRC) \
	      tests/test_all.c tests/test_nn.c tests/test_encode_oob.c tests/test_composition.c \
	      tests/test_contract.c tests/test_router.c tests/test_dag.c tests/test_consolidate.c \
	      tests/test_certify.c tests/test_property.c tests/test_decimal.c tests/test_circuit.c \
	      tests/test_library.c tests/test_fastpath.c tests/test_residue.c tests/test_expr.c tests/test_lifecycle.c \
	      tests/test_proposal_sidecar.c tests/test_below_beam_recovery.c tests/test_belowbeam_chars.c \
	      tests/test_structural_pref.c tests/test_structural_pref_b0.c tests/test_structural_pref_adversarial.c tests/test_structural_pref_adversarial_circuit.c \
	      tests/test_distillation_gate.c tests/test_expansion.c tests/test_mcp_security.c tests/test_coverage.c tests/test_conformal.c tests/test_logic_gate.c \
	      $(TFIDF_TEST) $(TILEMEM_TEST) $(GRADUATE_TEST) $(FONTDECODE_TEST) $(PDF_TEST) $(SYNONYMS_TEST) $(TILEINDEX_TEST) $(CONSOLIDATE_TEST) \
	      src/contract/mcp_utils.c src/contract/mcp_memory.c src/contract/mcp_wiki.c src/contract/mcp_web_search.c \
	      src/contract/mcp_file_read.c src/contract/mcp_file_write.c \
	      $(SCAN) $(LDFLAGS) $(MCP_LDFLAGS) -Wl,--allow-multiple-definition

legacy_test: test_all
	./$(BIN_DIR)/test_all

verify: cce_dll cce_safetensors_test cce_autograd_test cce_model_test cce_view forest_view cce_detect cce_ssm cce_st_llama cce_specgraph cce_wstore cce_tiers cce_similar contract_secure contract_unit acquire
	dotnet test dotnet/Cce.Tests/Cce.Tests.csproj -c Release --no-restore

verify-long: verify cce_train_bench supra_head_qat supra_head_qat_corpus wordlm_bitnet

test: verify

# (Legacy individual targets removed to enforce single-exe policy for tests.
# If you need to debug one suite in isolation, compile it manually or restore the rule temporarily.)

# Regenerates the frozen weight files via the demo, then composes
# hex_value -> increment with no training in between.
compose: nn_demo test_composition
	./$(BIN_DIR)/nn_demo
	./$(BIN_DIR)/test_composition

# Regenerates frozen weights, then has the planner discover and run a chain.
route: nn_demo route_demo
	./$(BIN_DIR)/nn_demo
	./$(BIN_DIR)/route_demo

# Regenerates frozen weights, then discovers and runs a branching DAG.
dag: nn_demo dag_demo
	./$(BIN_DIR)/nn_demo
	./$(BIN_DIR)/dag_demo

# Regenerates frozen weights, then runs a heterogeneous multi-input DAG.
hetero: nn_demo hetero_demo
	./$(BIN_DIR)/nn_demo
	./$(BIN_DIR)/hetero_demo

# Regenerates frozen weights, then runs a multi-output port-selection DAG.
split: nn_demo split_demo
	./$(BIN_DIR)/nn_demo
	./$(BIN_DIR)/split_demo

# Regenerates frozen weights, then distills proven plans into chunks.
chunk: nn_demo chunk_demo
	./$(BIN_DIR)/nn_demo
	./$(BIN_DIR)/chunk_demo

clean:
	# Remove all known generated binaries (core + demos + studies).
	# We also do a blanket *.exe cleanup so the worktree root stays tidy
	# (prevents the "ton of exe files" clutter you see in Rider).
	rm -f nn_demo test_nn test_encode_oob test_contract test_composition test_router test_dag \
	      test_consolidate test_certify test_property test_decimal test_circuit test_library \
	      property_demo route_demo dag_demo hetero_demo split_demo chunk_demo certify_demo \
	      decimal_demo circuit_demo capacity_study capacity_demo margin_study fuzzy_study stochastic_study \
	      test_fastpath throughput_study test_residue test_expr residue_study attention_study \
	      lifecycle_bench test_belowbeam_chars belowbeam_chars_study \
	      test_structural_pref test_structural_pref_b0 structural_pref_study \
	      cce_json_bench cce_smoke jsonstory pdftest pdflearn compound tiermem_test graduate fontdecode tfidf synonyms tileindex consolidate pdf_corpus.txt retrieval_store.bin cce.dll *.exe *.so *.dylib
	rm -rf $(BIN_DIR)
	rm -rf tile_store_test tile_store_res tile_store_per tile_store_cert tile_mem_pdf tile_grad_ctl tile_grad_pdf font_qa tfidf_ctl tfidf_book syn_ctl syn_rev syn_book tix_small tix_warm tix_exp tix_book cons_ctl cons_book cons_prune cons_cap
	# Clean GGUF/CNET test aftermath junk (temp forests, per-experiment logs/outputs, .txt files)
	rm -f gguf_qwen2_forest.cce gguf_qwen2_packed.cce gguf_test_run.log ssm_forest.cce st_llama_forest.cce
	rm -f ssm_test.safetensors ssm_test.gguf ssm_bad.safetensors stll_test.gguf
	rm -rf stll_tmp stll_nocfg
	rm -f sg_a.gguf sg_a.graph sg_mamba.safetensors ws_a.gguf ws_a.manifest ws_restore.cce
	rm -rf sg_a sg_b ws_a ws_b ws_store ws_forge
	rm -f tr_a.manifest tr_bad.manifest tr_restore.cce sim_*.manifest sim_merge.cce
	rm -rf tr_a tr_store sim_a sim_e sim_m sim_store
	rm -rf logs
	rm -f qwen2_*.cce qwen2_gguf_packed_*.cce test_*.txt make_*.log chat_*.txt err.txt
	rm -f test_gguf_*.txt test_phase*.txt test_final*.txt test_long*.txt test_completed*.txt test_quality*.txt test_int8*.txt test_rerun*.txt
	# Note: residue_study (the full budgeted study) is now implemented; run with `make residue`.

ifdef HAVE_CUDA
src/cce/cce_cuda.o: src/cce/cce_cuda.cu include/cce/cce_gpu.h
	@echo "Compiling CUDA kernels (optional)..."
	$(NVCC) -c -o $(BIN_DIR)/$@ $< -Xcompiler "-std=c++14 $(CFLAGS)" -Iinclude $(CUDA_CFLAGS)
endif

# Minimal CCE smoke test (Contract Cascade Engine foundation)
cce_smoke: $(CCE) $(CCE_CUDA_OBJ) tests/cce_smoke.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_smoke.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	@echo "cce_smoke built. Run manually: ./cce_smoke"

# Pure CCE build without legacy nn.c (for testing the new engine)
cce_smoke_pure: $(CCE) $(CCE_CUDA_OBJ) tests/cce_smoke.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_smoke.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_smoke_pure || echo "Pure CCE smoke exited with code $?"

cce_train_bench: $(CCE) $(CCE_CUDA_OBJ) tests/cce_train_bench.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_train_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_train_bench

cce_json_bench: $(CCE) $(CCE_CUDA_OBJ) tests/cce_json_bench.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_json_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_json_bench

# Zero-copy WARM load: cce_cascade_view_from_archive views weights in the mmap
# (owns_memory=0) instead of reload+copy. Verifies bit-identical forward + clean free.
cce_view: $(CCE) $(CCE_CUDA_OBJ) tests/cce_view_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_view_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_view > logs/cce_view.log 2>&1 || echo "view exited"

# Universal pre-run model structure detection: magic-sniff the container
# (gguf/safetensors/cce/packed), fingerprint the architecture from the tensors
# actually present, report hparams WITHOUT loading weights, dispatch to the
# matching loader via cce_anymodel_open. Unsupported structures refuse honestly.
cce_detect: $(CCE) $(CCE_CUDA_OBJ) tests/cce_detect_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_detect_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_detect > logs/cce_detect.log 2>&1 || echo "test exited non-zero (see log)"

# Mamba-1 SSM runner: forest-decomposed linear specialists + recurrent scan
# glue; verified against an independent double-precision reference and
# safetensors<->gguf mapping equivalence (bit-identical logits).
cce_ssm: $(CCE) $(CCE_CUDA_OBJ) tests/cce_ssm_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_ssm_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_ssm > logs/cce_ssm.log 2>&1 || echo "test exited non-zero (see log)"

# HF-llama safetensors loader: same decomposed transformer as the GGUF path,
# gated by bit-identical logits between the two container formats.
cce_st_llama: $(CCE) $(CCE_CUDA_OBJ) tests/cce_st_llama_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_st_llama_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_st_llama > logs/cce_st_llama.log 2>&1 || echo "test exited non-zero (see log)"

# Specialist knowledge graph: content digests + behavioral fingerprints +
# DATA_FLOWS wiring for any decomposed model (the codebase-memory move on
# weights). Gates: cross-container digest identity, one-matrix locality.
cce_specgraph: $(CCE) $(CCE_CUDA_OBJ) tests/cce_specgraph_test.c tests/tiny_model_fixture.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_specgraph_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_specgraph > logs/cce_specgraph.log 2>&1 || echo "test exited non-zero (see log)"

# Content-addressed weight store: specialists stored once by digest, models
# as manifests. Gates: 100% reuse on re-ingest + cross-container, fine-tune
# costs one payload, restore is bit-identical, reuse claims byte-verified.
cce_wstore: $(CCE) $(CCE_CUDA_OBJ) tests/cce_wstore_test.c tests/tiny_model_fixture.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_wstore_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_wstore > logs/cce_wstore.log 2>&1 || echo "test exited non-zero (see log)"

# Tiered runtime: run a store-backed transformer in bounded RAM (HOT cap +
# LRU eviction + on-demand rehydration). Gate: capped streaming logits are
# bit-identical to all-resident; residency never exceeds the cap.
cce_tiers: $(CCE) $(CCE_CUDA_OBJ) tests/cce_tiers_test.c tests/tiny_model_fixture.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_tiers_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_tiers > logs/cce_tiers.log 2>&1 || echo "test exited non-zero (see log)"

# SIMILAR_TO + evidence-gated merge: epsilon-equivalent specialists merge via
# manifest remap ONLY after an adversarial probe battery; unverified refuses.
cce_similar: $(CCE) $(CCE_CUDA_OBJ) tests/cce_similar_test.c tests/tiny_model_fixture.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_similar_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_similar > logs/cce_similar.log 2>&1 || echo "test exited non-zero (see log)"

# CLI probe: make detect FILE=Models/foo.gguf  (or run bin/detect_cli directly)
detect_cli: $(CCE) $(CCE_CUDA_OBJ) tests/detect_cli.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/detect_cli.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

detect: detect_cli
	./$(BIN_DIR)/detect_cli $(FILE)

# Forest tier wiring: zero-copy WARM views + copy-on-write to HOT + seal guard,
# live in cce_forest_forward / promote_to_hot.
forest_view: $(CCE) $(CCE_CUDA_OBJ) tests/cce_forest_view_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_forest_view_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/forest_view > logs/forest_view.log 2>&1 || echo "view exited"

cce_model_test: $(CCE) $(CCE_CUDA_OBJ) tests/test_cce_model_save.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/test_cce_model_save.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_model_test > logs/cce_model_test.log 2>&1 || echo "test exited non-zero (see log)"

cce_autograd_test: $(CCE) $(CCE_CUDA_OBJ) tests/test_cce_autograd.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/test_cce_autograd.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_autograd_test > logs/cce_autograd_test.log 2>&1 || echo "test exited non-zero (see log)"

# --- SIMD re-enable for the CCE/Supra targets ---
# The global -mno-avx works around the MinGW AVX struct-copy segfault triggered by
# the contract router's by-value Port struct (src/router/, include/router.h). These
# CCE targets do NOT link src/router/, so they are AVX-safe (verified: builds + runs
# clean). Combined with the vectorizable mat-vec in cce_block.c, AVX speeds up the
# tensor math. -std=c11 keeps FP contraction off, so weights stay bit-identical.
AVX_CFLAGS := $(filter-out -mno-avx,$(CFLAGS))
cce_safetensors_test supra_console supra_chat_mock supra_context_probe supra_longform supra_head_qat supra_head_qat_corpus: CFLAGS := $(AVX_CFLAGS) $(OMPFLAGS)

cce_safetensors_test: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/test_cce_safetensors.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/test_cce_safetensors.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_safetensors_test > logs/cce_safetensors_test.log 2>&1 || echo "test exited non-zero (see log)"

# Dedicated GGUF loader + Qwen2 forest + full K dequant + packed 1.6-bit roundtrip test
# Mirrors the Supra flow end-to-end (load -> specialists -> pack_trits -> export -> load_packed -> forward/generate)
cce_gguf_test: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/test_cce_gguf.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/test_cce_gguf.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/$@ > logs/gguf_test_run.log 2>&1 || echo "test exited non-zero (see log)"

supra_console: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_console.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_console.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	@echo "Built supra_console. Weights are downloaded once into ./supra_cache and reused."
	@echo "Try: ./supra_console --mode text --prompt \"Once upon a time\" --max_new 40"

# Context-length probe: fills the whole positional window once and reports the
# hard ceiling (block_size) vs the coherence-collapse point (looping). See
# tests/supra_context_probe.c.
supra_context_probe: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_context_probe.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_context_probe.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/supra_context_probe > logs/supra_context_probe.log 2>&1 || echo "test exited non-zero (see log)"

# Skeleton-driven long-form injector: slides a sub-384 window, cuts each chunk
# before the ~120 coherence decay, and steers with a caller-supplied beat list so
# the piece actually finishes. Runs Approach B (skeleton) vs Approach A (pure
# sliding-window baseline) for contrast. See tests/supra_longform.c.
supra_longform: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_longform.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_longform.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/supra_longform > logs/supra_longform.log 2>&1 || echo "test exited non-zero (see log)"

# Head-only QAT smoke (Supra quality phase, milestone 1): freeze the transformer,
# cache final hidden vectors, train a ternary BitLinear head (FP shadow + STE),
# and check QAT beats post-hoc ternary on the real Supra head. See tests/supra_head_qat.c.
supra_head_qat: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_head_qat.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_head_qat.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/supra_head_qat > logs/supra_head_qat.log 2>&1 || echo "test exited non-zero (see log)"

# Head QAT on a REAL corpus (pdf_corpus.txt): genuine held-out FP-recovery test.
supra_head_qat_corpus: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_head_qat_corpus.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_head_qat_corpus.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/supra_head_qat_corpus > logs/supra_head_qat_corpus.log 2>&1 || echo "test exited non-zero (see log)"

# Lightweight mock chat test: exercises the real BPE tokenizer (encode) without
# requiring the full model forward. Needs supra_cache/tokenizer.json (run
# supra_console or cce_safetensors_test once to populate it).
supra_chat_mock: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/test_supra_chat_mock.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/test_supra_chat_mock.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/supra_chat_mock > logs/supra_chat_mock.log 2>&1 || echo "test exited non-zero (see log)"

# Self-contained BitNet b1.58 QAT demo: proves ternary weights reach ~FP accuracy
# WHEN trained for (shadow weights + straight-through estimator), while post-hoc
# ternarization of an FP-trained net collapses. No CCE deps.
bitnet_qat: tests/bitnet_qat_demo.c
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ tests/bitnet_qat_demo.c $(LDFLAGS)
	./$(BIN_DIR)/bitnet_qat

# Scalable word LM: tied embedding + bottleneck + class-factored softmax,
# exact-gradient (Adam) training. Breaks the O(V^2) one-hot-head wall -> O(V*d).
wordlm: $(CCE_WORDLM) tests/wordlm_demo.c include/cce/cce_wordlm.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(CCE_WORDLM) tests/wordlm_demo.c $(LDFLAGS)
	./$(BIN_DIR)/wordlm

# Long BitNet b1.58 QAT run inside the real word-LM: trains FP vs ternary-QAT
# (STE) vs post-hoc ternary on the same corpus and compares perplexity + samples.
# Use WLM_EPOCHS=N to shorten local smoke runs.
wordlm_bitnet: $(CCE_WORDLM) tests/wordlm_bitnet_demo.c include/cce/cce_wordlm.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(CCE_WORDLM) tests/wordlm_bitnet_demo.c $(LDFLAGS)
	./$(BIN_DIR)/wordlm_bitnet

# DLL target for .NET / P/Invoke / C# interop (and other hosts).
# Builds cce.dll (Windows) or cce.so (else). Defines CCE_BUILD_DLL so headers
# emit __declspec(dllexport) / visibility for the C ABI (model, dataset, handle).
# Usage: make cce_dll   (then copy cce.dll next to your .exe or into PATH)
cce_dll: $(CCE) $(CCE_CUDA_OBJ)
	$(CC) -shared -DCCE_BUILD_DLL $(CFLAGS) -o cce.dll $(CCE) $(CCE_CUDA_OBJ) $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	@echo "Built cce.dll (for .NET P/Invoke). Add to your C# project and use DllImport."

