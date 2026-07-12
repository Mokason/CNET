CC := gcc
CXX := g++
# -march=native: measured 1.25x on the training loops, bit-identical weights
# (FP contraction stays off under -std=c11). Drop it for portable binaries.
CFLAGS := -std=c11 -Wall -Wextra -pedantic -O3 -march=native
ifeq ($(OS),Windows_NT)
# -mno-avx is REQUIRED with -march=native on the MinGW toolchain ONLY:
# MinGW gcc 15.2 emits aligned 256-bit moves for by-value structs >= 32 bytes
# (Port is 56) on a 16-byte-aligned Windows stack -> segfault. -mstackrealign
# does not fix it; disabling AVX does. The Linux ABI has no such bug, and the
# int8 oracle matvec runs 256/512-bit wide there (bit-identical: each output
# element keeps its own i-ascending accumulation regardless of SIMD width).
CFLAGS += -mno-avx
endif
LDFLAGS := -lm
MCP_LDFLAGS :=
ifeq ($(OS),Windows_NT)
MCP_LDFLAGS := -lwininet
else
# Linux/glibc: strict -std=c11 hides POSIX/BSD declarations (popen, fseeko,
# usleep, ...). _DEFAULT_SOURCE restores glibc's default feature set without
# changing the C standard; MinGW never sees this branch.
CFLAGS += -D_DEFAULT_SOURCE
endif

# OpenMP support for multi-threaded studies inside a single exe (MinGW GCC).
# Follows strict rules: default(none) on all pragmas, OMP_NUM_THREADS=1 for
# isolating races, etc. Only study targets get -fopenmp.
OMPFLAGS := -fopenmp

SRC := src/nn.c
# Router split (SRP).
# Router sources in src/router/ (filenames without router_ prefix for cleanliness)
# Contract sources in src/contract/ (filenames without contract_ prefix)
ROUTER := src/router/dag_full.c src/router/registry.c src/router/route.c
CONSOLIDATE := src/consolidate.c
PLAN_TABLE := src/plan_table.c
TOPOLOGY := src/topology.c
HYPERBOLIC := src/hyperbolic.c
APP := src/legacy/main.c
TEST := tests/test_nn.c
OOB_TEST := tests/test_encode_oob.c
CONTRACT_TEST := tests/test_contract.c
COMPOSE_TEST := tests/test_composition.c
ROUTER_TEST := tests/test_router.c
SPARSE_KV_TEST := tests/sparse_kv_test.c
NARRATIVE_COHERENCE_TEST := tests/narrative_coherence_test.c
PHASE4_UNCERTAINTY_TEST := tests/phase4_uncertainty_test.c
PHASE5_INTEGRATION_TOOL := tools/register_compression_improvements.c
PHASE5_INTEGRATION_TEST := tests/phase5_integration_test.c
COUNTERFACTUAL_ROUTER_TEST := tests/router/counterfactual_test.c
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
CCE_SPARSE_KV := src/cce/cce_sparse_kv.c
CCE_UNCERTAINTY := src/cce/cce_uncertainty.c
CCE_COMPRESSION := src/cce/cce_compression.c
CCE_LEARN   := src/cce/cce_learn.c
CCE_PATCH   := src/cce/cce_block_patch.c
CCE_GPU     := src/cce/cce_gpu.c
CCE_WORDLM  := src/cce/cce_wordlm.c
CCE_AICIMO  := src/cce/cce_aicimo.c
CCE_PERCEPTUAL := src/cce/cce_perceptual_leaf.c
CCE_MODEL := src/cce/cce_model.c
CCE_MODEL_IO := src/cce/cce_model_io.c
CCE_DATASET := src/cce/cce_dataset.c
CCE_AUTOGRAD := src/cce/cce_autograd.c src/cce/cce_autograd_ops.c
CCE_SAFETENSORS := src/cce/cce_safetensors.c
CCE_GGUF := src/cce/cce_gguf.c $(CCE_AICIMO)
CCE_AICIMO  := src/cce/cce_aicimo.c
CCE_QGKP := src/cce/cce_qgkp.c

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
  CUDA_CFLAGS  := -DCCE_HAVE_CUDA
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
CCE_CLGEMM  := src/cce/cce_clgemm.c
CCE_SUPRA_TRAIN := src/cce/cce_supra_train.c
CCE := $(CCE_TENSOR) $(CCE_BLOCK) $(CCE_CASCADE) $(CCE_ARCHIVE) $(CCE_FOREST) $(CCE_ROUTER) $(CCE_SPARSE_KV) $(CCE_UNCERTAINTY) $(CCE_COMPRESSION) $(CCE_LEARN) $(CCE_PATCH) $(CCE_GPU) $(CCE_ABI) $(CCE_CUDA_OBJ) $(CCE_PERCEPTUAL) $(CCE_WORDLM) $(CCE_MODEL) $(CCE_MODEL_IO) $(CCE_DATASET) $(CCE_AUTOGRAD) $(CCE_SAFETENSORS) $(CCE_GGUF) $(CCE_QGKP) $(CCE_DETECT) $(CCE_SSM) $(CCE_ST_LLAMA) $(CCE_SPECGRAPH) $(CCE_WSTORE) $(CCE_TIERRT) $(CCE_SIMILAR) $(CCE_CLGEMM) $(CCE_SUPRA_TRAIN)
CNET_CCE_ADAPTER := src/cce/cce_contract_adapter.c
SPECIALIST_ADAPTERS := src/specialist_adapters.c
SPECIALIST_SRC := src/specialist.c src/specialist_health.c
GAP_LANE_SRC := src/gap_lane.c
ASYNC_RUNTIME := src/async_runtime.c
MODEL_RUNTIME := src/model_runtime.c
CCE_MODEL_CATALOG := src/cce/cce_model_catalog.c
MODEL_PROBE := src/model_probe.c
CNET_LLAMA_EVAL := tools/cnet_llama_eval.cpp
LLAMA_CPP_ROOT ?= /home/marble/llama.cpp
LLAMA_CPP_BUILD ?= $(LLAMA_CPP_ROOT)/build-rocm
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
BASE_SRC := src/base.c
BASE_TEST := tests/test_base.c
FLAGSHIP_SRC := src/flagship.c
FLAGSHIP_TEST := tests/test_flagship.c
FONTDECODE_TEST := tests/test_font_decode.c
TFIDF_TEST := tests/test_tfidf.c
SYNONYMS_SRC := src/corpus/synonyms.c
SYNONYMS_TEST := tests/test_synonyms.c
TILEINDEX_TEST := tests/test_tile_index.c
CONSOLIDATE_TEST := tests/test_tile_consolidate.c

.PHONY: all run test verify verify-long demos compat unified unified_native unified_adapter unified_cce_adapter unified_oracle_adapter unified_specialist specialist_health gap_lane gap_lane_run_build dispatch_story claims oracle_v2_test soul_host_test legacy_test compose route dag hetero split chunk certify property coverage conformal logicgate decimal circuit study capacity library margin fuzzy stochastic fastpath throughput residue expr attention attention_study lifecycle_bench lbench proposal_sidecar probe_overhead belowbeam_chars struct_pref dgate_bench compounding_bench cce_smoke counterfactual_router_test sparse_kv_test narrative_coherence_test phase4_uncertainty_test register_compression_improvements phase5_integration_test cce_train_bench cce_view forest_view wordlm wordlm_bitnet cce_dll cnet_dll cce_safetensors_test cce_gguf_test cce_model_test cce_autograd_test endgate jsonstory pdftest pdflearn compound tiermem_test graduate fontdecode tfidf synonyms tileindex consolidate clean

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

counterfactual_router_test: $(CCE_ROUTER) $(COUNTERFACTUAL_ROUTER_TEST) include/cce/cce_router.h include/cce/cce_forest.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(CCE_ROUTER) $(COUNTERFACTUAL_ROUTER_TEST) $(LDFLAGS)
	./$(BIN_DIR)/counterfactual_router_test

sparse_kv_test: $(CCE_SPARSE_KV) $(SPARSE_KV_TEST) include/cce/cce_sparse_kv.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(CCE_SPARSE_KV) $(SPARSE_KV_TEST) $(LDFLAGS)
	./$(BIN_DIR)/sparse_kv_test

narrative_coherence_test: src/contract/narrative_coherence.c $(CCE_ROUTER) $(NARRATIVE_COHERENCE_TEST) include/contract/narrative_coherence.h include/cce/cce_router.h include/cce/cce_forest.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ src/contract/narrative_coherence.c $(CCE_ROUTER) $(NARRATIVE_COHERENCE_TEST) $(LDFLAGS)
	./$(BIN_DIR)/narrative_coherence_test

phase4_uncertainty_test: $(CCE_UNCERTAINTY) $(CCE_COMPRESSION) $(PHASE4_UNCERTAINTY_TEST) include/cce/cce_uncertainty.h include/cce/cce_compression.h include/cce/cce_router.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(CCE_UNCERTAINTY) $(CCE_COMPRESSION) $(PHASE4_UNCERTAINTY_TEST) $(LDFLAGS)
	./$(BIN_DIR)/phase4_uncertainty_test

register_compression_improvements: $(PHASE5_INTEGRATION_TOOL)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(PHASE5_INTEGRATION_TOOL) $(LDFLAGS)

phase5_integration_test: register_compression_improvements $(PHASE5_INTEGRATION_TEST)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(PHASE5_INTEGRATION_TEST) $(LDFLAGS)
	./$(BIN_DIR)/phase5_integration_test

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

# Loader robustness: systematic single-byte flip + truncation sweeps over
# every artifact loader. Sealed formats (.cnu/.cnb) must refuse EVERY
# mutation; unsealed probes (gguf/safetensors/.cce) must never crash.
mutate: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(CCE) tests/test_mutate.c include/contract/unit.h include/base.h include/cce/cce_archive.h include/cce/cce_detect.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(CCE) tests/test_mutate.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/mutate > logs/mutate.log 2>&1 || echo "test exited non-zero (see log)"

# Gap-triggered acquisition loop: gap ledger sidecar + oracle mining ->
# train -> certify (PROOF/SAMPLE) -> seal .cnu -> register -> replan.
# Link set mirrors the `coverage` target (+ acquire).
acquire: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(ACQUIRE_TEST) include/nn.h include/router.h include/contract/contract.h include/contract/coverage.h include/contract/unit.h include/acquire.h include/base.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(ACQUIRE_TEST) $(LDFLAGS)
	./$(BIN_DIR)/acquire > logs/acquire.log 2>&1 || echo "test exited non-zero (see log)"

# Unified base (CNB version 2 semantics under stable CNB1 magic): one sealed
# container (units + tags + stats + oracle descriptors) replacing per-unit file
# sprawl; tag governance with refusal.
base: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(BASE_TEST) include/nn.h include/router.h include/contract/contract.h include/contract/coverage.h include/contract/unit.h include/acquire.h include/base.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(BASE_TEST) $(LDFLAGS)
	./$(BIN_DIR)/base > logs/base.log 2>&1 || echo "test exited non-zero (see log)"

# Flagship harness gate (synthetic oracle, no CCE/GPU): duty-cycled,
# crash-resumable compounding run; base-as-checkpoint resume; stop file.
flagship: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(CONFORMAL) $(ACQUIRE_SRC) $(BASE_SRC) $(FLAGSHIP_SRC) $(FLAGSHIP_TEST) include/acquire.h include/base.h include/flagship.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(CONFORMAL) $(ACQUIRE_SRC) $(BASE_SRC) $(FLAGSHIP_SRC) $(FLAGSHIP_TEST) $(LDFLAGS)
	./$(BIN_DIR)/flagship > logs/flagship.log 2>&1 || echo "test exited non-zero (see log)"
	@CNET_ACQ_ADAPTIVE=1 ./$(BIN_DIR)/flagship > logs/flagship.adaptive.log 2>&1 || echo "flagship[adaptive] non-zero (see log)"
	@CNET_ACQ_WARMSTART=1 ./$(BIN_DIR)/flagship > logs/flagship.warmstart.log 2>&1 || echo "flagship[warmstart] non-zero (see log)"
	@CNET_TOPK_SET=1 ./$(BIN_DIR)/flagship > logs/flagship.topkset.log 2>&1 || echo "flagship[topkset] non-zero (see log)"
	@CNET_ACQ_ADAPTIVE=1 CNET_ACQ_WARMSTART=1 CNET_TOPK_SET=1 ./$(BIN_DIR)/flagship > logs/flagship.allon.log 2>&1 || echo "flagship[all-on] non-zero (see log)"
	@cc -O2 -w -o $(BIN_DIR)/test_dequant_xcheck tests/test_dequant_xcheck.c -lm && ./$(BIN_DIR)/test_dequant_xcheck || echo "dequant xcheck FAILED"
	@$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_f16_identity $(CCE) tests/test_f16_identity.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) && ./$(BIN_DIR)/test_f16_identity || echo "f16 identity FAILED"

# Base inspector: counts + certify-on-load + tag audit + digest fidelity
# compare between two bases. Usage: ./bin/cnb_audit <base.cnb> [other.cnb]
cnb_audit: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/cnb_audit.c include/base.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/cnb_audit.c $(LDFLAGS)

# The REAL flagship run (CCE model as oracle). NOT in verify (needs a model).
# Usage: make flagship_run_build && ./bin/flagship_run <model> [V] [max_units] [temp_C] [duty] [wall_s] [base.cnb]
# OMP: the oracle forwards dominate mining time; cce_block's tiled matvec
# pragmas parallelize across OUTPUT TILES (per-output sums keep their order),
# so threading is bit-identity-safe — verified by digest-identical re-mines
# against serial bases. Keeps -mno-avx (this exe links src/router).
flagship_run_build: CFLAGS := $(CFLAGS) $(OMPFLAGS) -DCNET_BUILD_REV='"$(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)"'
flagship_run_build: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(CONFORMAL) $(ACQUIRE_SRC) $(BASE_SRC) $(FLAGSHIP_SRC) $(CCE) $(CCE_CUDA_OBJ) tests/flagship_run.c include/flagship.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/flagship_run $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(CONFORMAL) $(ACQUIRE_SRC) $(BASE_SRC) $(FLAGSHIP_SRC) $(CCE) $(CCE_CUDA_OBJ) tests/flagship_run.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

# GPU equivalence gate: CPU vs OpenCL forward must be DECISION-identical
# (argmax + top-3) before --gpu mining is allowed. Needs model + GPU; NOT in
# verify. Usage: make gpu_equiv_build && ./bin/gpu_equiv <model> [V] [N]
gpu_equiv_build: $(CCE) tests/gpu_equiv.c include/cce/cce_clgemm.h include/cce/cce_gguf.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/gpu_equiv $(CCE) tests/gpu_equiv.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

# Decision-saturation depth probe: at which layer do the window decisions the
# oracle consumes stop changing? Measurement gate for any capped-depth oracle
# (CNET_ORACLE_LAYER_CAP). Needs the model; NOT in verify.
# Usage: make depth_probe_build && ./bin/depth_probe <model> [V] [N]
depth_probe_build: $(CCE) tests/depth_probe.c include/cce/cce_gguf.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/depth_probe $(CCE) tests/depth_probe.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

# Token-level greedy comparison vs an external reference (llama.cpp) on the
# same GGUF: the gemma4-forward validation gate. Needs the model; NOT in
# verify. Usage: make gemma4_vs_ref_build && tests/gemma4_vs_ref.py
gemma4_vs_ref_build: $(CCE) tests/gemma4_vs_ref.c include/cce/cce_gguf.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/gemma4_vs_ref $(CCE) tests/gemma4_vs_ref.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

# Exhaustive fp16 decode identity over the LIVE decoder (all 65536 patterns).
f16_identity: $(CCE) tests/test_f16_identity.c include/cce/cce_gguf.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_f16_identity $(CCE) tests/test_f16_identity.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/test_f16_identity

# Contract security + efficiency: content digests, certification cache,
# sealed (tamper-evident) contract files, certificate-to-weights binding.
.PHONY: contract_secure contract_opt_test contract_opt_sanitize contract_optimized
contract_secure: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) tests/test_contract_secure.c include/nn.h include/router.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) tests/test_contract_secure.c $(LDFLAGS)
	@./$(BIN_DIR)/contract_secure > logs/contract_secure.log 2>&1

# Focused tracer for frozen-descriptor safety, robust candidate quality, and
# single-replay promotion speed. The benchmark gate is structural (forward
# count), so CPU scheduling noise cannot produce a false pass or failure.
contract_opt_test: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) tests/test_contract_optimized.c include/nn.h include/router.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) tests/test_contract_optimized.c $(LDFLAGS)
	@./$(BIN_DIR)/contract_opt_test > logs/contract_optimized.log 2>&1
	@grep -q '^CONTRACT_OPT_BENCH iterations=2000 forwards=4000 ' logs/contract_optimized.log
	@grep -q '^CONTRACT_OPTIMIZED_PASS$$' logs/contract_optimized.log

contract_opt_sanitize: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) tests/test_contract_optimized.c include/nn.h include/router.h include/contract/contract.h
	$(CC) -std=c11 -Wall -Wextra -pedantic -O1 -g -D_DEFAULT_SOURCE \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) \
		$(CONTRACT) tests/test_contract_optimized.c $(LDFLAGS)
	@ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1 \
		./$(BIN_DIR)/contract_opt_sanitize > logs/contract_optimized_sanitize.log 2>&1
	@grep -q '^CONTRACT_OPTIMIZED_PASS$$' logs/contract_optimized_sanitize.log

# Umbrella closure gate: optimized contract semantics, contract persistence,
# security regressions, and the full historical native suite.
contract_optimized: contract_opt_test contract_opt_sanitize contract_secure contract_unit legacy_test
	@grep -q '^CONTRACT_OPTIMIZED_PASS$$' logs/contract_optimized.log
	@grep -q '^CONTRACT_OPTIMIZED_PASS$$' logs/contract_optimized_sanitize.log
	@grep -q '^All contract security tests passed\.$$' logs/contract_secure.log
	@grep -q '^All unit-file tests passed\.$$' logs/contract_unit.log
	@! grep -Eq '(^|[[:space:]])(FAIL|ERROR|SANITIZER|AddressSanitizer|runtime error)([[:space:]:]|$$)' \
		logs/contract_optimized.log logs/contract_optimized_sanitize.log \
		logs/contract_secure.log logs/contract_unit.log
	@echo CONTRACT_OPTIMIZATION_GATE_PASS

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
build_tool: $(CCE) build/build.c src/contract/mcp_utils.c src/contract/mcp_memory.c src/contract/mcp_file_write.c src/contract/mcp_web_search.c src/contract/mcp_file_read.c src/contract/mcp_wiki.c src/agent_memory.c src/contract/book_concept.c include/nn.h include/router.h include/plan_table.h include/contract/contract.h include/agent_memory.h include/contract/mcp_file_write.h include/contract/mcp_web_search.h include/contract/mcp_file_read.h include/contract/mcp_wiki.h include/contract/book_concept.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -Ibuild/include -o build_tool $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CCE) build/build.c src/contract/mcp_utils.c src/contract/mcp_memory.c src/contract/mcp_file_write.c src/contract/mcp_web_search.c src/contract/mcp_file_read.c src/contract/mcp_wiki.c src/agent_memory.c src/contract/book_concept.c src/cnet_lm.c src/contract/anti_repeat.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

build: build_tool
	./build_tool --build default

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
	$(CC) $(CFLAGS) -Wno-unused-function -DTEST_ALL -o $(BIN_DIR)/test_all \
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

# Live domain demos (decimal + circuit): these exercise the CURRENT planner
# and stay in the core verification chain.
.PHONY: demos compat
demos: decimal_demo circuit_demo
	./$(BIN_DIR)/decimal_demo > logs/decimal_demo.log 2>&1 || echo "demo exited non-zero (see log)"
	./$(BIN_DIR)/circuit_demo > logs/circuit_demo.log 2>&1 || echo "demo exited non-zero (see log)"

# The COMPAT tier (legacy quarantine): the restored historical test_all
# aggregate is back-compat coverage, not core verification — it runs here
# (and in verify-long) instead of blocking every `make test`. The gate that
# keeps the 2026-07-03 restoration from rotting again lives on, one tier out.
compat: test_all demos
	./$(BIN_DIR)/test_all > logs/legacy_test.log 2>&1 || echo "test exited non-zero (see log)"
	@sh tests/verify_logs.sh compat
	@echo "CNET_COMPAT_PASS"

# Back-compat alias for the pre-quarantine target name.
legacy: compat

# Allocation-balance gate (behavioral leak check; MinGW has no ASan).
LEAK_WRAP := tests/leak_wrap.c
LEAK_LDWRAP := -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
leakcheck: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(BASE_TEST) $(LEAK_WRAP)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(BASE_TEST) $(LEAK_WRAP) $(LEAK_LDWRAP) $(LDFLAGS)
	./$(BIN_DIR)/leakcheck > logs/leakcheck.log 2>&1 || echo "test exited non-zero (see log)"
	@grep "leakcheck" logs/leakcheck.log || true

# The test recipes above swallow their exit codes (`|| echo ...`) so the whole
# chain always runs. This recipe is the gate that makes the swallow safe: it
# runs AFTER every prerequisite (a target's recipe always follows its prereqs,
# even under -j) and re-reads each suite's log, asserting the suite's terminal
# SUCCESS marker is present -- so `make test` now exits non-zero if any gate
# failed, crashed, or produced no log. See tests/verify_logs.sh for the markers.
verify: claims_test cce_dll cce_safetensors_test cce_autograd_test cce_model_test cce_view forest_view cce_detect cce_ssm cce_st_llama cce_specgraph cce_wstore cce_tiers cce_similar merge_family hybrid_catalog supra_train contract_secure contract_unit mutate acquire base flagship demos leakcheck
	@sh tests/verify_logs.sh

# Everything verify covers PLUS the GPU equivalence gate (needs model + GPU;
# run this before any CNET_GPU=1 campaign).
test_full: test gpu_equiv_build
	./$(BIN_DIR)/gpu_equiv Models/gemma-4-12B-it-MTP-Q8_0.gguf 64 32
	dotnet test dotnet/Cce.Tests/Cce.Tests.csproj -c Release --no-restore

# `long` mode also asserts the two verify-long-only supra QAT gates (which
# likewise swallow their exit codes). The `verify` prereq already ran + gated
# the core chain first; this re-scan adds the extras.
verify-long: verify cce_train_bench supra_head_qat supra_head_qat_corpus wordlm_bitnet compat
	@sh tests/verify_logs.sh long

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
# wordlm/wordlm_bitnet/trit_bench link only CCE sources (no src/router/), so
# they are AVX-safe too; OMP activates the row-parallel packed-trit kernels.
cce_safetensors_test supra_console supra_chat_mock supra_context_probe supra_longform supra_head_qat supra_head_qat_corpus wordlm wordlm_bitnet trit_bench: CFLAGS := $(AVX_CFLAGS) $(OMPFLAGS)

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

# Fine-tune-family merge pipeline (model-merge scope M0): base + N fine-tunes
# in ONE content-addressed store — storage accounting vs naive, per-manifest
# bit-identity under a HOT cap, epsilon-merge accept + material refuse inside
# the pipeline. Hermetic (tiny fixture).
merge_family: $(CCE) $(CCE_CUDA_OBJ) tests/merge_family_test.c tests/tiny_model_fixture.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/merge_family_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/merge_family > logs/merge_family.log 2>&1 || echo "test exited non-zero (see log)"

# Hybrid catalog (model-merge scope M1): transformer + SSM in ONE store with
# a query-level task catalog; ssm restore round-trip (closes the stated
# limit); HONESTY gate measures cross-arch dedup (= 0). Hermetic.
hybrid_catalog: $(CCE) $(CCE_CUDA_OBJ) tests/hybrid_catalog_test.c tests/tiny_model_fixture.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/hybrid_catalog_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/hybrid_catalog > logs/hybrid_catalog.log 2>&1 || echo "test exited non-zero (see log)"

# Supra QAT trainer gate: hermetic transformer-backward gradcheck (central
# differences over EVERY parameter group) + determinism + FP smoke + QAT-vs-
# post-hoc on a tiny synthetic model. No model files. The joint-QAT quality
# phase (docs/superpowers/specs/2026-06-28-supra-qat-scope.md steps 7-10)
# stands on this backward.
supra_train: $(CCE) $(CCE_CUDA_OBJ) tests/test_supra_train.c include/cce/cce_supra_train.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/test_supra_train.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/supra_train > logs/supra_train.log 2>&1 || echo "test exited non-zero (see log)"

# Trit-kernel micro-benchmark: FP vs int8 vs packed 1.6-bit forward on a
# Supra-head-shaped block + the packed word-LM predict loop. Carries its own
# parity gate (trit MUST stay bit-identical to int8 ternary). Budgeted;
# NOT part of make test.
trit_bench: $(CCE) $(CCE_CUDA_OBJ) tests/trit_bench.c include/cce/cce_trit_lut.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/trit_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/trit_bench

# DLL target for .NET / P/Invoke / C# interop (and other hosts).
# Builds cce.dll (Windows) or cce.so (else). Defines CCE_BUILD_DLL so headers
# emit __declspec(dllexport) / visibility for the C ABI (model, dataset, handle).
# Usage: make cce_dll   (then copy cce.dll next to your .exe or into PATH)
# -fPIC: required for ELF shared objects (Linux); harmless on MinGW.
cce_dll: CFLAGS := $(CFLAGS) -fPIC
cce_dll: $(CCE) $(CCE_CUDA_OBJ)
	$(CC) -shared -DCCE_BUILD_DLL $(CFLAGS) -o cce.dll $(CCE) $(CCE_CUDA_OBJ) $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	@echo "Built cce.dll (for .NET P/Invoke). Add to your C# project and use DllImport."

# Unified CNET shared library: CCE runtime + certified base/registry/planner +
# soul_host + MCP tools. `cce_dll` remains a native compatibility artifact;
# managed hosts bind only this superset so there is one native ABI truth.
# Defines both export macros.
# Usage: make cnet_dll
cnet_dll: CFLAGS := $(CFLAGS) -fPIC
cnet_dll: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(BASE_SRC) $(SCAN) $(PROPERTY) $(CONSOLIDATE) $(ACQUIRE_SRC) $(COVERAGE) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) $(ASYNC_RUNTIME) $(MODEL_RUNTIME) $(CCE_MODEL_CATALOG) $(MODEL_PROBE) src/fastpath.c src/soul_host.c src/contract/mcp_calculator.c src/contract/mcp_file_read.c src/contract/mcp_file_write.c src/contract/mcp_memory.c src/contract/mcp_utils.c src/contract/mcp_web_search.c src/contract/mcp_wiki.c src/agent_memory.c
	$(CC) -shared -DCNET_BUILD_DLL -DCCE_BUILD_DLL $(CFLAGS) $(CUDA_CFLAGS) -o cnet.so \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(BASE_SRC) $(SCAN) $(PROPERTY) $(CONSOLIDATE) $(ACQUIRE_SRC) $(COVERAGE) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) $(ASYNC_RUNTIME) $(MODEL_RUNTIME) $(CCE_MODEL_CATALOG) $(MODEL_PROBE) src/fastpath.c src/soul_host.c src/contract/mcp_calculator.c src/contract/mcp_file_read.c src/contract/mcp_file_write.c src/contract/mcp_memory.c src/contract/mcp_utils.c src/contract/mcp_web_search.c src/contract/mcp_wiki.c src/agent_memory.c \
		$(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@echo "Built unified cnet.so (CCE + certified runtime + soul_host + MCP ABI)."

# CNET-native QGKP v3 envelope tool. Pack/materialize operations are resumable
# and validate prefixes before append; the final rename remains an external
# atomic policy decision.
.PHONY: cnet_qgkp
cnet_qgkp: cnet_dll tools/cnet_qgkp.c include/cce/cce_qgkp.h
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O2 -Iinclude \
		-o $(BIN_DIR)/cnet_qgkp tools/cnet_qgkp.c -L. -l:cnet.so \
		-Wl,-rpath,'$$ORIGIN/..'

# Optional real-model tracer bullet: CNET owns catalog/residency/placement while
# llama.cpp materializes the dense GGUF and performs token generation. Keep it
# outside `unified` because it requires a local ROCm llama.cpp build and model.
.PHONY: cnet_llama_eval
cnet_llama_eval: cnet_dll $(CNET_LLAMA_EVAL)
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 -Iinclude \
		-I$(LLAMA_CPP_ROOT)/include -I$(LLAMA_CPP_ROOT)/ggml/include \
		-o $(BIN_DIR)/cnet_llama_eval $(CNET_LLAMA_EVAL) \
		-L. -l:cnet.so -L$(LLAMA_CPP_BUILD)/bin -lllama -lggml -lggml-base -ldl -pthread \
		-Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,$(LLAMA_CPP_BUILD)/bin
	@echo "Built $(BIN_DIR)/cnet_llama_eval (CNET-governed llama.cpp dense inference)."

.PHONY: qwythos_coherence_gate
qwythos_coherence_gate: tools/score_cnet_coherence.py tests/test_qwythos_coherence.sh
	@bash tests/test_qwythos_coherence.sh

QWYTHOS_GGUF ?= /home/marble/Downloads/Qwythos-9B-Claude-Mythos-5-1M-MTP-Q8_0.gguf
QWYTHOS_QGKP ?= $(CURDIR)/hermes_wrappers/Qwythos-9B-Claude-Mythos-5-1M-MTP-QGKP-v3.cnetpack
QWYTHOS_QGKP_SOURCE ?= $(CURDIR)/hermes_wrappers/Qwythos-9B-Claude-Mythos-5-1M-MTP-TQ1_0-fixed.cnetpack
QWYTHOS_GPU ?= 1
QWYTHOS_RESOURCE ?= gpu1

.PHONY: qwythos_qgkp_acceptance
qwythos_qgkp_acceptance: cnet_qgkp cnet_llama_eval unified_models
	@test -f "$(QWYTHOS_QGKP)"
	@test -f "$(QWYTHOS_QGKP_SOURCE)"
	@./$(BIN_DIR)/cnet_qgkp inspect "$(QWYTHOS_QGKP)" | tee logs/qwythos_qgkp_inspect.log
	@timeout --signal=TERM --kill-after=20s 180s ./$(BIN_DIR)/cnet_qgkp materialize \
		"$(QWYTHOS_QGKP)" "$(QWYTHOS_QGKP).gguf.cache"
	@src_hash=$$(sha256sum "$(QWYTHOS_QGKP_SOURCE)" | cut -d' ' -f1); \
	 cache_hash=$$(sha256sum "$(QWYTHOS_QGKP).gguf.cache" | cut -d' ' -f1); \
	 test "$$src_hash" = "$$cache_hash"; \
	 printf 'QGKP_SHA256_PASS %s\n' "$$src_hash" | tee logs/qwythos_qgkp_sha256.log
	timeout --signal=TERM --kill-after=20s 420s env ROCR_VISIBLE_DEVICES=$(QWYTHOS_GPU) \
		./$(BIN_DIR)/cnet_llama_eval --model "$(QWYTHOS_QGKP)" \
		--prompts logs/qwythos_coherence_prompts.tsv \
		--output logs/qwythos_qgkp_gpu1_results.jsonl \
		--resource $(QWYTHOS_RESOURCE) --main-gpu 0 --ctx 4096 \
		--max-tokens 220 --temperature 0 --seed 424242 \
		> logs/qwythos_qgkp_gpu1_run.log 2>&1
	@bash tests/test_qwythos_coherence.sh \
		logs/qwythos_qgkp_gpu1_results.jsonl \
		logs/qwythos_qgkp_gpu1_coherence_report.json \
		| tee logs/qwythos_qgkp_gpu1_score.log

.PHONY: qwythos_gpu_acceptance
qwythos_gpu_acceptance: cnet_llama_eval
	@test -f "$(QWYTHOS_GGUF)"
	timeout --signal=TERM --kill-after=20s 240s env ROCR_VISIBLE_DEVICES=$(QWYTHOS_GPU) \
		./$(BIN_DIR)/cnet_llama_eval --model "$(QWYTHOS_GGUF)" \
		--prompts logs/qwythos_coherence_prompts.tsv \
		--output logs/qwythos_cnet_gpu1_results.jsonl \
		--resource $(QWYTHOS_RESOURCE) --main-gpu 0 --ctx 4096 \
		--max-tokens 220 --temperature 0 --seed 424242 \
		> logs/qwythos_cnet_gpu1_run.log 2>&1
	@$(MAKE) --no-print-directory qwythos_coherence_gate

# CPU-only unified-runtime tracer bullets. These deliberately avoid model files
# and GPUs: synthetic sealed CNB -> certified registry -> planner/executor,
# generic runtime adapter, real in-memory CCE model adapter, native host, .NET,
# and stdio MCP all traverse the same public cnet.so boundary.
unified_adapter: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) tests/test_unified_runtime.c
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) tests/test_unified_runtime.c $(LDFLAGS)
	./$(BIN_DIR)/$@ > logs/unified_adapter.log 2>&1
	@grep -q "UNIFIED_ADAPTER_PASS" logs/unified_adapter.log

unified_cce_adapter: $(CCE) $(CNET_CCE_ADAPTER) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) tests/test_cce_contract_adapter.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CNET_CCE_ADAPTER) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) tests/test_cce_contract_adapter.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/$@ > logs/unified_cce_adapter.log 2>&1
	@grep -q "UNIFIED_CCE_ADAPTER_PASS" logs/unified_cce_adapter.log

unified_oracle_adapter: $(SPECIALIST_ADAPTERS) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_oracle_contract_adapter.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_oracle_contract_adapter \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(SPECIALIST_ADAPTERS) \
		tests/test_oracle_contract_adapter.c $(LDFLAGS)
	@./$(BIN_DIR)/test_oracle_contract_adapter > logs/unified_oracle_adapter.log 2>&1
	@grep -q "ORACLE_CONTRACT_ADAPTER_PASS" logs/unified_oracle_adapter.log

# The gap-lane gate: the 24/7 learning loop, hermetic. Serving misses land
# in the inbox, the tick ingests + health-bridges, the drain teaches from a
# bound oracle (dynamic-growth students, certified, sealed), checkpoints are
# atomic for base AND ledger, and a fresh lane resumes from disk.
.PHONY: gap_lane
gap_lane: $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_gap_lane.c include/gap_lane.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_gap_lane \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(GAP_LANE_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) \
		$(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) \
		tests/test_gap_lane.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_gap_lane > logs/gap_lane.log 2>&1
	@grep -q "GAP_LANE_PASS" logs/gap_lane.log

# The 24/7 daemon (REAL local-model teacher via the CCE GGUF runner).
.PHONY: gap_lane_run_build
gap_lane_run_build: CFLAGS := $(CFLAGS) $(OMPFLAGS)
gap_lane_run_build: $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/gap_lane_run.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/gap_lane_run \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(GAP_LANE_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) \
		$(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) \
		tests/gap_lane_run.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread

# The one-dispatch-story gate (docs/dispatch.md): recall dispatches INSIDE
# a specialist under its contract; certification outranks everything across
# specialists; among the certified, learned reliability ranks.
.PHONY: dispatch_story
dispatch_story: $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_dispatch_story.c docs/dispatch.md
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_dispatch_story \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) \
		tests/test_dispatch_story.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_dispatch_story > logs/dispatch_story.log 2>&1
	@grep -q "DISPATCH_STORY_PASS" logs/dispatch_story.log

# The heterogeneous-plan acceptance gate: ONE ordinary planner plan whose
# nodes are a native BTN, a real CCE model, and an Oracle unit, all admitted
# through the single Specialist door (specialist_admit) and certified
# end-to-end. Passing means the three backends are the same planning citizen,
# not three coexisting subsystems.
.PHONY: specialist_unit
specialist_unit: $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_specialist.c include/specialist.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_specialist \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) \
		tests/test_specialist.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_specialist > logs/specialist_unit.log 2>&1
	@grep -q "SPECIALIST_UNIT_PASS" logs/specialist_unit.log

# Runtime health optimizer gate: one maintenance pass fixes (audit -> label
# from contract/teacher -> heal via retrain + re-certify) and improves
# (evidence promotion, shadow hot-swap) through existing certified paths
# only; healthy registry = proven no-op; zero-init config = total no-op.
.PHONY: specialist_health
specialist_health: $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_specialist_health.c include/specialist_health.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_specialist_health \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) \
		tests/test_specialist_health.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_specialist_health > logs/specialist_health.log 2>&1
	@grep -q "SPECIALIST_HEALTH_PASS" logs/specialist_health.log

unified_specialist: specialist_unit specialist_health $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_heterogeneous_plan.c include/specialist.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_heterogeneous_plan \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) \
		tests/test_heterogeneous_plan.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_heterogeneous_plan > logs/unified_specialist.log 2>&1
	@grep -q "HET_PLAN_PASS" logs/unified_specialist.log

oracle_v2_test: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_oracle_v2.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_oracle_v2 \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_oracle_v2.c $(LDFLAGS)
	@./$(BIN_DIR)/test_oracle_v2 > logs/oracle_v2_test.log 2>&1
	@grep -q "ORACLE_V2_PASS" logs/oracle_v2_test.log

.PHONY: unified_async
unified_async: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(ASYNC_RUNTIME) tests/test_async_runtime.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_async_runtime \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(ASYNC_RUNTIME) \
		tests/test_async_runtime.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_async_runtime > logs/unified_async.log 2>&1
	@grep -q "ASYNC_RUNTIME_PASS" logs/unified_async.log

.PHONY: qgkp_envelope_test
qgkp_envelope_test: $(CCE_QGKP) tests/test_qgkp_envelope.c include/cce/cce_qgkp.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O2 -Iinclude \
		-o $(BIN_DIR)/test_qgkp_envelope $(CCE_QGKP) tests/test_qgkp_envelope.c
	@./$(BIN_DIR)/test_qgkp_envelope > logs/qgkp_envelope_test.log 2>&1
	@grep -q "QGKP_ENVELOPE_PASS" logs/qgkp_envelope_test.log

.PHONY: unified_models
unified_models: qgkp_envelope_test $(MODEL_RUNTIME) $(CCE_MODEL_CATALOG) $(MODEL_PROBE) tests/test_model_runtime.c tests/test_model_catalog.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O2 -Iinclude \
		-o $(BIN_DIR)/test_model_runtime $(MODEL_RUNTIME) tests/test_model_runtime.c -pthread
	@./$(BIN_DIR)/test_model_runtime > logs/unified_models_runtime.log 2>&1
	@grep -q "MODEL_RUNTIME_PASS" logs/unified_models_runtime.log
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -Iinclude -o $(BIN_DIR)/test_model_catalog \
		$(CCE) $(CCE_MODEL_CATALOG) $(MODEL_PROBE) tests/test_model_catalog.c \
		$(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_model_catalog > logs/unified_models_catalog.log 2>&1
	@grep -q "MODEL_CATALOG_PASS" logs/unified_models_catalog.log

.PHONY: unified_ds4_launcher
unified_ds4_launcher: scripts/run_cnet_ds4_dual.sh scripts/cnet_chunk_hash.py scripts/verify_ds4_endpoint.py tests/test_ds4_dual_launcher.sh
	@mkdir -p logs
	@bash tests/test_ds4_dual_launcher.sh > logs/unified_ds4_launcher.log 2>&1
	@grep -q "DS4_DUAL_LAUNCHER_PASS" logs/unified_ds4_launcher.log

.PHONY: unified_gpu
unified_gpu: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(ASYNC_RUNTIME) $(CCE_CLGEMM) tests/test_async_gpu_lanes.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_async_gpu_lanes \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(ASYNC_RUNTIME) \
		$(CCE_CLGEMM) tests/test_async_gpu_lanes.c $(LDFLAGS) -ldl -pthread
	@./$(BIN_DIR)/test_async_gpu_lanes > logs/unified_gpu.log 2>&1
	@grep -q "ASYNC_GPU_LANES_PASS" logs/unified_gpu.log

.PHONY: oracle_v2_bench
oracle_v2_bench: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/oracle_v2_bench.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/oracle_v2_bench \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/oracle_v2_bench.c $(LDFLAGS)
	@./$(BIN_DIR)/oracle_v2_bench > logs/oracle_v2_bench.log
	@grep -q "Oracle v2 governed invocation benchmark" logs/oracle_v2_bench.log

soul_host_test: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) src/soul_host.c tests/test_soul_host.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) src/soul_host.c tests/test_soul_host.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	CNET_KEEP_TEST_BASE=1 ./$(BIN_DIR)/$@ > logs/soul_host_test.log 2>&1
	@grep -q "SOUL_HOST_UNIFIED_PASS" logs/soul_host_test.log

unified_native: unified_adapter unified_cce_adapter unified_oracle_adapter unified_specialist gap_lane dispatch_story oracle_v2_test unified_async unified_models unified_ds4_launcher soul_host_test cnet_dll
	@for sym in specialist_wrap_btn specialist_wrap_cce_model \
		specialist_wrap_oracle specialist_admit specialist_axes \
		specialist_residency_of_model specialist_residency_of_branch \
		specialist_residency_of_entry \
		specialist_health_pass specialist_health_config_defaults \
		specialist_residency_from_cce_tier specialist_residency_from_model_state \
		specialist_kind_name specialist_trust_name specialist_residency_name \
		specialist_role_name; do \
		nm -D cnet.so | grep -q " $$sym$$" || exit 1; \
	done
	@nm -D cnet.so | grep -q " soul_unit_count$$"
	@nm -D cnet.so | grep -q " gap_lane_tick$$"
	@nm -D cnet.so | grep -q " gap_inbox_note_no_plan$$"
	@nm -D cnet.so | grep -q " soul_health_tick$$"
	@nm -D cnet.so | grep -q " soul_request$$"
	@nm -D cnet.so | grep -q " soul_unit_axes$$"
	@nm -D cnet.so | grep -q " cce_model_init_contract_adapter$$"
	@nm -D cnet.so | grep -q " cnet_oracle_init_contract_adapter$$"
	@nm -D cnet.so | grep -q " cnet_oracle_invoke$$"
	@nm -D cnet.so | grep -q " cnet_oracle_identity_digest$$"
	@nm -D cnet.so | grep -q " cnet_model_manager_open$$"
	@nm -D cnet.so | grep -q " cce_model_descriptor_probe$$"
	@nm -D cnet.so | grep -q " cnet_lane_pool_open$$"
	@nm -D cnet.so | grep -q " cnet_lane_pool_submit$$"
	@nm -D cnet.so | grep -q " soul_oracle_count$$"
	@nm -D cnet.so | grep -q " soul_oracle_identity$$"


.PHONY: unified
unified:
	@mkdir -p logs
	@rm -f logs/unified.started
	@touch logs/unified.started
	@$(MAKE) --no-print-directory unified_native
	dotnet build dotnet/CceHost/CceHost.csproj -c Release --no-restore --nologo -v:q > logs/unified_dotnet_build.log 2>&1
	dotnet build dotnet/CnetMcpServer/CnetMcpServer.csproj -c Release --no-restore --nologo -v:q >> logs/unified_dotnet_build.log 2>&1
	LD_LIBRARY_PATH="$(CURDIR)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}" dotnet dotnet/CceHost/bin/Release/net10.0/CceHost.dll --list tmp_soul_host.cnb > logs/unified_host.log 2>&1
	@grep -q "CNET_HOST_UNIFIED_PASS" logs/unified_host.log
	@printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}' '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"cnet_list_units","arguments":{}}}' '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"cnet_list_oracles","arguments":{}}}' '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"cnet_health_tick","arguments":{}}}' '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"cnet_request_capability","arguments":{"goal_tag":"unified_novel_probe","in_tag":"unified_input","width":4}}}' | CNET_BASE_PATH="$(CURDIR)/tmp_soul_host.cnb" CNET_GAP_INBOX="$(CURDIR)/tmp_soul_host.cnb.inbox" LD_LIBRARY_PATH="$(CURDIR)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}" dotnet dotnet/CnetMcpServer/bin/Release/net10.0/CnetMcpServer.dll > logs/unified_mcp.log 2> logs/unified_mcp.stderr.log
	@grep -q '"name":"cnet-mcp"' logs/unified_mcp.log
	@grep -q "acq_unified_goal" logs/unified_mcp.log
	@grep -q "unified_teacher" logs/unified_mcp.log
	@grep -q "descriptor_only_not_runtime_trust" logs/unified_mcp.log
	@grep -q "reset_remaining" logs/unified_mcp.log
	@grep -qE 'gap_noted[^:]*:true' logs/unified_mcp.log
	@grep -q "NO_PLAN" tmp_soul_host.cnb.inbox
	LD_LIBRARY_PATH="$(CURDIR)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}" dotnet test dotnet/Cce.Tests/Cce.Tests.csproj -c Release --no-restore --nologo -v:q > logs/unified_dotnet_test.log 2>&1
	@rm -f tmp_soul_host.cnb tmp_soul_host.cnb.tmp tmp_soul_host.cnb.inbox
	@$(MAKE) --no-print-directory claims_test
	@bash scripts/gen_claims.sh --strict --scope unified --since logs/unified.started
	@rm -f logs/unified.started
	@echo "CNET_UNIFIED_PASS"

# Generated claims ledger. `make claims` executes the unified CPU gate and emits
# run-scoped evidence fresh relative to its sentinel. `make claims_all` merely
# inventories every known log (including standalone GPU evidence); its PASS is
# marker presence, not current-run provenance.
.PHONY: claims claims_all claims_test
claims_test: tests/test_claims.sh scripts/gen_claims.sh
	@bash tests/test_claims.sh

claims: unified
	@grep -q '^# Verified Today (generated)' docs/verified-today.generated.md

claims_all: claims_test
	@bash scripts/gen_claims.sh --strict


# Unit-structure probe: how hard is the function each TOPK unit must memorize?
# The near-miss-vs-fundamental discriminator. Needs the model; NOT in verify.
unit_structure_build: $(CCE) tests/unit_structure.c include/cce/cce_gguf.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/unit_structure $(CCE) tests/unit_structure.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

# Ask the soul questions: run certified base units vs the live model.
soul_query_build: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(CCE) tests/soul_query.c include/base.h include/cce/cce_detect.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/soul_query $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(CCE) tests/soul_query.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

# Run the model on explicit token ids (enabler for real-context extraction).
tok_forward_build: $(CCE) tests/tok_forward.c include/cce/cce_detect.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/tok_forward $(CCE) tests/tok_forward.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

gguf_dump_build: $(CCE) tests/gguf_dump.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/gguf_dump $(CCE) tests/gguf_dump.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

gemma_ref_build: $(CCE) tests/gemma_ref.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/gemma_ref $(CCE) tests/gemma_ref.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)


# AICIMO test target (pure C)
aicimo_smoke: $(CCE) tests/aicimo_smoke.c
	$(CC) $(CFLAGS) -o $(BIN_DIR)/aicimo_smoke $(CCE) tests/aicimo_smoke.c $(LDFLAGS)
	./$(BIN_DIR)/aicimo_smoke
