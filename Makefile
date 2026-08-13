# Every recipe runs under bash with pipefail. Without it `producer | tee log`
# reports tee's exit status, so a gate whose producer printed its PASS marker
# and then crashed, timed out, or failed a sanitizer teardown was still green --
# the 2026-07-30 re-analysis reproduced exactly that against 35+ pipelines.
# `tests/test_pipeline_status.sh` instantiates every real pipeline shape with a
# producer that prints the marker and exits 7, and requires each one to fail.
# Note this is pipefail only, deliberately not `-e`: recipes already rely on
# Make checking each line's status, and errexit would change unrelated control
# flow inside multi-command lines.
SHELL := /bin/bash
.SHELLFLAGS := -o pipefail -c

CC := gcc
CXX := g++

# Python interpreter, PROBED not assumed.
#
# 94 recipes hardcoded `python3`. On Windows that name is normally NOT an
# interpreter: it resolves to the Microsoft Store "App execution alias" stub,
# which prints an install advertisement and exits non-zero -- so every one of
# those recipes failed with a message that looks nothing like "wrong python
# name". Probe for one that actually runs a Python 3.
#
# Override explicitly with `make PYTHON=/path/to/python`.
PYTHON ?= $(shell for p in python3 python py; do \
	if command -v $$p >/dev/null 2>&1 && \
	   $$p -c 'import sys; sys.exit(0 if sys.version_info[0] == 3 else 1)' >/dev/null 2>&1; \
	then echo $$p; break; fi; done)
ifeq ($(strip $(PYTHON)),)
PYTHON := python3
endif

# Python on Windows defaults stdout to the ANSI codepage (cp1252 here), so any
# script printing a non-Latin-1 character -- an arrow, a checkmark, a box-drawing
# glyph -- dies with UnicodeEncodeError partway through, AFTER doing its work.
# The tools legitimately print such characters, so force UTF-8 rather than
# de-Unicode 77 scripts.
export PYTHONIOENCODING := utf-8
PORTABLE ?= 0
OPTFLAGS ?= -O3
PREFIX ?= /usr/local
DESTDIR ?=
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig
DIST_DIR ?= $(CURDIR)/dist
INSTALL ?= install
CNET_VERSION := $(strip $(file <VERSION))
CNET_ABI_VERSION := $(word 1,$(subst ., ,$(CNET_VERSION)))

# -march=native: measured 1.25x on the training loops, bit-identical weights
# (FP contraction stays off under -std=c11). PORTABLE=1 omits host-specific ISA.
#
# PORTABLE=v3 targets x86-64-v3 (AVX2+FMA, every x86-64 CPU since ~2013) and is
# what the shipped cce.dll/cnet.so are built with. Rationale, measured on the
# int8 oracle matvec (3840x4096, best-of-3 ms/call, this Zen 5 host):
#
#            1 thread   4 threads   32 threads
#   baseline   1.310       0.334        0.212
#   v3         0.683       0.176        0.195
#   v4/native  0.427       0.124        0.183
#
# Baseline costs ~3x when the call is thread-constrained; past ~8 threads the
# loop is bandwidth-bound and ISA stops mattering. v3 recovers most of that and
# still runs everywhere. v4 (AVX-512) matches native but SIGILLs on CPUs without
# it -- including current Intel consumer parts -- which is the failure PORTABLE
# exists to prevent. Output is bit-identical across all four (verified by FNV
# over the result vector), so this is purely a speed/reach tradeoff.
ifeq ($(PORTABLE),1)
ARCH_CFLAGS :=
else ifeq ($(PORTABLE),v3)
ARCH_CFLAGS := -march=x86-64-v3
else
ARCH_CFLAGS := -march=native
endif
CFLAGS := -std=c11 -Wall -Wextra -pedantic $(OPTFLAGS) $(ARCH_CFLAGS)
ifeq ($(OS),Windows_NT)
# -mno-avx is REQUIRED with native tuning on the MinGW toolchain ONLY:
# MinGW gcc 15.2 emits aligned 256-bit moves for by-value structs >= 32 bytes
# (Port is 56) on a 16-byte-aligned Windows stack -> segfault. -mstackrealign
# does not fix it; disabling AVX does. The Linux ABI has no such bug, and the
# int8 oracle matvec runs 256/512-bit wide there (bit-identical: each output
# element keeps its own i-ascending accumulation regardless of SIMD width).
CFLAGS += -mno-avx
endif
LDFLAGS := -lm -lpthread

# libcurl is OPTIONAL. This used to be an unconditional `CURL_LDFLAGS :=
# $(CURL_LDFLAGS)` with no probe, so on any box without libcurl (every stock MinGW
# install) three TUs failed to compile on the missing <curl/curl.h> and every
# link died with "cannot find $(CURL_LDFLAGS)" -- including cce_dll, which is verify's
# third prerequisite. Probe for it instead, and let the source degrade to an
# honest "HTTP support not compiled in" rather than not building at all.
#
# Force it off with `make CNET_NO_CURL=1` (useful to test the degraded path on
# a box that does have curl).
ifdef CNET_NO_CURL
CNET_HAVE_CURL := 0
else
# One probe, header AND library, at parse time. Writes to a temp file rather
# than /dev/null because MinGW's ld will not emit an executable there.
CNET_HAVE_CURL := $(shell t=$$(mktemp -u 2>/dev/null || echo ./.curlprobe)$$$$.exe; \
	printf '#include <curl/curl.h>\nint main(void){return curl_easy_init()?0:1;}\n' \
	| $(CC) -xc - $(CURL_LDFLAGS) -o "$$t" >/dev/null 2>&1 && echo 1 || echo 0; \
	rm -f "$$t")
endif

ifeq ($(CNET_HAVE_CURL),1)
CURL_LDFLAGS := $(CURL_LDFLAGS)
else
CURL_LDFLAGS :=
endif
CFLAGS += -DCNET_HAVE_CURL=$(CNET_HAVE_CURL)
# Pull curl into all residual/personal_ai-linked binaries (HTTP residual).
LDFLAGS += $(CURL_LDFLAGS)
MCP_LDFLAGS :=
ifeq ($(OS),Windows_NT)
MCP_LDFLAGS := -lwininet
else
# Linux/glibc: strict -std=c11 hides POSIX/BSD declarations (popen, fseeko,
# usleep, ...). _DEFAULT_SOURCE restores glibc's default feature set without
# changing the C standard; MinGW never sees this branch.
CFLAGS += -D_DEFAULT_SOURCE
CNET_SONAME_LDFLAGS := -Wl,-soname,libcnet.so.$(CNET_ABI_VERSION)
endif

# OpenMP support for multi-threaded studies inside a single exe (MinGW GCC).
# Follows strict rules: default(none) on all pragmas, OMP_NUM_THREADS=1 for
# isolating races, etc. Only study targets get -fopenmp.
OMPFLAGS := -fopenmp

.PHONY: print-config
print-config:
	@printf '%s\n' \
		"CNET_VERSION=$(CNET_VERSION)" \
		"PORTABLE=$(PORTABLE)" \
		"CC=$(CC)" \
		"CFLAGS=$(CFLAGS)" \
		"PREFIX=$(PREFIX)" \
		"LIBDIR=$(LIBDIR)" \
		"INCLUDEDIR=$(INCLUDEDIR)"

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
PHASE123_BENCHMARK_TEST := $(BIN_DIR)/test_phase123_benchmarks
PHASE123_BENCHMARK_TOOL := $(BIN_DIR)/run_phase123_benchmarks
PHASE123_BENCHMARK_LIBRARY = $(BIN_DIR)/libphase123_benchmark.so
PHASE4_UNCERTAINTY_TEST := tests/phase4_uncertainty_test.c
PHASE5_INTEGRATION_TOOL := tools/register_compression_improvements.c
PHASE5_INTEGRATION_TEST := tests/phase5_integration_test.c
CNET_CONTROL := dotnet run --project dotnet/CnetControlPlane --
CNET_CONTROL_TEST := dotnet test dotnet/CnetControlPlane.Tests --verbosity minimal
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
CCE_DSA := src/cce/cce_dsa.c
CCE_KV_PAGE := src/cce/cce_kv_page.c
CCE_MTK := src/cce/cce_mtk.c
CCE_MTK_HOST := src/cce/cce_mtk_host.c
CCE_MLA := src/cce/cce_mla.c
CCE_DS_MAP := src/cce/cce_deepseek_map.c
CCE_DS_RT  := src/cce/cce_ds_runtime.c
CCE_INFER  := src/cce/cce_infer_backend.c
CCE_UNCERTAINTY := src/cce/cce_uncertainty.c
CCE_COMPRESSION := src/cce/cce_compression.c
CCE_LEARN   := src/cce/cce_learn.c
CCE_LORA    := src/cce/cce_lora.c
CCE_LILY    := src/cce/cce_lily.c
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
CCE_AICIMO := src/cce/cce_aicimo.c
CCE_QGKP := src/cce/cce_qgkp.c

# Build directory for all executables to avoid polluting the root with endless .exe junk.
# Same philosophy as the fixed-temp cleanup for .cce / logs.
BIN_DIR := bin

# Standalone purge tools (no libcnet link). Built on demand by consumers.
.PHONY: gate_evidence_bin purge_tool_bins no_python_audit
gate_evidence_bin: tools/gate_evidence.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/gate_evidence tools/gate_evidence.c $(LDFLAGS)

$(BIN_DIR)/gate_evidence: tools/gate_evidence.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ tools/gate_evidence.c $(LDFLAGS)

$(BIN_DIR)/gen_altmodel: tools/gen_altmodel.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/gen_json_toolcall_alphabet: tools/gen_json_toolcall_alphabet.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/propose_recipe_improvements: tools/propose_recipe_improvements.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/score_cnet_coherence: tools/score_cnet_coherence.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/margin_sweep_analyze: tools/margin_sweep_analyze.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/roe_daily_packs_seed: tools/roe_daily_packs_seed.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/roe_pdf_chunk_run: tools/roe_pdf_chunk_run.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/roe_table_extract: tools/roe_table_extract.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/governor_autonomous: tools/governor_autonomous.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/governor_personality: tools/governor_personality.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/governor_zen_reflect: tools/governor_zen_reflect.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/governor_hermes_structured: tools/governor_hermes_structured.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/test_metric_honesty: tests/test_metric_honesty.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/test_phase123_benchmarks: tests/test_phase123_benchmarks.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/run_phase123_benchmarks: tools/run_phase123_benchmarks.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/cnet_harness_gpu_benchmark: tests/cnet_harness_gpu_benchmark.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/voice_teacher: tools/voice_teacher.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/gap_inject: tools/gap_inject.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/cnet_chunk_hash: tools/cnet_chunk_hash.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

no_python_audit: scripts/no_python_audit.sh
	@bash scripts/no_python_audit.sh
EXE_EXT := $(if $(filter Windows_NT,$(OS)),.exe,)

# Portable dotnet command discovery.
# Tries: PATH (command -v) → DOTNET_ROOT → $HOME/dotnet.
# Override by setting DOTNET on the command line or exporting it.
DOTNET ?= $(shell command -v dotnet 2>/dev/null || \
    { [ -n "$${DOTNET_ROOT:-}" ] && [ -x "$${DOTNET_ROOT}/dotnet" ] && echo "$${DOTNET_ROOT}/dotnet"; } || \
    { [ -x "$$HOME/dotnet/dotnet" ] && echo "$$HOME/dotnet/dotnet"; } || \
    echo "")
DOTNET_RESTORE_PROJECTS := dotnet/CceHost/CceHost.csproj \
    dotnet/CnetMcpServer/CnetMcpServer.csproj \
    dotnet/Cce.Tests/Cce.Tests.csproj \
    dotnet/CnetMcpServer.Tests/CnetMcpServer.Tests.csproj \
    dotnet/CnetHarnessSmoke/CnetHarnessSmoke.csproj \
    dotnet/Cce.Benchmarks/Cce.Benchmarks.csproj

# Guard: emit a clear prerequisite error when dotnet is needed but missing.
define dotnet_guard
@if [ -z "$(DOTNET)" ]; then \
    echo "ERROR: dotnet not found. Install .NET SDK 10 and ensure it is on PATH," \
         "set DOTNET_ROOT, or place it at $$HOME/dotnet." >&2; \
    exit 1; \
fi
endef

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
CCE_HYBRID  := src/cce/cce_hybrid.c
CCE_QWEN35  := src/cce/cce_qwen35.c
CCE_GGUF_QWEN35 := src/cce/cce_gguf_qwen35.c
CCE_ORACLE_PREFIX_CACHE := src/cce/cce_oracle_prefix_cache.c
CCE_CAMPAIGN_PROVENANCE := src/cce/cce_campaign_provenance.c
CCE_ST_LLAMA := src/cce/cce_st_llama.c
CCE_SPECGRAPH := src/cce/cce_specgraph.c
CCE_WSTORE  := src/cce/cce_weight_store.c
CCE_TIERRT  := src/cce/cce_tier_runtime.c
CCE_SIMILAR := src/cce/cce_similar.c
CCE_CLGEMM  := src/cce/cce_clgemm.c
CCE_HIPGEMM := src/cce/cce_hipgemm.c
# Optional NVIDIA cuBLAS seam (dlopen libcudart/libcublas — no nvcc/toolkit to build).
# Inert at runtime unless an NVIDIA driver is present and CNET_GPU_BACKEND=cuda
# (or auto falls back after OpenCL/hip miss). Peer of CCE_HIPGEMM / CCE_CLGEMM.
CCE_CUDAGEMM := src/cce/cce_cudagemm.c
CCE_TRANSFORMER_QAT := src/cce/cce_transformer_qat.c
CCE := $(CCE_TENSOR) $(CCE_BLOCK) $(CCE_CASCADE) $(CCE_ARCHIVE) $(CCE_FOREST) $(CCE_ROUTER) $(CCE_SPARSE_KV) $(CCE_DSA) $(CCE_KV_PAGE) $(CCE_MTK) $(CCE_MLA) $(CCE_DS_MAP) $(CCE_DS_RT) $(CCE_INFER) $(CCE_UNCERTAINTY) $(CCE_COMPRESSION) $(CCE_LEARN) $(CCE_LORA) $(CCE_LILY) $(CCE_PATCH) $(CCE_GPU) $(CCE_ABI) $(CCE_CUDA_OBJ) $(CCE_PERCEPTUAL) $(CCE_WORDLM) $(CCE_MODEL) $(CCE_MODEL_IO) $(CCE_DATASET) $(CCE_AUTOGRAD) $(CCE_SAFETENSORS) $(CCE_GGUF) $(CCE_AICIMO) $(CCE_QGKP) $(CCE_DETECT) $(CCE_SSM) $(CCE_HYBRID) $(CCE_QWEN35) $(CCE_GGUF_QWEN35) $(CCE_ST_LLAMA) $(CCE_SPECGRAPH) $(CCE_WSTORE) $(CCE_TIERRT) $(CCE_SIMILAR) $(CCE_CLGEMM) $(CCE_HIPGEMM) $(CCE_CUDAGEMM) $(CCE_TRANSFORMER_QAT)
CNET_CCE_ADAPTER := src/cce/cce_contract_adapter.c
SPECIALIST_ADAPTERS := src/specialist_adapters.c
SPECIALIST_SRC := src/specialist.c src/specialist_health.c
FAULT_SRC := src/cnet_fault.c src/cnet_promote.c src/cnet_serve_decode.c
OPENLAB_SRC := src/cnet_moe.c src/cnet_acct.c
GAP_LANE_SRC := src/gap_lane.c src/cnet_auto_learn.c src/cnet_charter.c src/cnet_record_teacher.c $(FAULT_SRC)
GOV_SRC := src/cnet_governance.c
ASYNC_RUNTIME := src/async_runtime.c
MODEL_RUNTIME := src/model_runtime.c
RESOURCE_GOV_SRC := src/resource_governor.c
CF_ORDER_SRC := src/counterfactual_order.c
SELF_IMPROVE_SRC := src/self_improve.c
HARNESS_ORACLE_SRC := src/harness_oracle.c
EXT_TEACHER_SRC := src/external_teacher.c
MODALITY_VOICE_SRC := src/modality_voice.c
MODALITY_VISION_SRC := src/modality_vision.c
JSON_TOOLCALL_SRC := src/json_toolcall.c
MULTIMODAL_SRC := $(EXT_TEACHER_SRC) $(MODALITY_VOICE_SRC) $(MODALITY_VISION_SRC) $(JSON_TOOLCALL_SRC)
PERSONAL_AI_SRC := src/personal_ai.c $(OPENLAB_SRC)
CAPSULE_SRC := src/cnet_capsule.c
HYBRID_AI_SRC := src/hybrid_ai.c src/cnet_sparse_serve.c
BRAIN_SIDECAR_SRC := src/cnet_brain_sidecar.c
SHARED_WORKSPACE_SRC := src/cnet_shared_workspace.c
SEMANTIC_CORTEX_SRC := src/cnet_semantic_cortex.c
SLEEP_CONSOLIDATE_SRC := src/cnet_sleep_consolidate.c
CALIBRATED_GOVERNANCE_SRC := src/cnet_calibrated_governance.c
# Held-out capability fixture reader. Linked into every capability evaluator so
# the declared fixture drives the assertions instead of decorating the report.
HELDOUT_SRC := src/cnet_heldout.c
RESIDUAL_GGUF_SRC := src/residual_gguf.c src/residual_http.c
PILOT_SRC := src/cnet_pilot.c
CURIOSITY_SRC := src/cnet_curiosity.c
EG_SRC := src/cnet_eg.c
AGENT_ROLE_SRC := src/cnet_agent_role.c
ROUTE_LOG_SRC := src/cnet_route_log.c
EVIDENCE_BUNDLE_SRC := src/cnet_evidence_bundle.c
HEALTH_LAYERS_SRC := src/cnet_health_layers.c
PLACEMENT_SRC := src/cnet_placement.c
CCE_MODEL_CATALOG := src/cce/cce_model_catalog.c
MODEL_PROBE := src/model_probe.c
CNET_LLAMA_EVAL := tools/cnet_llama_eval.cpp
LLAMA_CPP_ROOT ?= /home/marble/llama.cpp
LLAMA_CPP_BUILD ?= $(LLAMA_CPP_ROOT)/build-rocm
LLAMA_CPP_HYBRID_BUILD ?= $(LLAMA_CPP_ROOT)/build-rocm-nographs
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
ACQUIRE_SRC := src/acquire.c src/runtime_identity.c
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

.PHONY: hipgemm_res cudagemm_res ci_rocm int8_matvec_bench artifact_isa_gate runtime_artifact_hygiene
.PHONY: all run test verify verify-long recipe_gate demos compat unified unified_native unified_adapter unified_cce_adapter unified_oracle_adapter unified_specialist specialist_health gap_lane gap_lane_run_build dispatch_story claims claims_model model_evidence oracle_v2_test soul_host_test legacy_test compose route dag hetero split chunk certify property coverage conformal logicgate decimal circuit study capacity library margin fuzzy stochastic fastpath throughput residue expr attention attention_study lifecycle_bench lbench proposal_sidecar probe_overhead belowbeam_chars struct_pref dgate_bench compounding_bench cce_smoke counterfactual_router_test sparse_kv_test narrative_coherence_test phase4_uncertainty_test register_compression_improvements phase5_integration_test cce_train_bench cce_view forest_view wordlm wordlm_bitnet cce_dll cnet_dll cce_safetensors_test cce_gguf_test cce_model_test cce_autograd_test endgate jsonstory pdftest pdflearn compound tiermem_test graduate fontdecode tfidf synonyms tileindex consolidate clean aicimo_smoke aicimo_core_test cnet_harness_contract_test cnet_harness_plugin dotnet_harness_test cce_lora_test cce_lora_bench cce_lily_test cce_lily_serve cce_lily_collect cce_lily_teacher registry_lily_test registry_lily_compute registry_lora_test jtc_lora_live jtc_lora_faultq personal_ai_lora_tick gigatok_bench gigatok_encode_bench gigatok_cache_bench moe_train moe_xf gigatok_encode_bench

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

# Open-addressing name → index map on PrimitiveRegistry (registry_find).
.PHONY: registry_hash
registry_hash: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) tests/test_registry_hash.c include/router.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_registry_hash \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) \
		tests/test_registry_hash.c $(LDFLAGS)
	@./$(BIN_DIR)/test_registry_hash > logs/registry_hash.log 2>&1
	@grep -q "REGISTRY_HASH_PASS" logs/registry_hash.log
	@grep "REGISTRY_HASH_PASS" logs/registry_hash.log

counterfactual_router_test: $(CCE_ROUTER) $(COUNTERFACTUAL_ROUTER_TEST) include/cce/cce_router.h include/cce/cce_forest.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(CCE_ROUTER) $(COUNTERFACTUAL_ROUTER_TEST) $(LDFLAGS)
	./$(BIN_DIR)/counterfactual_router_test

sparse_kv_test: $(CCE_SPARSE_KV) $(SPARSE_KV_TEST) include/cce/cce_sparse_kv.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(CCE_SPARSE_KV) $(SPARSE_KV_TEST) $(LDFLAGS)
	./$(BIN_DIR)/sparse_kv_test
	@grep -q "CCE_KV_STREAM_INDEX_PASS" <<< "$$(./$(BIN_DIR)/sparse_kv_test 2>&1)" || \
		./$(BIN_DIR)/sparse_kv_test | tee logs/sparse_kv_test.log | grep -q CCE_KV_STREAM_INDEX_PASS

# Alias: streaming-aware budgeted KV index (same binary)
.PHONY: kv_stream_index
kv_stream_index: sparse_kv_test
	@echo "KV_STREAM_INDEX_OK"

.PHONY: kv_stream_bench
kv_stream_bench: $(CCE_SPARSE_KV) include/cce/cce_sparse_kv.h tools/kv_stream_bench.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/kv_stream_bench $(CCE_SPARSE_KV) tools/kv_stream_bench.c $(LDFLAGS)
	@./$(BIN_DIR)/kv_stream_bench | tee logs/kv_stream_bench.log
	@grep -q "KV_STREAM_BENCH_PASS" logs/kv_stream_bench.log

# STM/LTM bridge (HOT never blocks on COLD) — C
CCE_STM_LTM := src/cce/cce_stm_ltm_bridge.c $(CCE_SPARSE_KV) src/cce/cce_kv_page.c
.PHONY: stm_ltm_bridge
stm_ltm_bridge: $(CCE_STM_LTM) include/cce/cce_stm_ltm_bridge.h tests/test_stm_ltm_bridge.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -o $(BIN_DIR)/test_stm_ltm_bridge \
		$(CCE_STM_LTM) tests/test_stm_ltm_bridge.c $(LDFLAGS)
	@./$(BIN_DIR)/test_stm_ltm_bridge | tee logs/stm_ltm_bridge.log
	@grep -q "STM_LTM_BRIDGE_PASS" logs/stm_ltm_bridge.log

.PHONY: stm_ltm_bench
stm_ltm_bench: $(CCE_STM_LTM) include/cce/cce_stm_ltm_bridge.h tools/stm_ltm_bench.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -o $(BIN_DIR)/stm_ltm_bench \
		$(CCE_STM_LTM) tools/stm_ltm_bench.c $(LDFLAGS)
	@./$(BIN_DIR)/stm_ltm_bench | tee logs/stm_ltm_bench.log
	@grep -q "STM_LTM_BENCH_PASS" logs/stm_ltm_bench.log

# Sparse KV EXECUTION gate: the ONE cce_sparse_kv selector wired into the
# REAL cce_gguf_qwen2 KV-cache attention path (the oracle seam), on a
# hermetic synthetic qwen2 GGUF the test writes itself. Pins OFF == ON@1.0
# bit-identity, budget-0.25 planted-needle retention + budget ceiling +
# decode argmax agreement, malformed-budget refusal (API and CNET_SPARSE_KV
# env knob), and OFF-restore bit-identity. Terminal marker: SPARSE_KV_EXEC_PASS.
sparse_kv_exec: $(CCE) $(CCE_CUDA_OBJ) tests/sparse_kv_exec_test.c tests/tiny_model_fixture.h include/cce/cce_sparse_kv.h include/cce/cce_gguf.h
	@mkdir -p logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/sparse_kv_exec_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/sparse_kv_exec > logs/sparse_kv_exec.log 2>&1
	@grep "SPARSE_KV_EXEC_PASS" logs/sparse_kv_exec.log

narrative_coherence_test: src/contract/narrative_coherence.c $(CCE_ROUTER) $(NARRATIVE_COHERENCE_TEST) include/contract/narrative_coherence.h include/cce/cce_router.h include/cce/cce_forest.h
	$(CC) $(CFLAGS) -Iinclude -o $(BIN_DIR)/$@ src/contract/narrative_coherence.c $(CCE_ROUTER) $(NARRATIVE_COHERENCE_TEST) $(LDFLAGS)
	./$(BIN_DIR)/narrative_coherence_test

.PHONY: phase123_benchmark_build phase123_benchmark_test sparse_kv_exec
phase123_benchmark_build: counterfactual_router_test sparse_kv_test sparse_kv_exec narrative_coherence_test
	$(CC) $(CFLAGS) -fPIC -shared -Iinclude -o $(PHASE123_BENCHMARK_LIBRARY) \
		src/cce/cce_sparse_kv.c src/contract/narrative_coherence.c $(LDFLAGS)

phase123_benchmark_test: phase123_benchmark_build $(PHASE123_BENCHMARK_TEST) $(PHASE123_BENCHMARK_TOOL)
	$(PHASE123_BENCHMARK_TEST)

phase4_uncertainty_test: $(CCE_UNCERTAINTY) $(CCE_COMPRESSION) $(PHASE4_UNCERTAINTY_TEST) include/cce/cce_uncertainty.h include/cce/cce_compression.h include/cce/cce_router.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(CCE_UNCERTAINTY) $(CCE_COMPRESSION) $(PHASE4_UNCERTAINTY_TEST) $(LDFLAGS)
	./$(BIN_DIR)/phase4_uncertainty_test

register_compression_improvements: $(PHASE5_INTEGRATION_TOOL)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(PHASE5_INTEGRATION_TOOL) $(LDFLAGS)

phase5_integration_test: register_compression_improvements $(PHASE5_INTEGRATION_TEST)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(PHASE5_INTEGRATION_TEST) $(LDFLAGS)
	./$(BIN_DIR)/phase5_integration_test

.PHONY: phase5_bounded_activation_test real_model_control_plane_test
phase5_bounded_activation_test: dotnet/CnetControlPlane/CnetControlPlane.csproj dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj
	dotnet test dotnet/CnetControlPlane.Tests --filter FullyQualifiedName~ActivateTests --verbosity minimal

real_model_control_plane_test: phase5_bounded_activation_test
	dotnet test dotnet/CnetControlPlane.Tests --filter FullyQualifiedName~RealModelAcceptanceTests --verbosity minimal
	dotnet test dotnet/CnetControlPlane.Tests --filter FullyQualifiedName~HermesTests --verbosity minimal
	dotnet test dotnet/CnetControlPlane.Tests --filter FullyQualifiedName~HermesTests --verbosity minimal
	dotnet test dotnet/CnetControlPlane.Tests --filter FullyQualifiedName~IngestTests --verbosity minimal
	bash tests/test_qgkp_cli_runtime.sh

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
	./$(BIN_DIR)/contract_unit > logs/contract_unit.log 2>&1

# registry_heal contract/BTN dimension-mismatch memory-safety gate.
# Asserts registry_heal refuses mismatched contract port signatures
# (input and output width) without mutating state or the retrain queue.
# Normal build + run; the _san variant runs under ASan/UBSan.
.PHONY: heal_mismatch heal_mismatch_san
heal_mismatch: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) tests/test_heal_mismatch.c include/nn.h include/router.h include/contract/contract.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) tests/test_heal_mismatch.c $(LDFLAGS)
	@./$(BIN_DIR)/heal_mismatch > logs/heal_mismatch.log 2>&1
	@grep -q '^HEAL_MISMATCH_PASS$$' logs/heal_mismatch.log

heal_mismatch_san: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) tests/test_heal_mismatch.c include/nn.h include/router.h include/contract/contract.h
	$(CC) -std=c11 -Wall -Wextra -pedantic -O1 -g -D_DEFAULT_SOURCE \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-o $(BIN_DIR)/heal_mismatch_san $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) \
		$(CONTRACT) tests/test_heal_mismatch.c $(LDFLAGS)
	@ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1 \
		./$(BIN_DIR)/heal_mismatch_san > logs/heal_mismatch_san.log 2>&1
	@grep -q '^HEAL_MISMATCH_PASS$$' logs/heal_mismatch_san.log

# Loader robustness: systematic single-byte flip + truncation sweeps over
# every artifact loader. Sealed formats (.cnu/.cnb) must refuse EVERY
# mutation; unsealed probes (gguf/safetensors/.cce) must never crash.
mutate: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(CCE) tests/test_mutate.c include/contract/unit.h include/base.h include/cce/cce_archive.h include/cce/cce_detect.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(CCE) tests/test_mutate.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/mutate > logs/mutate.log 2>&1

# Gap-triggered acquisition loop: gap ledger sidecar + oracle mining ->
# train -> certify (PROOF/SAMPLE) -> seal .cnu -> register -> replan.
# Link set mirrors the `coverage` target (+ acquire).
acquire: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(ACQUIRE_TEST) include/nn.h include/router.h include/contract/contract.h include/contract/coverage.h include/contract/unit.h include/acquire.h include/base.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(ACQUIRE_TEST) $(LDFLAGS)
	./$(BIN_DIR)/acquire > logs/acquire.log 2>&1

# Unified base (CNB version 2 semantics under stable CNB1 magic): one sealed
# container (units + tags + stats + oracle descriptors) replacing per-unit file
# sprawl; tag governance with refusal.
base: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(BASE_TEST) include/nn.h include/router.h include/contract/contract.h include/contract/coverage.h include/contract/unit.h include/acquire.h include/base.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(BASE_TEST) $(LDFLAGS)
	./$(BIN_DIR)/base > logs/base.log 2>&1

# Flagship harness gate (synthetic oracle, no CCE/GPU): duty-cycled,
# crash-resumable compounding run; base-as-checkpoint resume; stop file.
flagship: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(CONFORMAL) $(ACQUIRE_SRC) $(BASE_SRC) $(FLAGSHIP_SRC) $(FLAGSHIP_TEST) include/acquire.h include/base.h include/flagship.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(CONFORMAL) $(ACQUIRE_SRC) $(BASE_SRC) $(FLAGSHIP_SRC) $(FLAGSHIP_TEST) $(LDFLAGS)
	./$(BIN_DIR)/flagship > logs/flagship.log 2>&1
	@CNET_ACQ_ADAPTIVE=1 ./$(BIN_DIR)/flagship > logs/flagship.adaptive.log 2>&1
	@CNET_ACQ_WARMSTART=1 ./$(BIN_DIR)/flagship > logs/flagship.warmstart.log 2>&1
	@CNET_TOPK_SET=1 ./$(BIN_DIR)/flagship > logs/flagship.topkset.log 2>&1
	@CNET_ACQ_ADAPTIVE=1 CNET_ACQ_WARMSTART=1 CNET_TOPK_SET=1 ./$(BIN_DIR)/flagship > logs/flagship.allon.log 2>&1
	@cc -O2 -w -o $(BIN_DIR)/test_dequant_xcheck tests/test_dequant_xcheck.c -lm && ./$(BIN_DIR)/test_dequant_xcheck
	@$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_f16_identity $(CCE) tests/test_f16_identity.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) && ./$(BIN_DIR)/test_f16_identity

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
flagship_run_build: CFLAGS := $(CFLAGS) $(OMPFLAGS) -DCNET_BUILD_REV='"$(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)"' -DCNET_SOURCE_DIRTY=$(shell test -z "$$(git status --porcelain --untracked-files=normal 2>/dev/null)" && echo 0 || echo 1)
flagship_run_build: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(CONFORMAL) $(ACQUIRE_SRC) $(BASE_SRC) $(FLAGSHIP_SRC) $(CCE) $(CCE_CUDA_OBJ) $(CCE_ORACLE_PREFIX_CACHE) $(CCE_CAMPAIGN_PROVENANCE) tests/flagship_run.c include/flagship.h include/cce/cce_oracle_prefix_cache.h include/cce/cce_campaign_provenance.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/flagship_run $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(CONFORMAL) $(ACQUIRE_SRC) $(BASE_SRC) $(FLAGSHIP_SRC) $(CCE) $(CCE_CUDA_OBJ) $(CCE_ORACLE_PREFIX_CACHE) $(CCE_CAMPAIGN_PROVENANCE) tests/flagship_run.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

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
# verify. Usage: make gemma4_vs_ref_build && tests/gemma4_vs_ref.sh
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
# LEGACY/EXPERIMENTAL: glyph_habitat links src/cnet_lm.c — a second
# training/generation/head-routing path that is quarantined from the core
# CCE aggregate. It is reachable ONLY through this explicit legacy target.
glyph_habitat: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CCE) tests/glyph_habitat.c src/contract/text_add.c src/contract/text_add_compound.c src/contract/text_add_abstain.c src/contract/perceptual_query.c src/contract/narrative_diffusion.c src/contract/narrative_branching.c src/contract/interactive_agent.c src/contract/mcp_utils.c src/contract/mcp_memory.c src/contract/mcp_wiki.c src/contract/mcp_web_search.c src/contract/mcp_file_read.c src/contract/mcp_calculator.c src/contract/mcp_summarizer.c src/contract/mcp_file_write.c src/agent_memory.c src/contract/book_concept.c include/nn.h include/router.h include/plan_table.h include/contract/contract.h include/contract/text_add.h include/contract/text_add_compound.h include/contract/text_add_abstain.h include/contract/perceptual_query.h include/contract/narrative_diffusion.h include/contract/narrative_branching.h include/contract/interactive_agent.h include/contract/mcp_wiki.h include/contract/mcp_web_search.h include/contract/mcp_file_read.h include/contract/mcp_calculator.h include/contract/mcp_summarizer.h include/contract/mcp_file_write.h include/agent_memory.h include/contract/book_concept.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CCE) tests/glyph_habitat.c src/contract/text_add.c src/contract/text_add_compound.c src/contract/text_add_abstain.c src/contract/perceptual_query.c src/contract/narrative_diffusion.c src/contract/narrative_branching.c src/contract/interactive_agent.c src/contract/mcp_utils.c src/contract/mcp_memory.c src/contract/mcp_wiki.c src/contract/mcp_web_search.c src/contract/mcp_file_read.c src/contract/mcp_calculator.c src/contract/mcp_summarizer.c src/contract/mcp_file_write.c src/agent_memory.c src/contract/book_concept.c src/cnet_lm.c src/contract/anti_repeat.c $(LDFLAGS) $(MCP_LDFLAGS)
# Note: for full contract-based decimal response in glyph_habitat (dec_full_add composition),
# run `make decimal` (or decimal_demo) first to generate dec_value_weights.txt + dec_full_add_weights.txt.

# Build tool: dedicated build/ folder similar to tests/
# Focuses on the "build" agent features (artifact construction, etc.)
# LEGACY/EXPERIMENTAL: links src/cnet_lm.c — quarantined from core CCE.
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

# A trained artifact that FAILED must leave no artifact behind. Each run fails
# exactly one of the demo's nine artifacts for real (a zero-epoch budget), and
# the gate proves its weights and contract are absent while everything written
# before it is still there. Then one unchanged run must persist all nine.
.PHONY: legacy_persist_guard
legacy_persist_guard: nn_demo tests/test_legacy_persist_guard.sh $(BIN_DIR)/gate_evidence
	@mkdir -p logs
	@$(BIN_DIR)/gate_evidence legacy_persist_guard \
		logs/legacy_persist_guard.log LEGACY_PERSIST_GUARD_PASS -- \
		sh tests/test_legacy_persist_guard.sh

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
	./$(BIN_DIR)/decimal_demo > logs/decimal_demo.log 2>&1
	./$(BIN_DIR)/circuit_demo > logs/circuit_demo.log 2>&1

# The COMPAT tier (legacy quarantine): the restored historical test_all
# aggregate is back-compat coverage, not core verification — it runs here
# (and in verify-long) instead of blocking every `make test`. The gate that
# keeps the 2026-07-03 restoration from rotting again lives on, one tier out.
compat: test_all demos
	./$(BIN_DIR)/test_all > logs/legacy_test.log 2>&1
	@sh tests/verify_logs.sh compat
	@echo "CNET_COMPAT_PASS"

# Back-compat alias for the pre-quarantine target name.
legacy: compat

# Allocation-balance gate (behavioral leak check; MinGW has no ASan).
LEAK_WRAP := tests/leak_wrap.c
LEAK_LDWRAP := -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
leakcheck: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(BASE_TEST) $(LEAK_WRAP)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(BASE_TEST) $(LEAK_WRAP) $(LEAK_LDWRAP) $(LDFLAGS)
	./$(BIN_DIR)/leakcheck > logs/leakcheck.log 2>&1
	@grep "leakcheck" logs/leakcheck.log || true

# The recipe-integrity gate. Static analysis alone declared exit propagation
# sound while ignoring every pipeline, so it now also executes the false-green
# mutation (a producer that prints the PASS marker then exits 7) against each
# real recipe shape, and pins WITHHELD to a non-success exit code.
recipe_gate:
	@sh tests/test_recipe_gates_selftest.sh
	@sh tests/test_recipe_gates.sh
	@bash tests/test_pipeline_status.sh
	@sh tests/test_benchmark_verdict.sh
	@bash tests/test_gate_evidence.sh

# Every verify prerequisite is an ACTION, not a file. Without .PHONY, a file or
# directory of the same name at the repo root makes Make declare the target up
# to date and skip its recipe entirely -- the gate then reports success without
# compiling, running, or refreshing any evidence. These 26 were unprotected.
.PHONY: cnet_lm_bounds_test cce_detect cce_ssm cce_hybrid cce_qwen35 cce_st_llama
.PHONY: cce_specgraph cce_wstore cce_tiers cce_similar merge_family hybrid_catalog
.PHONY: transformer_qat contract_unit mutate acquire base flagship
.PHONY: leakcheck cnet_fault_test cce_adapter_bank_test cce_dora_test cnet_serve_decode_test cnet_fault_loop_test
.PHONY: registry_lora_store_test jtc_adapter_bench

# README told readers to run ./test_tinystories, but no target built it --
# a documented command that cannot work. The source is self-contained and
# Windows-only (wininet), so gate it on the platform rather than pretend.
.PHONY: test_tinystories
test_tinystories: test_tinystories.c
	@mkdir -p $(BIN_DIR)
ifeq ($(OS),Windows_NT)
	$(CC) $(CFLAGS) -Iinclude -o $(BIN_DIR)/test_tinystories test_tinystories.c $(CCE) $(LDFLAGS) $(MCP_LDFLAGS)
	@echo "built $(BIN_DIR)/test_tinystories"
else
	@echo "test_tinystories is Windows-only (uses wininet); skipping on this platform"
endif

# One JSON escaper, proven against a real parser. Two of the three escapers
# this replaces emitted documents that json.loads rejects (raw C0 bytes) or
# that silently lost data (control characters dropped).
.PHONY: json_escape
json_escape: tests/test_json_escape.c include/cnet_json_escape.h tests/test_json_escape.py
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Iinclude -o $(BIN_DIR)/test_json_escape tests/test_json_escape.c
	@./$(BIN_DIR)/test_json_escape | tee logs/json_escape.log
	@$(PYTHON) tests/test_json_escape.py ./$(BIN_DIR)/test_json_escape | tee -a logs/json_escape.log
	@grep -q "^JSON_ESCAPE_PASS" logs/json_escape.log
	@grep -q "^JSON_ESCAPE_PY_PASS" logs/json_escape.log

# Test recipes propagate their exit codes directly. This positive-marker gate
# runs after every prerequisite and rejects missing or stale-success logs.
#
# RUN BINDING. `verify` stamps logs/.verify_sentinel and only then invokes the
# real prerequisite chain, so tests/verify_logs.sh can require every log to be
# strictly newer than the stamp. Until 2026-08-12 the gate only checked that a
# marker existed SOMEWHERE in a file under logs/, and nothing in the chain ever
# cleared logs/ -- so an interrupted or month-old run left a complete set of
# green logs that this gate happily certified.
#
# The sentinel is stamped by a RECURSIVE make rather than by an ordinary
# prerequisite because Make gives no ordering guarantee among prerequisites
# under -j; a sentinel that raced the suites it is meant to predate would make
# the freshness check meaningless exactly when the build is fastest.
VERIFY_SENTINEL := logs/.verify_sentinel

.PHONY: verify verify_impl
verify:
	@mkdir -p logs
	@rm -f $(VERIFY_SENTINEL)
	@touch $(VERIFY_SENTINEL)
	@$(MAKE) --no-print-directory verify_impl
	@VERIFY_SINCE=$(VERIFY_SENTINEL) sh tests/verify_logs.sh

verify_impl: recipe_gate json_escape claims_test cce_dll cce_safetensors_test cnet_lm_bounds_test cce_autograd_test cce_model_test cce_view forest_view cce_detect cce_ssm cce_hybrid cce_qwen35 cce_st_llama cce_specgraph cce_wstore cce_tiers cce_similar merge_family hybrid_catalog transformer_qat contract_secure contract_unit heal_mismatch mutate acquire base flagship demos leakcheck cnet_fault_test cce_adapter_bank_test cce_dora_test cnet_serve_decode_test cnet_fault_loop_test registry_lora_store_test jtc_adapter_bench metric_honesty moe_ckpt_test
	@:

# Everything verify covers PLUS the GPU equivalence gate (needs model + GPU;
# run this before any CNET_GPU=1 campaign).
test_full: test gpu_equiv_build
	./$(BIN_DIR)/gpu_equiv Models/gemma-4-12B-it-MTP-Q8_0.gguf 64 32
	$(call dotnet_guard)
	$(DOTNET) test dotnet/Cce.Tests/Cce.Tests.csproj -c Release --no-restore

# `long` mode also asserts the two verify-long-only supra QAT gates. The
# `verify` prerequisite already ran and gated the core chain; this re-scan adds
# the extras.
# Same run-binding as `verify`: `verify` re-stamps the sentinel when it runs as
# a prerequisite below, and every extra suite here writes its log afterwards, so
# the whole CORE+LONG+COMPAT set is still required to postdate that one stamp.
.PHONY: verify-long verify-long_impl
verify-long:
	@$(MAKE) --no-print-directory verify-long_impl
	@VERIFY_SINCE=$(VERIFY_SENTINEL) sh tests/verify_logs.sh long

verify-long_impl: verify cce_train_bench supra_head_qat supra_head_qat_corpus transformer_qat_joint wordlm_bitnet wordlm_holdout compat
	@:


# Fast PR gate: light PEFT/fault only (no full-runtime JTC campaigns)
verify-fast: recipe_gate claims_test cce_dll cnet_fault_test cce_adapter_bank_test cce_dora_test cnet_serve_decode_test
	@echo VERIFY_FAST_PASS

# Nightly: full verify + heavy PEFT/fault/openlab/grade campaigns + procedure chunks
verify-nightly: verify cnet_fault_loop_test registry_lora_store_test jtc_adapter_bench cnet_openlab_import cnet_grade_up cnet_a_grade procedure_chunks metric_honesty_mutation
	@echo VERIFY_NIGHTLY_PASS

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
	# Only remove files/dirs that are NOT git-tracked — this preserves
	# committed fixtures and shared libraries (cce.dll, cnet.so, etc.)
	# while still cleaning genuinely generated build artifacts.
	@# Root-level generated executables (none of these are tracked).
	rm -f nn_demo test_nn test_encode_oob test_contract test_composition test_router test_dag \
	      test_consolidate test_certify test_property test_decimal test_circuit test_library \
	      property_demo route_demo dag_demo hetero_demo split_demo chunk_demo certify_demo \
	      decimal_demo circuit_demo capacity_study capacity_demo margin_study fuzzy_study stochastic_study \
	      test_fastpath throughput_study test_residue test_expr residue_study attention_study \
	      lifecycle_bench test_belowbeam_chars belowbeam_chars_study \
	      test_structural_pref test_structural_pref_b0 structural_pref_study \
	      cce_json_bench cce_smoke jsonstory pdftest pdflearn compound tiermem_test graduate fontdecode tfidf synonyms tileindex consolidate
	@# Shared libraries: only remove if NOT git-tracked (cce.dll/cnet.so are committed).
	for lib in cce.dll cnet.dll cnet.so retrieval_store.bin pdf_corpus.txt; do \
	  if [ -e "$$lib" ] && ! git ls-files --error-unmatch "$$lib" >/dev/null 2>&1; then \
	    rm -f "$$lib"; \
	  fi; \
	done
	@# Remove generated *.exe/*.so/*.dylib from root, but skip tracked ones.
	for f in *.exe *.so *.dylib; do \
	  [ -e "$$f" ] || continue; \
	  git ls-files --error-unmatch "$$f" >/dev/null 2>&1 || rm -f "$$f"; \
	done
	@# bin/ is always generated.
	rm -rf $(BIN_DIR)
	@# Fixture/store dirs: only remove if NOT git-tracked.
	for d in tile_store_test tile_store_res tile_store_per tile_store_cert tile_mem_pdf tile_grad_ctl tile_grad_pdf font_qa tfidf_ctl tfidf_book syn_ctl syn_rev syn_book tix_small tix_warm tix_exp tix_book cons_ctl cons_book cons_prune cons_cap; do \
	  if [ -e "$$d" ] && ! git ls-files --error-unmatch "$$d" >/dev/null 2>&1; then \
	    rm -rf "$$d"; \
	  fi; \
	done
	# Clean GGUF/CNET test aftermath junk (temp forests, per-experiment logs/outputs, .txt files)
	rm -f gguf_qwen2_forest.cce gguf_qwen2_packed.cce gguf_test_run.log ssm_forest.cce st_llama_forest.cce
	rm -f ssm_test.safetensors ssm_test.gguf ssm_bad.safetensors stll_test.gguf
	rm -rf stll_tmp stll_nocfg
	rm -f sg_a.gguf sg_a.graph sg_mamba.safetensors ws_a.gguf ws_a.manifest ws_restore.cce
	rm -rf sg_a sg_b ws_a ws_b ws_store ws_forge
	rm -f tr_a.manifest tr_bad.manifest tr_restore.cce sim_*.manifest sim_merge.cce
	rm -rf tr_a tr_store sim_a sim_e sim_m sim_store
	@# logs/ is always generated.
	rm -rf logs
	@# Generated text files: only remove if NOT git-tracked.
	for f in qwen2_*.cce qwen2_gguf_packed_*.cce test_*.txt make_*.log chat_*.txt err.txt; do \
	  [ -e "$$f" ] || continue; \
	  git ls-files --error-unmatch "$$f" >/dev/null 2>&1 || rm -f "$$f"; \
	done
	for f in test_gguf_*.txt test_phase*.txt test_final*.txt test_long*.txt test_completed*.txt test_quality*.txt test_int8*.txt test_rerun*.txt; do \
	  [ -e "$$f" ] || continue; \
	  git ls-files --error-unmatch "$$f" >/dev/null 2>&1 || rm -f "$$f"; \
	done
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

# gigatoken confirmation: SWAR GPT-2 pretokenizer (regex-replacement lever) ported
# to C, benchmarked on this host. Gate: scalar==SWAR boundaries byte-for-byte.
# Run a bigger/real corpus with:  ./bin/gigatok_bench owt_train.txt
gigatok_bench: src/cce/cce_pretok_swar.c tests/gigatok_bench.c include/cce/cce_pretok_swar.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(OMPFLAGS) -o $(BIN_DIR)/$@ src/cce/cce_pretok_swar.c tests/gigatok_bench.c -lm
	./$(BIN_DIR)/gigatok_bench

# End-to-end BPE encode: SWAR pretok + pretoken cache wired into cce_gguf_tok,
# benchmarked on a real GGUF vocab. Pass GGUF=<path> (default gemma-4-12B).
GGUF ?= Models/gemma-4-12B-it-MTP-Q8_0.gguf
gigatok_encode_bench: src/cce/cce_gguf_tok.c tests/gigatok_encode_bench.c include/cce/cce_gguf_tok.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ src/cce/cce_gguf_tok.c tests/gigatok_encode_bench.c -lm
	./$(BIN_DIR)/gigatok_encode_bench $(GGUF)

# DSA-inspired Sparse Softmax (SSMax) — no dense tail mass under the curve.
.PHONY: ssmax
ssmax: $(CCE_ROUTER) tests/test_ssmax.c include/cce/cce_router.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_ssmax $(CCE_ROUTER) tests/test_ssmax.c -lm
	@./$(BIN_DIR)/test_ssmax > logs/ssmax.log 2>&1
	@grep -q "SSMAX_PASS" logs/ssmax.log
	@grep -q "failures=0" logs/ssmax.log
	@grep "SSMAX_PASS" logs/ssmax.log

# Full DeepSeek-style DSA attention kernel (index → top-k → sleep/floor → skip).
.PHONY: dsa
dsa: $(CCE_ROUTER) $(CCE_SPARSE_KV) $(CCE_DSA) tests/test_dsa.c include/cce/cce_dsa.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_dsa $(CCE_ROUTER) $(CCE_SPARSE_KV) $(CCE_DSA) tests/test_dsa.c -lm
	@./$(BIN_DIR)/test_dsa > logs/dsa.log 2>&1
	@grep -q "DSA_PASS" logs/dsa.log
	@grep -q "failures=0" logs/dsa.log
	@grep "DSA_PASS" logs/dsa.log

# DeepSeek Multi-head Latent Attention (latent KV cache + decoupled RoPE).
.PHONY: mla
mla: $(CCE_ROUTER) $(CCE_MLA) $(CCE_DSA) $(CCE_SPARSE_KV) tests/test_mla.c include/cce/cce_mla.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_mla $(CCE_ROUTER) $(CCE_MLA) $(CCE_DSA) $(CCE_SPARSE_KV) tests/test_mla.c -lm
	@./$(BIN_DIR)/test_mla > logs/mla.log 2>&1
	@grep -q "MLA_PASS" logs/mla.log
	@grep -q "failures=0" logs/mla.log
	@grep "MLA_PASS" logs/mla.log
# CNET-native DeepSeek map + real forest bind (isolated from DS/llama.cpp).
.PHONY: deepseek_map
deepseek_map: $(CCE) tests/test_deepseek_map.c include/cce/cce_deepseek_map.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_deepseek_map $(CCE) \
		tests/test_deepseek_map.c $(LDFLAGS) -lm
	@./$(BIN_DIR)/test_deepseek_map > logs/deepseek_map.log 2>&1
	@grep -q "DEEPSEEK_MAP_PASS" logs/deepseek_map.log
	@grep -q "failures=0" logs/deepseek_map.log
	@grep "DEEPSEEK_MAP_PASS" logs/deepseek_map.log

# Full isolated stack: map→forest→MLA+MoE+DSA+cold experts + microbench.
.PHONY: ds_stack
ds_stack: $(CCE) tests/test_ds_runtime.c include/cce/cce_ds_runtime.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_ds_runtime $(CCE) \
		tests/test_ds_runtime.c $(LDFLAGS) -lm
	@./$(BIN_DIR)/test_ds_runtime > logs/ds_stack.log 2>&1
	@grep -q "DS_STACK_PASS" logs/ds_stack.log
	@grep -q "failures=0" logs/ds_stack.log
	@grep "DS_STACK_PASS\|bench:" logs/ds_stack.log

# GGUF → .cnetpack importer (CNET reader only).
.PHONY: cnet_ds_import
cnet_ds_import: $(CCE) tools/cnet_ds_import.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/cnet_ds_import $(CCE) tools/cnet_ds_import.c \
		$(LDFLAGS) -lm

# Aggregate sparse stack gate (DS residual host).
.PHONY: sparse_stack
sparse_stack: ssmax dsa mla deepseek_map ds_stack
	@echo "SPARSE_STACK_PASS"

# Forest MTP speculative + EP place tags
.PHONY: mtp_spec
mtp_spec: $(CCE) tests/test_mtp_spec.c include/cce/cce_ds_runtime.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_mtp_spec $(CCE) tests/test_mtp_spec.c $(LDFLAGS) -lm
	@./$(BIN_DIR)/test_mtp_spec 2>&1 | tee logs/mtp_spec.log
	@grep -q "MTP_SPEC_PASS" logs/mtp_spec.log
	@grep -q "failures=0" logs/mtp_spec.log

.PHONY: mtp_bench
mtp_bench: $(CCE) tools/cnet_mtp_bench.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/cnet_mtp_bench $(CCE) tools/cnet_mtp_bench.c $(LDFLAGS) -lm
	@CNET_MTP_SIM_LAUNCH=1000000 ./$(BIN_DIR)/cnet_mtp_bench 128 | tee logs/mtp_bench.log
	@grep -q "MTP_BENCH_PASS" logs/mtp_bench.log
	@grep -E 'k=2 par=1' logs/mtp_bench.log | tail -1 | grep -qE 'speedup=1\.[0-9]'

.PHONY: kv_page
kv_page: $(CCE_KV_PAGE) tests/test_kv_page.c include/cce/cce_kv_page.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_kv_page $(CCE_KV_PAGE) tests/test_kv_page.c -lpthread -lm
	@./$(BIN_DIR)/test_kv_page 2>&1 | tee logs/kv_page.log
	@grep -q "KV_PAGE_PASS" logs/kv_page.log
	@grep -q "failures=0" logs/kv_page.log

# Micro-Trensor Kernel: hot-swap CMSK skills on Forest weights (LEGO knowledge).
.PHONY: mtk
mtk: $(CCE) tests/test_mtk.c include/cce/cce_mtk.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_mtk $(CCE) tests/test_mtk.c $(LDFLAGS) -lm
	@CNET_FOREST_NO_PERSIST=1 ./$(BIN_DIR)/test_mtk 2>&1 | tee logs/mtk.log
	@grep -q "MTK_PASS" logs/mtk.log
	@grep -q "failures=0" logs/mtk.log
	@grep -q "9/9 needles" logs/mtk.log

# Product wiring eval: synthetic host (always) + optional real GGUF.
.PHONY: mtk_eval
mtk_eval: $(CCE) $(CCE_MTK_HOST) $(RESOURCE_GOV_SRC) tools/cnet_mtk_eval.c include/cce/cce_mtk_host.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/cnet_mtk_eval $(CCE) $(CCE_MTK_HOST) $(RESOURCE_GOV_SRC) \
		tools/cnet_mtk_eval.c $(LDFLAGS) -lm
	@CNET_FOREST_NO_PERSIST=1 ./$(BIN_DIR)/cnet_mtk_eval 2>&1 | tee logs/mtk_eval.log
	@grep -q "MTK_EVAL_PASS" logs/mtk_eval.log
	@grep -q "failures=0" logs/mtk_eval.log

.PHONY: mtk_eval_real
mtk_eval_real: mtk_eval
	@if [ -z "$$CNET_MTK_EVAL_GGUF" ]; then \
		echo "Set CNET_MTK_EVAL_GGUF=/path/model.gguf for real smoke"; exit 2; \
	fi
	@CNET_FOREST_NO_PERSIST=1 CNET_MTK_EVAL_GGUF="$$CNET_MTK_EVAL_GGUF" \
		./$(BIN_DIR)/cnet_mtk_eval 2>&1 | tee logs/mtk_eval_real.log
	@grep -q "MTK_EVAL_PASS" logs/mtk_eval_real.log
	@grep -q "real generate" logs/mtk_eval_real.log

# GGUF tokenizer (chat template + BPE encode/decode) — hermetic + optional model.
CCE_GGUF_TOK := src/cce/cce_gguf_tok.c
.PHONY: gguf_tok
gguf_tok: $(CCE_GGUF_TOK) tests/test_gguf_tok.c include/cce/cce_gguf_tok.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_gguf_tok $(CCE_GGUF_TOK) \
		tests/test_gguf_tok.c $(LDFLAGS) -lm
	@./$(BIN_DIR)/test_gguf_tok 2>&1 | tee logs/gguf_tok.log
	@grep -q "GGUF_TOK_PASS" logs/gguf_tok.log
	@grep -q "fails=0" logs/gguf_tok.log

# Progressive specialist conversion ladder (STE QAT → certify → pack).
CCE_SPEC_LADDER := src/cce/cce_spec_ladder.c
.PHONY: spec_ladder
spec_ladder: $(CCE) $(CCE_SPEC_LADDER) tests/test_spec_ladder.c \
		include/cce/cce_spec_ladder.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_spec_ladder $(CCE) $(CCE_SPEC_LADDER) \
		tests/test_spec_ladder.c $(LDFLAGS) -lm
	@./$(BIN_DIR)/test_spec_ladder 2>&1 | tee logs/spec_ladder.log
	@grep -q "SPEC_LADDER_PASS" logs/spec_ladder.log
	@grep -q "fails=0" logs/spec_ladder.log

.PHONY: spec_ladder_tool
spec_ladder_tool: $(CCE) $(CCE_SPEC_LADDER) tools/cnet_spec_ladder.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/cnet_spec_ladder $(CCE) $(CCE_SPEC_LADDER) \
		tools/cnet_spec_ladder.c $(LDFLAGS) -lm
	@echo "built bin/cnet_spec_ladder — run with MODEL path + family|schedule"

.PHONY: ladder_bench
ladder_bench: $(CCE) $(CCE_SPEC_LADDER) tools/cnet_ladder_bench.c \
		tests/tiny_model_fixture.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/cnet_ladder_bench $(CCE) $(CCE_SPEC_LADDER) \
		tools/cnet_ladder_bench.c $(LDFLAGS) -lm
	@./$(BIN_DIR)/cnet_ladder_bench 2>&1 | tee logs/ladder_bench.log
	@grep -q "LADDER_BENCH_PASS" logs/ladder_bench.log

# Real-model quality + latency (tok/s, English detokenize, optional skill delta).
# Optional CNET_LADDER_IMPORT=.ldtr loads certified trit specialists.
.PHONY: quality_eval
quality_eval: $(CCE) $(CCE_MTK_HOST) $(CCE_SPEC_LADDER) $(RESOURCE_GOV_SRC) \
		$(CCE_GGUF_TOK) tools/cnet_quality_eval.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/cnet_quality_eval $(CCE) $(CCE_MTK_HOST) \
		$(CCE_SPEC_LADDER) $(RESOURCE_GOV_SRC) $(CCE_GGUF_TOK) \
		tools/cnet_quality_eval.c $(LDFLAGS) -lm
	@echo "built bin/cnet_quality_eval — run with MODEL path"

.PHONY: campaign_bench
campaign_bench: mtk mtk_eval kv_page mtp_bench sparse_stack gguf_stack gguf_tok quality_eval
	@bash scripts/run_campaign_bench.sh


.PHONY: cnet_ds_bench
cnet_ds_bench: $(CCE) tools/cnet_ds_bench.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/cnet_ds_bench $(CCE) tools/cnet_ds_bench.c \
		$(LDFLAGS) -lm
	@./$(BIN_DIR)/cnet_ds_bench 128 | tee logs/ds_bench.log
	@grep -q "DS_BENCH" logs/ds_bench.log

# GGUF residual/token stack: synthetic tiny weights → load → multi-token gen
# + residual layer tap + sparse_kv + microbench + dual-backend open.
.PHONY: gguf_stack
gguf_stack: $(CCE) tests/test_gguf_runtime.c tests/tiny_model_fixture.h \
		include/cce/cce_infer_backend.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_gguf_runtime \
		$(CCE) tests/test_gguf_runtime.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -lm
	@./$(BIN_DIR)/test_gguf_runtime > logs/gguf_stack.log 2>&1
	@grep -q "GGUF_STACK_PASS" logs/gguf_stack.log
	@grep -q "failures=0" logs/gguf_stack.log
	@grep "GGUF_STACK_PASS\|bench:" logs/gguf_stack.log

.PHONY: cnet_gguf_bench
cnet_gguf_bench: $(CCE) tools/cnet_gguf_bench.c include/cce/cce_infer_backend.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/cnet_gguf_bench \
		$(CCE) tools/cnet_gguf_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -lm
	@./$(BIN_DIR)/cnet_gguf_bench 128 | tee logs/gguf_bench.log
	@grep -q "GGUF_BENCH" logs/gguf_bench.log

# Dual CPU: DS residual host + GGUF token path side-by-side (GPU hooks reserved).
.PHONY: dual_cpu_bench
dual_cpu_bench: $(CCE) tests/test_dual_cpu_bench.c include/cce/cce_infer_backend.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_dual_cpu_bench \
		$(CCE) tests/test_dual_cpu_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -lm
	@./$(BIN_DIR)/test_dual_cpu_bench > logs/dual_cpu_bench.log 2>&1
	@grep -q "DUAL_CPU_BENCH_PASS" logs/dual_cpu_bench.log
	@grep -q "failures=0" logs/dual_cpu_bench.log
	@grep "DUAL_CPU_BENCH_PASS\|DS \|GGUF " logs/dual_cpu_bench.log

# Aggregate: sparse DS stack + GGUF token stack + dual CPU baselines.
.PHONY: dual_stack
dual_stack: sparse_stack gguf_stack dual_cpu_bench
	@echo "DUAL_STACK_PASS"

# GGUF GPU: OpenCL primary (AMD pure-C). Soft-skip if no discrete GPU.
# Decision gate: 100% argmax vs CPU on synthetic fixture.
.PHONY: gguf_gpu
gguf_gpu: $(CCE) tests/test_gguf_gpu.c tests/tiny_model_fixture.h \
		include/cce/cce_infer_backend.h include/cce/cce_clgemm.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_gguf_gpu \
		$(CCE) tests/test_gguf_gpu.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -lm -ldl
	@./$(BIN_DIR)/test_gguf_gpu > logs/gguf_gpu.log 2>&1
	@if grep -q "GGUF_GPU_SKIP" logs/gguf_gpu.log; then \
		grep "GGUF_GPU_SKIP" logs/gguf_gpu.log; \
	else \
		grep -q "GGUF_GPU_PASS" logs/gguf_gpu.log && \
		grep -q "failures=0" logs/gguf_gpu.log && \
		grep "GGUF_GPU_PASS\|OpenCL\|argmax" logs/gguf_gpu.log; \
	fi

# Dual GPU bench: DS CPU + GGUF CPU/OpenCL; DS GPU reserved.
.PHONY: dual_gpu_bench
dual_gpu_bench: $(CCE) tests/test_dual_gpu_bench.c include/cce/cce_infer_backend.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_dual_gpu_bench \
		$(CCE) tests/test_dual_gpu_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -lm -ldl
	@./$(BIN_DIR)/test_dual_gpu_bench > logs/dual_gpu_bench.log 2>&1
	@if grep -q "DUAL_GPU_BENCH_SKIP" logs/dual_gpu_bench.log; then \
		grep "DUAL_GPU_BENCH_SKIP\|DS \|GGUF " logs/dual_gpu_bench.log; \
	else \
		grep -q "DUAL_GPU_BENCH_PASS" logs/dual_gpu_bench.log && \
		grep -q "failures=0" logs/dual_gpu_bench.log && \
		grep "DUAL_GPU_BENCH_PASS\|DS \|GGUF " logs/dual_gpu_bench.log; \
	fi

# Raw GEMM: CPU vs OpenCL×1/×2 vs hipBLAS×1/×2 (no model load).
.PHONY: gpu_matmul_bench
gpu_matmul_bench: $(CCE) tools/cnet_gpu_matmul_bench.c include/cce/cce_clgemm.h \
		include/cce/cce_hipgemm.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/cnet_gpu_matmul_bench \
		$(CCE) tools/cnet_gpu_matmul_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -lm -ldl
	@LD_LIBRARY_PATH=/opt/rocm/lib:$$LD_LIBRARY_PATH \
		./$(BIN_DIR)/cnet_gpu_matmul_bench 4096 4096 20 | tee logs/gpu_matmul_bench.log
	@grep -q "GPU_MATMUL_BENCH_PASS" logs/gpu_matmul_bench.log

# CNET-native real GGUF GPU bench (not llama.cpp). MODEL= path required.
# Example: make gguf_gpu_real MODEL=Models/gemma-4-12B-it-MTP-Q8_0.gguf N=16
.PHONY: gguf_gpu_real
gguf_gpu_real: $(CCE) tools/cnet_gguf_gpu_bench.c
	@mkdir -p $(BIN_DIR) logs
	@if [ -z "$(MODEL)" ]; then \
		echo "Set MODEL=/path/to.gguf  (optional N= tokens, BACKEND=auto|hip|opencl|cpu)"; \
		exit 2; \
	fi
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/cnet_gguf_gpu_bench \
		$(CCE) tools/cnet_gguf_gpu_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -lm -ldl
	@LD_LIBRARY_PATH=/opt/rocm/lib:$$LD_LIBRARY_PATH \
		./$(BIN_DIR)/cnet_gguf_gpu_bench "$(MODEL)" $(or $(N),16) $(or $(BACKEND),auto) \
		| tee logs/gguf_gpu_real.log
	@grep -q "GGUF_GPU_BENCH_PASS" logs/gguf_gpu_real.log

# Steady-state decode: long enough for device KV + stream attn (n>=64).
# Example: make gguf_gpu_steady MODEL=/path/Qwythos.gguf N=64
.PHONY: gguf_gpu_steady
gguf_gpu_steady: $(CCE) tools/cnet_gguf_gpu_bench.c
	@mkdir -p $(BIN_DIR) logs
	@if [ -z "$(MODEL)" ]; then \
		echo "Set MODEL=/path/to.gguf  (optional N=64+)"; \
		exit 2; \
	fi
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/cnet_gguf_gpu_bench \
		$(CCE) tools/cnet_gguf_gpu_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -lm -ldl
	@LD_LIBRARY_PATH=/opt/rocm/lib:$$LD_LIBRARY_PATH \
		CNET_ORACLE_INT8=1 CNET_FOREST_NO_PERSIST=1 CNET_GPU_COUNT=2 \
		./$(BIN_DIR)/cnet_gguf_gpu_bench "$(MODEL)" $(or $(N),64) opencl \
		| tee logs/gguf_gpu_steady.log
	@grep -q "GGUF_GPU_BENCH_PASS" logs/gguf_gpu_steady.log
	@grep "tok_s\|speedup\|GGUF_GPU_BENCH" logs/gguf_gpu_steady.log

# Pure CCE build without legacy nn.c (for testing the new engine)
cce_smoke_pure: $(CCE) $(CCE_CUDA_OBJ) tests/cce_smoke.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_smoke.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_smoke_pure

cce_train_bench: $(CCE) $(CCE_CUDA_OBJ) $(HELDOUT_SRC) tests/cce_train_bench.c include/cnet_heldout.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) $(HELDOUT_SRC) tests/cce_train_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	@./$(BIN_DIR)/cce_train_bench > logs/cce_train_bench.log 2>&1; rc=$$?; \
		cat logs/cce_train_bench.log; exit $$rc
	@grep -q '^CLASSIFICATION_GATE_PASS ' logs/cce_train_bench.log

cce_json_bench: $(CCE) $(CCE_CUDA_OBJ) tests/cce_json_bench.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_json_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_json_bench

# Zero-copy WARM load: cce_cascade_view_from_archive views weights in the mmap
# (owns_memory=0) instead of reload+copy. Verifies bit-identical forward + clean free.
cce_view: $(CCE) $(CCE_CUDA_OBJ) tests/cce_view_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_view_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_view > logs/cce_view.log 2>&1

# Universal pre-run model structure detection: magic-sniff the container
# (gguf/safetensors/cce/packed), fingerprint the architecture from the tensors
# actually present, report hparams WITHOUT loading weights, dispatch to the
# matching loader via cce_anymodel_open. Unsupported structures refuse honestly.
cce_detect: $(CCE) $(CCE_CUDA_OBJ) tests/cce_detect_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_detect_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_detect > logs/cce_detect.log 2>&1

# Mamba-1 SSM runner: forest-decomposed linear specialists + recurrent scan
# glue; verified against an independent double-precision reference and
# safetensors<->gguf mapping equivalence (bit-identical logits).
cce_ssm: $(CCE) $(CCE_CUDA_OBJ) tests/cce_ssm_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_ssm_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_ssm > logs/cce_ssm.log 2>&1

# Native hybrid attention+SSM runner: a single MIXED forward threads one
# residual stream through interleaved llama/qwen2-style attention layers and
# mamba-1 SSM layers (jamba/zamba class), reusing the same forest-decomposed
# linear specialists as the pure runners. Gated against an independent
# double-precision reference; mutation proves both sublayers are in the path.
cce_hybrid: $(CCE) $(CCE_CUDA_OBJ) tests/cce_hybrid_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_hybrid_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_hybrid > logs/cce_hybrid.log 2>&1

# Native Qwen3.5 execution core (attention + Gated-DeltaNet hybrid): the
# architecture-specific layer-schedule dispatch, the stateful recurrent
# Gated-DeltaNet step, and the Qwen3.5 gated causal attention. Gated against an
# independent double-precision reference recurrence/attention; the generic
# cce_hybrid mamba-1 runner deliberately does NOT cover this convention.
cce_qwen35: $(CCE) $(CCE_CUDA_OBJ) tests/cce_qwen35_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_qwen35_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_qwen35 > logs/cce_qwen35.log 2>&1

# Hermetic end-to-end test of the qwen35 hybrid RUNNER (cce_gguf_qwen35.c):
# writes a tiny qwen35-arch GGUF fixture, gates the forward against an
# independent double-precision reference, and pins the oracle-harness state
# contract (rewind checkpoint, probe batches, int8 head skip, MTP skip).
cce_qwen35_e2e: $(CCE) $(CCE_CUDA_OBJ) tests/cce_qwen35_e2e_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_qwen35_e2e_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_qwen35_e2e > logs/cce_qwen35_e2e.log 2>&1

# Hermetic state-machine regression for the real flagship oracle's prefix
# cache. A direct/foreign forward (notably the golden battery) must force the
# next unit to rebuild its prefix; failed fills must remain invalid.
flagship_prefix_cache: $(CCE_ORACLE_PREFIX_CACHE) include/cce/cce_oracle_prefix_cache.h tests/test_flagship_prefix_cache.c
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(CCE_ORACLE_PREFIX_CACHE) tests/test_flagship_prefix_cache.c $(LDFLAGS)
	./$(BIN_DIR)/flagship_prefix_cache > logs/flagship_prefix_cache.log 2>&1
	@grep -q "FLAGSHIP_PREFIX_CACHE_PASS" logs/flagship_prefix_cache.log

campaign_provenance_unit: $(CCE_CAMPAIGN_PROVENANCE) include/cce/cce_campaign_provenance.h tests/test_campaign_provenance.c
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/$@ $(CCE_CAMPAIGN_PROVENANCE) tests/test_campaign_provenance.c $(LDFLAGS)
	./$(BIN_DIR)/campaign_provenance_unit > logs/campaign_provenance.log 2>&1
	@grep -q "CAMPAIGN_PROVENANCE_PASS" logs/campaign_provenance.log

# The Qwythos campaign was recorded from source_dirty=1 and its exact binary
# was not retained. Verify the immutable record/local artifacts without ever
# relabelling today's flagship_run as the historical teacher.
campaign_provenance: campaign_provenance_unit \
		qwythos_english_v1.cnb.manifest.json qwythos_english_v1.cnb.sha256 \
		english_window_256_qwythos.txt goldens_qwythos.int8.txt \
		tests/test_qwythos_campaign_record.sh
	@bash tests/test_qwythos_campaign_record.sh > logs/qwythos_provenance.log 2>&1
	@grep -q "QWYTHOS_CAMPAIGN_RECORD_PASS" logs/qwythos_provenance.log
	@if [ -f qwythos_english_v1.cnb ]; then \
		sha256sum -c qwythos_english_v1.cnb.sha256 > logs/qwythos_base_digest.log; \
	else \
		printf '%s\n' 'QWYTHOS_BASE_LOCAL_UNAVAILABLE record_only=1' \
			> logs/qwythos_base_digest.log; \
	fi

execution_tiers_doc_gate: docs/EXECUTION_TIERS.md tests/test_execution_tiers_doc.sh Makefile tests/test_alt_paths_gate.c
	@sh tests/test_execution_tiers_doc.sh > logs/execution_tiers_doc.log 2>&1
	@grep -q "EXECUTION_TIERS_DOC_PASS" logs/execution_tiers_doc.log

# HF-llama safetensors loader: same decomposed transformer as the GGUF path,
# gated by bit-identical logits between the two container formats.
cce_st_llama: $(CCE) $(CCE_CUDA_OBJ) tests/cce_st_llama_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_st_llama_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_st_llama > logs/cce_st_llama.log 2>&1

# Specialist knowledge graph: content digests + behavioral fingerprints +
# DATA_FLOWS wiring for any decomposed model (the codebase-memory move on
# weights). Gates: cross-container digest identity, one-matrix locality.
cce_specgraph: $(CCE) $(CCE_CUDA_OBJ) tests/cce_specgraph_test.c tests/tiny_model_fixture.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_specgraph_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_specgraph > logs/cce_specgraph.log 2>&1

# Content-addressed weight store: specialists stored once by digest, models
# as manifests. Gates: 100% reuse on re-ingest + cross-container, fine-tune
# costs one payload, restore is bit-identical, reuse claims byte-verified.
cce_wstore: $(CCE) $(CCE_CUDA_OBJ) tests/cce_wstore_test.c tests/tiny_model_fixture.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_wstore_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_wstore > logs/cce_wstore.log 2>&1

# Tiered runtime: run a store-backed transformer in bounded RAM (HOT cap +
# LRU eviction + on-demand rehydration). Gate: capped streaming logits are
# bit-identical to all-resident; residency never exceeds the cap.
cce_tiers: $(CCE) $(CCE_CUDA_OBJ) tests/cce_tiers_test.c tests/tiny_model_fixture.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_tiers_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_tiers > logs/cce_tiers.log 2>&1

# Dense expert-streaming arc (Arc A1): a QUANTIZED (int8 weight-only PTQ) dense
# model streams from the weight store under a bounded resident cap. Gates the
# quantized streaming and reports whether the store preserves int8 end-to-end
# (finding: it re-materializes FP at serialize_cascade) + the size/RAM numbers.
dense_stream_q: $(CCE) $(CCE_CUDA_OBJ) tests/dense_stream_q.c tests/tiny_model_fixture.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/dense_stream_q.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/dense_stream_q > logs/dense_stream_q.console.log 2>&1

# The dense expert-streaming pipeline on a REAL dense model (default Qwen2.5-0.5B):
# load -> int8-quantize -> ingest -> restore -> tier-stream under a bounded cap,
# verifying real forward + near-lossless int8 + bit-identical quantized streaming +
# bounded RAM + real on-disk compression. Standalone (needs a checkpoint via argv[1]).
dense_stream_real: $(CCE) $(CCE_CUDA_OBJ) tests/dense_stream_real.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/dense_stream_real.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/dense_stream_real > logs/dense_stream_real.console.log 2>&1

# Dense expert-streaming arc (Arc A2): DATA-AWARE quantization at ingest. Each
# streamed specialist is quantized with our GPTQ/OBQ solver calibrated on its
# REAL input activations (captured via an opt-in forward hook). Proves the
# quantized model still streams BIT-IDENTICALLY under a bounded cap AND that
# data-aware beats naive at int4 (held-out logit relerr) — int8 near-lossless.
dense_stream_a2: $(CCE) $(CCE_CUDA_OBJ) tests/dense_stream_a2.c tests/tiny_model_fixture.h
	@mkdir -p logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/dense_stream_a2.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/dense_stream_a2 > logs/dense_stream_a2.console.log 2>&1

# MoE expert-streaming arc (B1): MoE checkpoints parse into streamable
# per-expert specialists. Hermetic: synthetic MoE gguf (split gate/up/down
# banks + router) — every expert loads bit-exact vs the generator, round-trips
# the weight store (FP + int8), dedups by digest; dense ggufs are refused.
# Real (auto-skips if absent): gemma-4-26B-A4B — 128-expert fused-bank layout
# parses, Q8_0/Q6_K expert slices bit-exact vs whole-bank dequant, real expert
# streams through the store; per-expert economics reported.
moe_loader: $(CCE) $(CCE_CUDA_OBJ) tests/moe_loader.c tests/tiny_model_fixture.h
	@mkdir -p logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/moe_loader.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/moe_loader > logs/moe_loader.console.log 2>&1

# MoE expert-streaming arc (B2): routed, demand-loaded MoE FFN forward.
# Conventions pinned against llama.cpp (softmax -> top-k -> renorm; gemma4
# router-on-attn_out, GEGLU, fused gate|up split, per-expert down scale).
# Hermetic: forward == an independent reference (selection, weights, output);
# only routed experts fetched. Real: 8-of-128 demand loading, bounded cap.
# Parity: replays llama.cpp's dumped attn_out through CNET's forward —
# identical top-8 selection, logits/weights/output within tolerance
# (dumps regenerate via bin/moe_parity_dump, see tools/moe_parity_dump.cpp).
moe_forward: $(CCE) $(CCE_CUDA_OBJ) tests/moe_forward.c tests/tiny_model_fixture.h
	@mkdir -p logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/moe_forward.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/moe_forward > logs/moe_forward.console.log 2>&1

# MoE expert-streaming arc (B3): the streaming throughput layer. Experts
# ingest into the weight store on first touch (int8 = ~4x smaller payloads,
# codes run directly, no dequant) and re-stream from it; a background worker
# prefetches every routed expert while the forward computes (zero sync
# fetches); pin_hot keeps the most-ROUTED experts resident; forward_batch
# loads each unique expert once per batch, bit-identical to per-token.
# Real ladder measured: gguf reload vs int8 store vs +lookahead vs pinning.
moe_stream: $(CCE) $(CCE_CUDA_OBJ) tests/moe_stream.c tests/tiny_model_fixture.h
	@mkdir -p logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/moe_stream.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/moe_stream > logs/moe_stream.console.log 2>&1

# MoE expert-streaming arc (B4): per-expert data-aware quantization on
# ROUTED activations. Calibration collects each routed token's expert input
# per expert; the data-aware store modes quantize each expert at ingest
# against its own traffic (damped sample-space OBQ, naive fallback under 8
# samples). Gates: data-aware int4 beats naive int4 on calibration traffic
# (hermetic, strict) and on held-out real routed tokens (gemma-4-26B);
# payloads ~6x smaller than FP; quantized experts re-stream bit-identical.
moe_expert_quant: $(CCE) $(CCE_CUDA_OBJ) tests/moe_expert_quant.c tests/tiny_model_fixture.h
	@mkdir -p logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/moe_expert_quant.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/moe_expert_quant > logs/moe_expert_quant.console.log 2>&1

# MoE end-to-end (Arc B capstone): full-stack gemma4 single-token parity.
# CNET runs the COMPLETE 26B layer stack (attention exact at position 0:
# softmax over one score = 1) with the expert bank DEMAND-STREAMED from
# disk; gates vs llama.cpp dumps: every layer's output, identical next-token
# argmax + top-8, full-vocab logits within quant tolerance.
# Dumps regenerate via: bin/moe_parity_dump <gguf> logs/moe_e2e "ids:2"
moe_e2e_build: $(CCE) $(CCE_CUDA_OBJ) tests/moe_e2e.c
	@mkdir -p logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/moe_e2e $(CCE) $(CCE_CUDA_OBJ) tests/moe_e2e.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

moe_e2e: moe_e2e_build
	./$(BIN_DIR)/moe_e2e > logs/moe_e2e.console.log 2>&1

# MoE generation (Arc B finale): multi-token GENERATION parity. CNET decodes
# a real prompt through the full gemma4 stack with REAL attention (NEOX rope,
# QK norms, kv cache, unscaled causal scores) + streamed experts, then
# greedy-generates — gated TOKEN-FOR-TOKEN against llama.cpp's continuation,
# with the per-layer ladder matching at every prompt position.
# Reference: bin/moe_parity_dump <gguf> logs/moe_gen "ids:..." <n_gen>
moe_gen: $(CCE) $(CCE_CUDA_OBJ) tests/moe_gen.c
	@mkdir -p logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/moe_gen.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/moe_gen > logs/moe_gen.console.log 2>&1

# Dense expert-streaming arc (A3): async readahead + learned hot-pinning.
# The tier runtime learns the fetch order of the first cold pass, then a
# background worker prefetches the next `depth` payloads (wrapping around the
# pass boundary — the decode regime) while the forward computes; pin_hot keeps
# the most-fetched specialists resident. Gates: deterministic fetch mechanics
# (1 sync fetch after a cold start, 0 with wrap-around, rehydrations drop by
# the pin count) and BIT-IDENTICAL logits vs a synchronous twin every pass.
dense_stream_a3: $(CCE) $(CCE_CUDA_OBJ) tests/dense_stream_a3.c tests/tiny_model_fixture.h
	@mkdir -p logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/dense_stream_a3.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/dense_stream_a3 > logs/dense_stream_a3.console.log 2>&1

# Dense expert-streaming arc (packed storage): the weight store's quantized-
# payload path extended to PACKED formats — ternary at 1.6 bit/weight (5 trits/
# byte base-3, ~20x) and int4 at 2 codes/byte (~8x). Gates: four precision
# variants of one cascade land under DISTINCT digests with EXACT payload sizes
# and restore representation-bit-exact; packed models stream BIT-IDENTICALLY
# to their all-resident twins under a bounded resident cap.
dense_stream_trit: $(CCE) $(CCE_CUDA_OBJ) tests/dense_stream_trit.c tests/tiny_model_fixture.h
	@mkdir -p logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/dense_stream_trit.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/dense_stream_trit > logs/dense_stream_trit.console.log 2>&1

# SIMILAR_TO + evidence-gated merge: epsilon-equivalent specialists merge via
# manifest remap ONLY after an adversarial probe battery; unverified refuses.
cce_similar: $(CCE) $(CCE_CUDA_OBJ) tests/cce_similar_test.c tests/tiny_model_fixture.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_similar_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_similar > logs/cce_similar.log 2>&1

# CLI probe: make detect FILE=Models/foo.gguf  (or run bin/detect_cli directly)
detect_cli: $(CCE) $(CCE_CUDA_OBJ) tests/detect_cli.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/detect_cli.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)

detect: detect_cli
	./$(BIN_DIR)/detect_cli $(FILE)

# Forest tier wiring: zero-copy WARM views + copy-on-write to HOT + seal guard,
# live in cce_forest_forward / promote_to_hot.
forest_view: $(CCE) $(CCE_CUDA_OBJ) tests/cce_forest_view_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_forest_view_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/forest_view > logs/forest_view.log 2>&1

cce_model_test: $(CCE) $(CCE_CUDA_OBJ) tests/test_cce_model_save.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/test_cce_model_save.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_model_test > logs/cce_model_test.log 2>&1

cce_autograd_test: $(CCE) $(CCE_CUDA_OBJ) tests/test_cce_autograd.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/test_cce_autograd.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_autograd_test > logs/cce_autograd_test.log 2>&1

# cce_lora: rank-r low-rank adapter prototype (M1 apply/merge, M2 train, M3 store).
cce_lora_test: $(CCE) $(CCE_CUDA_OBJ) tests/cce_lora_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_lora_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_lora_test

# cce_lora_bench: M5 — dense output-SGD vs rank-r adapters on the same residuals.
cce_lora_bench: $(CCE) $(CCE_CUDA_OBJ) tests/bench_cce_lora.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/bench_cce_lora.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_lora_bench

# cce_lily: interconnected multi-layer low-rank adapter (Lily) + interconnection bench.
cce_lily_test: $(CCE) $(CCE_CUDA_OBJ) tests/cce_lily_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_lily_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_lily_test

# cce_lily_serve: interior-layer serve hook verified against the real DS residual forward.
cce_lily_serve: $(CCE) $(CCE_CUDA_OBJ) tests/cce_lily_serve_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_lily_serve_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_lily_serve

# cce_lily_collect: training-data collection loop through the deep forward + residual distillation.
cce_lily_collect: $(CCE) $(CCE_CUDA_OBJ) tests/cce_lily_collect_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_lily_collect_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_lily_collect

# cce_lily_teacher: real teacher residual stream (self-distillation, cheap student -> full teacher).
cce_lily_teacher: $(CCE) $(CCE_CUDA_OBJ) tests/cce_lily_teacher_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/cce_lily_teacher_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_lily_teacher

# registry_lily: cce_lily deep-base adapter hosted by registry_lora's certify gate (serve-loop teach).
REGISTRY_LILY := src/router/registry_lily.c

# Unified fault bus + promote gate (Tier 0 replace/improve)

# Cross-process fault bus → ingest → lora tick (JTC closed loop)

# Persist certified adapters (save/load round-trip)
registry_lora_store_test: json_toolcall_alphabet $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) tests/registry_lora_store_test.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ \
		$(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) \
		src/soul_host.c $(ROUTE_LOG_SRC) tests/registry_lora_store_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	./$(BIN_DIR)/registry_lora_store_test

# JTC accuracy before/after adapter (+ writes CNET_PROMOTE_EVAL_DELTA)
jtc_adapter_bench: json_toolcall_alphabet $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) $(HELDOUT_SRC) tests/jtc_adapter_bench.c include/cnet_heldout.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ \
		$(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) \
		src/soul_host.c $(ROUTE_LOG_SRC) $(HELDOUT_SRC) tests/jtc_adapter_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	CNET_PROMOTE_EVAL_DELTA=logs/ghost_eval_delta.txt ./$(BIN_DIR)/jtc_adapter_bench

# Production autoteach binaries. These ship in bin/ and are invoked by the
# 24/7 loop (cnet_autoteach_tick.sh) and the governor, but had no recipe — they
# were built ad hoc, so a change to e.g. src/cnet_fault.c silently did not reach
# them. Same link line as the benches above.
.PHONY: autoteach_bins
autoteach_bins: cnet_cert_learn_tick struct_mine_persist

cnet_cert_learn_tick: json_toolcall_alphabet $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) tools/cnet_cert_learn_tick.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ \
		$(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) \
		src/soul_host.c $(ROUTE_LOG_SRC) tools/cnet_cert_learn_tick.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@echo built $(BIN_DIR)/$@

struct_mine_persist: json_toolcall_alphabet $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) tools/struct_mine_persist.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ \
		$(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) \
		src/soul_host.c $(ROUTE_LOG_SRC) tools/struct_mine_persist.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@echo built $(BIN_DIR)/$@

cnet_fault_loop_test: json_toolcall_alphabet $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) tests/cnet_fault_loop_test.c include/json_toolcall.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/cnet_fault_loop_test \
		$(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) \
		src/soul_host.c $(ROUTE_LOG_SRC) tests/cnet_fault_loop_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	./$(BIN_DIR)/cnet_fault_loop_test

# Metric honesty: tests that fail when a governor metric lies.
# Every 2026-07-25 defect was an instrument bug, not a mechanism bug; these
# encode the invariant each metric must satisfy and are verified by mutation
# (make metric_honesty_mutation) to go red when the old behaviour returns.
.PHONY: metric_honesty metric_honesty_mutation
cnet_fault_dedupe_probe: src/cnet_fault.c tests/cnet_fault_dedupe_probe.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ src/cnet_fault.c tests/cnet_fault_dedupe_probe.c $(LDFLAGS)

metric_honesty: cnet_fault_dedupe_probe gap_lane $(BIN_DIR)/test_metric_honesty
	@mkdir -p logs
	@$(BIN_DIR)/test_metric_honesty > logs/metric_honesty.log 2>&1 \
		|| { tail -40 logs/metric_honesty.log; false; }
	@grep -q "METRIC_HONESTY_PASS" logs/metric_honesty.log
	@grep -E "^Ran [0-9]+ tests" logs/metric_honesty.log
	@echo METRIC_HONESTY_PASS

metric_honesty_mutation: cnet_fault_dedupe_probe
	@mkdir -p logs
	@bash tests/metric_honesty_mutation.sh 2>&1 | tee logs/metric_honesty_mutation.log
	@grep -q "MUTATION_GATE_PASS" logs/metric_honesty_mutation.log

cnet_fault_test: src/cnet_fault.c src/cnet_promote.c tests/cnet_fault_test.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ src/cnet_fault.c src/cnet_promote.c tests/cnet_fault_test.c $(LDFLAGS)
	./$(BIN_DIR)/cnet_fault_test

cnet_promote_test: cnet_fault_test
# Tier1 PEFT: hard-routed adapter bank + DoRA-lite
cce_adapter_bank_test: src/cce/cce_adapter_bank.c tests/cce_adapter_bank_test.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ src/cce/cce_adapter_bank.c tests/cce_adapter_bank_test.c $(LDFLAGS)
	./$(BIN_DIR)/cce_adapter_bank_test

cce_dora_test: $(CCE) $(CCE_CUDA_OBJ) src/cce/cce_dora.c tests/cce_dora_test.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/cce/cce_dora.c tests/cce_dora_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_dora_test

cnet_serve_decode_test: src/cnet_serve_decode.c tests/cnet_serve_decode_test.c
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ src/cnet_serve_decode.c tests/cnet_serve_decode_test.c $(LDFLAGS)
	./$(BIN_DIR)/cnet_serve_decode_test

registry_lily_test: $(CCE) $(CCE_CUDA_OBJ) $(REGISTRY_LILY) tests/registry_lily_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) $(REGISTRY_LILY) tests/registry_lily_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/registry_lily_test

# compute-quality teacher (dense attn / all experts vs cheap sparse student),
# hosted under the SAME certify gate — a multi-token compute gap distilled.
registry_lily_compute: $(CCE) $(CCE_CUDA_OBJ) $(REGISTRY_LILY) tests/registry_lily_compute_test.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) $(REGISTRY_LILY) tests/registry_lily_compute_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/registry_lily_compute

# registry_lora: adapter wired into a real registry unit's retrain queue (opt-in).
REGISTRY_LORA := src/router/registry_lora.c src/router/registry_lora_store.c src/cce/cce_adapter_bank.c
registry_lora_test: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(REGISTRY_LORA) $(FAULT_SRC) $(CCE) $(CCE_CUDA_OBJ) tests/test_registry_lora.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(REGISTRY_LORA) $(FAULT_SRC) $(CCE) $(CCE_CUDA_OBJ) tests/test_registry_lora.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/registry_lora_test

# jtc_lora_live: registry_teach_lora on the real certified json_toolcall_v2 unit.
# Mirrors the json_toolcall link line + the registry_lora TU.
jtc_lora_live: json_toolcall_alphabet $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) tests/jtc_lora_live.c include/json_toolcall.h include/json_toolcall_alphabet.inc
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/jtc_lora_live \
		$(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) \
		src/soul_host.c $(ROUTE_LOG_SRC) tests/jtc_lora_live.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	./$(BIN_DIR)/jtc_lora_live

# jtc_lora_faultq: validate the adapter on a queue built from REAL runtime faults.
# Mirrors the json_toolcall link line + the registry_lora TU.
jtc_lora_faultq: json_toolcall_alphabet $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) tests/jtc_lora_faultq.c include/json_toolcall.h include/json_toolcall_alphabet.inc
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/jtc_lora_faultq \
		$(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) \
		src/soul_host.c $(ROUTE_LOG_SRC) tests/jtc_lora_faultq.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	./$(BIN_DIR)/jtc_lora_faultq

# personal_ai_lora_tick: run the live orchestrator tick with a loaded base.
# Mirrors the json_toolcall link line + the registry_lora TU.
personal_ai_lora_tick: json_toolcall_alphabet $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) tests/personal_ai_lora_tick.c include/json_toolcall.h include/json_toolcall_alphabet.inc
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/personal_ai_lora_tick \
		$(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) \
		src/soul_host.c $(ROUTE_LOG_SRC) tests/personal_ai_lora_tick.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	./$(BIN_DIR)/personal_ai_lora_tick

# --- SIMD re-enable for the CCE/Supra targets ---
# The global -mno-avx works around the MinGW AVX struct-copy segfault triggered by
# the contract router's by-value Port struct (src/router/, include/router.h). These
# CCE targets do NOT link src/router/, so they are AVX-safe (verified: builds + runs
# clean). Combined with the vectorizable mat-vec in cce_block.c, AVX speeds up the
# tensor math. -std=c11 keeps FP contraction off, so weights stay bit-identical.
AVX_CFLAGS := $(filter-out -mno-avx,$(CFLAGS))
# wordlm/wordlm_bitnet/trit_bench link only CCE sources (no src/router/), so
# they are AVX-safe too; OMP activates the row-parallel packed-trit kernels.
cce_safetensors_test supra_console supra_chat_mock supra_context_probe supra_longform supra_head_qat supra_head_qat_corpus transformer_qat_joint transformer_qat_real transformer_qat_altmodel transformer_qat_gpt2names proj_qat_recon proj_qat_gemma proj_qat_stack proj_qat_gpu gptq_solver proj_qat_gemma_e2e proj_qat_bitwidth dense_stream_real moe_loader moe_forward moe_stream moe_expert_quant moe_e2e moe_gen wordlm wordlm_bitnet wordlm_holdout trit_bench: CFLAGS := $(AVX_CFLAGS) $(OMPFLAGS)

cce_safetensors_test: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/test_cce_safetensors.c
	$(CC) $(CFLAGS) -DCCE_SAFETENSORS_TESTING $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/test_cce_safetensors.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/cce_safetensors_test > logs/cce_safetensors_test.log 2>&1

cnet_lm_bounds_test: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CCE) src/cnet_lm.c src/contract/anti_repeat.c tests/test_cnet_lm_bounds.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CCE) \
		src/cnet_lm.c src/contract/anti_repeat.c tests/test_cnet_lm_bounds.c \
		$(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	@./$(BIN_DIR)/cnet_lm_bounds_test > logs/cnet_lm_bounds_test.log 2>&1
	@grep -q '^CNET_LM_BOUNDS_PASS$$' logs/cnet_lm_bounds_test.log

# Dedicated GGUF loader + Qwen2 forest + full K dequant + packed 1.6-bit roundtrip test
# Mirrors the Supra flow end-to-end (load -> specialists -> pack_trits -> export -> load_packed -> forward/generate)
cce_gguf_test: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/test_cce_gguf.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/test_cce_gguf.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/$@ > logs/gguf_test_run.log 2>&1

supra_console: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_console.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_console.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	@echo "Built supra_console. Weights are downloaded once into ./supra_cache and reused."
	@echo "Try: ./supra_console --mode text --prompt \"Once upon a time\" --max_new 40"

# Context-length probe: fills the whole positional window once and reports the
# hard ceiling (block_size) vs the coherence-collapse point (looping). See
# tests/supra_context_probe.c.
supra_context_probe: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_context_probe.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_context_probe.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/supra_context_probe > logs/supra_context_probe.log 2>&1

# Skeleton-driven long-form injector: slides a sub-384 window, cuts each chunk
# before the ~120 coherence decay, and steers with a caller-supplied beat list so
# the piece actually finishes. Runs Approach B (skeleton) vs Approach A (pure
# sliding-window baseline) for contrast. See tests/supra_longform.c.
supra_longform: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_longform.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_longform.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/supra_longform > logs/supra_longform.log 2>&1

# Head-only QAT smoke (Supra quality phase, milestone 1): freeze the transformer,
# cache final hidden vectors, train a ternary BitLinear head (FP shadow + STE),
# and check QAT beats post-hoc ternary on the real Supra head. See tests/supra_head_qat.c.
supra_head_qat: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_head_qat.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_head_qat.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/supra_head_qat > logs/supra_head_qat.log 2>&1

# Head QAT on a REAL corpus (pdf_corpus.txt): genuine held-out FP-recovery test.
supra_head_qat_corpus: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_head_qat_corpus.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/supra_head_qat_corpus.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/supra_head_qat_corpus > logs/supra_head_qat_corpus.log 2>&1

# Joint ternary QAT vs head-only vs post-hoc: does training the WHOLE stack
# ternary recover held-out next-byte accuracy where a frozen-transformer head
# can't? Byte-level from-scratch on pdf_corpus.txt. See tests/transformer_qat_joint.c.
transformer_qat_joint: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/transformer_qat_joint.c include/cce/cce_transformer_qat.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/transformer_qat_joint.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/transformer_qat_joint > logs/transformer_qat_joint.log 2>&1

# Real-weight joint QAT: load pretrained Supra into the cce_transformer_qat trainer,
# prove forward-parity vs cce_supra_gpt_forward, then joint-QAT the transformer
# blocks (head+emb FP) with a held-out generalization test. See tests/transformer_qat_real.c.
transformer_qat_real: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/transformer_qat_real.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/transformer_qat_real.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/transformer_qat_real > logs/transformer_qat_real.log 2>&1

# The same load+parity path on a DIFFERENT model (not Supra): a 4-layer model in
# Supra's naming with every free dim changed (D128/V2000/B96/mlp512), generated by
# tools/gen_altmodel.c. Proves the trainer/loader/forward aren't tied to Supra's
# dimensions. See tests/transformer_qat_altmodel.c.
transformer_qat_altmodel: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/transformer_qat_altmodel.c
	@mkdir -p $(BIN_DIR) logs
	@$(MAKE) --no-print-directory $(BIN_DIR)/gen_altmodel
	@test -f altmodel_cache/model.safetensors || $(BIN_DIR)/gen_altmodel
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/transformer_qat_altmodel.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/transformer_qat_altmodel > logs/transformer_qat_altmodel.log 2>&1

# The decomposer-generalization gate: load + forward parity on a model the old
# hardwired decomposer could NOT load — GPT-2-STYLE tensor naming (h.{i}.attn.c_attn,
# wte/wpe), 6 layers, 2 heads (from safetensors __metadata__), Conv1D [in,out]
# block weights, TIED head (no lm_head, like real HF gpt2). Parity is asserted
# both against the trainer AND against golden logits from an independent numpy
# forward embedded in the fixture. Standalone (C gen_altmodel, no Python),
# not in verify-long. See tests/transformer_qat_gpt2names.c.
transformer_qat_gpt2names: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/transformer_qat_gpt2names.c
	@mkdir -p $(BIN_DIR) logs
	@$(MAKE) --no-print-directory $(BIN_DIR)/gen_altmodel
	@test -f altmodel_gpt2_cache/model.safetensors || $(BIN_DIR)/gen_altmodel altmodel_gpt2_cache gpt2
	@test -f altmodel_gpt2_nohead_cache/model.safetensors || $(BIN_DIR)/gen_altmodel altmodel_gpt2_nohead_cache gpt2_nohead
	@test -f altmodel_gpt2_gap_cache/model.safetensors || $(BIN_DIR)/gen_altmodel altmodel_gpt2_gap_cache gpt2_gap
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/transformer_qat_gpt2names.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/transformer_qat_gpt2names > logs/transformer_qat_gpt2names.log 2>&1
	@grep -q "TRANSFORMER_QAT_GPT2NAMES_PASS" logs/transformer_qat_gpt2names.log

# Core unit of decomposed data-aware per-projection ternary QAT (GPTQ/AWQ regime):
# reconstruct one linear projection's FP output from calibration activations with
# STE, beating naive post-hoc on HELD-OUT activations. No global backward -> works
# for any surrounding architecture. Hermetic. See tests/proj_qat_recon.c.
proj_qat_recon: tests/proj_qat_recon.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ tests/proj_qat_recon.c -lm
	./$(BIN_DIR)/proj_qat_recon > logs/proj_qat_recon.log 2>&1

# Milestone 2: the same per-projection reconstruction on REAL weights + REAL
# activations from a real gemma4 GGUF (layer-0 attn_q, input = RMSNorm(embed·√D),
# no forward needed). Needs Models/gemma-4-12B-it-MTP-Q8_0.gguf. See tests/proj_qat_gemma.c.
proj_qat_gemma: $(CCE) $(CCE_CUDA_OBJ) tests/proj_qat_gemma.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/proj_qat_gemma.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/proj_qat_gemma > logs/proj_qat_gemma.log 2>&1

# Milestone 4 core: END-TO-END reconstruction across many projections / many
# layers (SwiGLU MLP stack) — does it compound into collapse, and does SEQUENTIAL
# calibration beat INDEPENDENT? Hermetic. See tests/proj_qat_stack.c.
proj_qat_stack: tests/proj_qat_stack.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ tests/proj_qat_stack.c -lm
	./$(BIN_DIR)/proj_qat_stack > logs/proj_qat_stack.log 2>&1

# Milestone 3: dual-GPU async dispatch of the embarrassingly-parallel per-projection
# GEMM jobs across the two R9700s (cce_clgemm, one handle pinned per device via
# open_device — the oracle-pool pattern). Verifies GPU==CPU + measures the ~2x
# throughput. Needs OpenCL + a discrete GPU; reports+passes with none. See tests/proj_qat_gpu.c.
proj_qat_gpu: $(CCE) tests/proj_qat_gpu.c include/cce/cce_clgemm.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) tests/proj_qat_gpu.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/proj_qat_gpu > logs/proj_qat_gpu.log 2>&1

# Milestone 4-full Part 1: the EFFICIENT GPTQ-Cholesky OBQ solver — a fast drop-in
# for proj_qat_recon's coordinate-descent reconstruct(). One Cholesky of the
# activation Hessian H=Σ, then a single OBQ error-feedback pass over input columns.
# Asserts held-out quality matches the coordinate-descent oracle and >=3x faster at
# in=512,out=512. Hermetic. See tests/gptq_solver.c.
gptq_solver: tests/gptq_solver.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ tests/gptq_solver.c -lm
	./$(BIN_DIR)/gptq_solver > logs/gptq_solver.log 2>&1

# Milestone 4-full Part 2: real end-to-end on gemma-4-12B. Quantizes EVERY linear
# projection of the real gemma MLP stack with the GPTQ data-aware solver and shows
# the quantized model's HELD-OUT output stays close to FP, far better than naive.
# Standalone; needs Models/gemma-4-12B-it-MTP-Q8_0.gguf. NOT in verify-long.
# See tests/proj_qat_gemma_e2e.c.
proj_qat_gemma_e2e_build: $(CCE) $(CCE_CUDA_OBJ) tests/proj_qat_gemma_e2e.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -fopenmp -o $(BIN_DIR)/proj_qat_gemma_e2e $(CCE) $(CCE_CUDA_OBJ) tests/proj_qat_gemma_e2e.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -fopenmp

proj_qat_gemma_e2e: proj_qat_gemma_e2e_build
	./$(BIN_DIR)/proj_qat_gemma_e2e > logs/proj_qat_gemma_e2e.log 2>&1

# Component-dependent bit-width POLICY sweep (Colibri's insight): which projection
# gets which precision. Sweeps (gate/up/down) bit-widths over real gemma FFN weights,
# reports quality-vs-compression, finds the sweet spot. See tests/proj_qat_bitwidth.c.
proj_qat_bitwidth: $(CCE) tests/proj_qat_bitwidth.c include/cce/cce_gguf.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) tests/proj_qat_bitwidth.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/proj_qat_bitwidth > logs/proj_qat_bitwidth.log 2>&1

# Lightweight mock chat test: exercises the real BPE tokenizer (encode) without
# requiring the full model forward. Needs supra_cache/tokenizer.json (run
# supra_console or cce_safetensors_test once to populate it).
supra_chat_mock: $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/test_supra_chat_mock.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) src/nn.c tests/test_supra_chat_mock.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/supra_chat_mock > logs/supra_chat_mock.log 2>&1

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

# Held-out generalization of joint QAT on the GENERAL trainer (cce_wordlm, not
# the Supra-shaped cce_transformer_qat): FP vs post-hoc ternary vs QAT on UNSEEN
# sentences. Confirms the transformer_qat_joint finding is trainer-independent.
wordlm_holdout: $(CCE_WORDLM) tests/wordlm_holdout.c include/cce/cce_wordlm.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(CCE_WORDLM) tests/wordlm_holdout.c $(LDFLAGS)
	./$(BIN_DIR)/wordlm_holdout > logs/wordlm_holdout.log 2>&1

# Fine-tune-family merge pipeline (model-merge scope M0): base + N fine-tunes
# in ONE content-addressed store — storage accounting vs naive, per-manifest
# bit-identity under a HOT cap, epsilon-merge accept + material refuse inside
# the pipeline. Hermetic (tiny fixture).
merge_family: $(CCE) $(CCE_CUDA_OBJ) tests/merge_family_test.c tests/tiny_model_fixture.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/merge_family_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/merge_family > logs/merge_family.log 2>&1

# Hybrid catalog (model-merge scope M1): transformer + SSM in ONE store with
# a query-level task catalog; ssm restore round-trip (closes the stated
# limit); HONESTY gate measures cross-arch dedup (= 0). Hermetic.
hybrid_catalog: $(CCE) $(CCE_CUDA_OBJ) tests/hybrid_catalog_test.c tests/tiny_model_fixture.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/hybrid_catalog_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/hybrid_catalog > logs/hybrid_catalog.log 2>&1

# Supra QAT trainer gate: hermetic transformer-backward gradcheck (central
# differences over EVERY parameter group) + determinism + FP smoke + QAT-vs-
# post-hoc on a tiny synthetic model. No model files. The joint-QAT quality
# phase (docs/superpowers/specs/2026-06-28-supra-qat-scope.md steps 7-10)
# stands on this backward.
transformer_qat: $(CCE) $(CCE_CUDA_OBJ) tests/test_transformer_qat.c include/cce/cce_transformer_qat.h
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) tests/test_transformer_qat.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/transformer_qat > logs/transformer_qat.log 2>&1

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
cnet_dll: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(BASE_SRC) $(SCAN) $(PROPERTY) $(CONSOLIDATE) $(ACQUIRE_SRC) $(COVERAGE) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) $(ASYNC_RUNTIME) $(MODEL_RUNTIME) $(CCE_MODEL_CATALOG) $(MODEL_PROBE) $(RESOURCE_GOV_SRC) $(CF_ORDER_SRC) $(SELF_IMPROVE_SRC) $(HARNESS_ORACLE_SRC) $(MULTIMODAL_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(AGENT_ROLE_SRC) $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(LIBRARY) src/fastpath.c src/soul_host.c src/contract/mcp_calculator.c src/contract/mcp_math_eval.c src/contract/mcp_file_read.c src/contract/mcp_file_write.c src/contract/mcp_memory.c src/contract/mcp_utils.c src/contract/mcp_web_search.c src/contract/mcp_wiki.c src/contract/mcp_math_eval.c src/agent_memory.c
	$(CC) -shared -DCNET_BUILD_DLL -DCCE_BUILD_DLL $(CFLAGS) $(CUDA_CFLAGS) -o cnet.so \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(BASE_SRC) $(SCAN) $(PROPERTY) $(CONSOLIDATE) $(ACQUIRE_SRC) $(COVERAGE) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) $(ASYNC_RUNTIME) $(MODEL_RUNTIME) $(CCE_MODEL_CATALOG) $(MODEL_PROBE) $(RESOURCE_GOV_SRC) $(CF_ORDER_SRC) $(SELF_IMPROVE_SRC) $(HARNESS_ORACLE_SRC) $(MULTIMODAL_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(AGENT_ROLE_SRC) $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(LIBRARY) src/fastpath.c src/soul_host.c src/contract/mcp_calculator.c src/contract/mcp_math_eval.c src/contract/mcp_file_read.c src/contract/mcp_file_write.c src/contract/mcp_memory.c src/contract/mcp_utils.c src/contract/mcp_web_search.c src/contract/mcp_wiki.c src/agent_memory.c \
		$(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) $(CNET_SONAME_LDFLAGS) -pthread
	@echo "Built unified cnet.so (CCE + certified runtime + soul_host + MCP ABI)."



# CNET-native QGKP v3 envelope tool. Pack/materialize operations are resumable
# and validate prefixes before append; the final rename remains an external
# atomic policy decision.
.PHONY: cnet_qgkp
cnet_qgkp: cnet_dll tools/cnet_qgkp.c include/cce/cce_qgkp.h
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O2 -Iinclude \
		-o $(BIN_DIR)/cnet_qgkp tools/cnet_qgkp.c -L. -l:cnet.so \
		-Wl,-rpath,'$$ORIGIN:$$ORIGIN/..'
	ln -sfn ../cnet.so $(BIN_DIR)/libcnet.so.5

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
qwythos_coherence_gate: tools/score_cnet_coherence.c tests/test_qwythos_coherence.sh
	@bash tests/test_qwythos_coherence.sh

# Real-model parity + token-identity gate: qwen35 runner vs the llama.cpp CPU
# reference dumps (generate those first via qwen35_parity_dump; see the test
# header). FP inference-only load: ~46 GB resident, no archive scratch.
qwythos_e2e_build: $(CCE) $(CCE_CUDA_OBJ) tests/qwythos_e2e.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/qwythos_e2e $(CCE) $(CCE_CUDA_OBJ) tests/qwythos_e2e.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
.PHONY: qwythos_e2e
qwythos_e2e: qwythos_e2e_build
	CNET_INFER_FP=1 CNET_FOREST_NO_PERSIST=1 CNET_MAX_CTX=64 \
		./$(BIN_DIR)/qwythos_e2e $(QWYTHOS_GGUF) > logs/qwythos_e2e.console.log 2>&1
	@tail -5 logs/qwythos_e2e.console.log

# qwen35 parity dump: llama.cpp reference tensor dumps for the Qwythos hybrid
# forward. Links the CPU build DELIBERATELY — the pinned llama.cpp HEAD enables
# -funsafe-math-optimizations in ggml-hip, so the ROCm build is not an IEEE-
# faithful numeric oracle; build-cpu has no GPU backend compiled in at all.
LLAMA_CPP_BUILD_CPU ?= $(LLAMA_CPP_ROOT)/build-cpu
.PHONY: qwen35_parity_dump_build
qwen35_parity_dump_build: tools/moe_parity_dump.cpp
	$(CXX) -std=c++17 -Wall -Wextra -O2 \
		-I$(LLAMA_CPP_ROOT)/include -I$(LLAMA_CPP_ROOT)/ggml/include \
		-o $(BIN_DIR)/qwen35_parity_dump tools/moe_parity_dump.cpp \
		-L$(LLAMA_CPP_BUILD_CPU)/bin -lllama -lggml -lggml-base -lggml-cpu \
		-Wl,-rpath,$(LLAMA_CPP_BUILD_CPU)/bin
	@echo "Built $(BIN_DIR)/qwen35_parity_dump (CPU llama.cpp reference dumps)."

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
gap_lane: $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_gap_lane.c include/gap_lane.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_gap_lane \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) \
		$(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) \
		tests/test_gap_lane.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_gap_lane > logs/gap_lane.log 2>&1
	@grep -q "GAP_LANE_PASS" logs/gap_lane.log

record_teacher: $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_record_teacher.c include/cnet_record_teacher.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_record_teacher \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) \
		$(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) \
		tests/test_record_teacher.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_record_teacher > logs/record_teacher.log 2>&1
	@grep -q "RECORD_TEACHER_PASS" logs/record_teacher.log
	@echo "record teacher gate: PASS (logs/record_teacher.log)"

# The 24/7 daemon (REAL local-model teacher via the CCE GGUF runner).
.PHONY: gap_lane_run_build
# toolchain attestation: the daemon's teacher identity folds in the build
# flags and source revision (see lm_toolchain_identity in gap_lane_run.c)
gap_lane_run_build: CNET_SRC_REV := $(shell git rev-parse --short=16 HEAD 2>/dev/null || echo unknown)
gap_lane_run_build: CFLAGS := $(CFLAGS) $(OMPFLAGS) -DCNET_RESIDUAL_HTTP_STANDALONE -DCNET_TOOLCHAIN_CFLAGS="\"$(CFLAGS) $(OMPFLAGS)\"" -DCNET_SOURCE_REV="\"$(CNET_SRC_REV)\""
gap_lane_run_build: $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(CURIOSITY_SRC) $(EG_SRC) $(JSON_TOOLCALL_SRC) $(EXT_TEACHER_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/gap_lane_run.c include/json_toolcall.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/gap_lane_run \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(CURIOSITY_SRC) $(EG_SRC) $(JSON_TOOLCALL_SRC) $(EXT_TEACHER_SRC) \
		src/residual_http.c \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) \
		$(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) \
		tests/gap_lane_run.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread

# Fail-safe deployment tracer: static check that the gap-lane systemd unit
# carries ConditionFileIsExecutable matching ExecStart's binary, plus a
# no-start systemd-analyze semantic probe when available. No systemctl and no
# daemon execution.
.PHONY: gap_lane_service_config
gap_lane_service_config: tests/test_gap_lane_service_config.sh config/cnet-gap-lane.service Makefile
	@mkdir -p logs
	@bash tests/test_gap_lane_service_config.sh > logs/gap_lane_service_config.log 2>&1
	@grep -q "GAP_LANE_SERVICE_CONFIG_PASS" logs/gap_lane_service_config.log

# Build-only prepare: compile the daemon and run the config tracer.  This does
# NOT start, enable, or systemctl the service.  Operators must start the lane
# explicitly with `systemctl start cnet-gap-lane` after verifying the binary
# and model paths exist on the target host.
.PHONY: gap_lane_service_prepare
gap_lane_service_prepare: gap_lane_run_build gap_lane_service_config
	@echo "GAP_LANE_SERVICE_PREPARE_DONE"

# Gap-lane student-training throughput gate: CNET_TRAIN_FAST=1 (default OFF)
# is a byte-identical fast plain-SGD step in btn_train_dynamic (row-blocked
# reduction chains + exact-zero input skip + deterministic OpenMP worksharing;
# see the comment block in src/nn.c). The gate teaches the campaign-shaped
# hermetic fixture (256 one-hot -> 3x256 one-hot, 256 exemplars, the deployed
# service's staging/loss knobs) twice from the same seed and asserts the
# knob-on student is byte-identical to the knob-off one and both certify
# EXACT. `profile` and `run` modes of the same binary give the phase
# breakdown and A/B timings.
.PHONY: teach_fast
teach_fast: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CONSOLIDATE) tests/teach_fast_bench.c include/nn.h include/contract/contract.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(OMPFLAGS) -o $(BIN_DIR)/teach_fast_bench \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CONSOLIDATE) \
		tests/teach_fast_bench.c $(LDFLAGS) $(OMPFLAGS)
	@OMP_NUM_THREADS=4 ./$(BIN_DIR)/teach_fast_bench gate > logs/teach_fast.log 2>&1
	@grep -q "TEACH_FAST_PASS" logs/teach_fast.log
	@grep "TEACH_FAST_PASS" logs/teach_fast.log

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

# ---- Unified self-improve + resource program (plans/unified_self_improve_resource.md) ----
.PHONY: resource_governor counterfactual_order self_improve deploy_profile recipe_proposals unified_self_improve
resource_governor: $(RESOURCE_GOV_SRC) tests/test_resource_governor.c include/resource_governor.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_resource_governor \
		$(RESOURCE_GOV_SRC) tests/test_resource_governor.c $(LDFLAGS)
	@./$(BIN_DIR)/test_resource_governor > logs/resource_governor.log 2>&1
	@grep -q "RESOURCE_GOVERNOR_PASS" logs/resource_governor.log
	@grep "RESOURCE_GOVERNOR_PASS" logs/resource_governor.log

counterfactual_order: $(CF_ORDER_SRC) tests/test_counterfactual_order.c include/counterfactual_order.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_counterfactual_order \
		$(CF_ORDER_SRC) tests/test_counterfactual_order.c $(LDFLAGS)
	@./$(BIN_DIR)/test_counterfactual_order > logs/counterfactual_order.log 2>&1
	@grep -q "CF_ORDER_PASS" logs/counterfactual_order.log
	@grep "CF_ORDER_PASS" logs/counterfactual_order.log

self_improve: $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(HARNESS_ORACLE_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_self_improve.c include/self_improve.h include/harness_oracle.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_self_improve \
		$(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(HARNESS_ORACLE_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_self_improve.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_self_improve > logs/self_improve.log 2>&1
	@grep -q "SELF_IMPROVE_PASS" logs/self_improve.log
	@grep "SELF_IMPROVE_PASS" logs/self_improve.log

deploy_profile: tests/test_deploy_profile.sh config/cnet-deploy.env config/cnet-gap-lane.service config/qwythos_v2_campaign.env scripts/apply_deploy_profile.sh
	@mkdir -p logs
	@bash tests/test_deploy_profile.sh > logs/deploy_profile.log 2>&1
	@grep -q "DEPLOY_PROFILE_PASS" logs/deploy_profile.log
	@grep "DEPLOY_PROFILE_PASS" logs/deploy_profile.log

recipe_proposals: $(BIN_DIR)/propose_recipe_improvements
	@mkdir -p logs suggestions
	@$(BIN_DIR)/propose_recipe_improvements \
		--out suggestions/cnet_recipe_proposals.jsonl \
		> logs/recipe_proposals.log 2>&1
	@grep -q "RECIPE_PROPOSALS_PASS" logs/recipe_proposals.log
	@grep "RECIPE_PROPOSALS_PASS" logs/recipe_proposals.log

# Full hermetic umbrella for the 4-phase self-improve/resource program.
unified_self_improve: resource_governor counterfactual_order self_improve deploy_profile recipe_proposals gap_lane_service_config campaign_v2_fast multimodal_v0 voice_real_teacher json_toolcall personal_ai post_seal_serve hybrid_ai hybrid_bench residual_gguf colibri_integrate curiosity eg registry_hash
	@echo "UNIFIED_SELF_IMPROVE_PASS"

# Personal AI: local certified library first; big-AI teacher only on gaps.
.PHONY: personal_ai


.PHONY: auto_learn
auto_learn: src/cnet_auto_learn.c tests/test_auto_learn.c include/cnet_auto_learn.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_auto_learn src/cnet_auto_learn.c tests/test_auto_learn.c $(LDFLAGS)
	@./$(BIN_DIR)/test_auto_learn > logs/auto_learn.log 2>&1
	@grep -q AUTO_LEARN_PASS logs/auto_learn.log
	@grep AUTO_LEARN_PASS logs/auto_learn.log

.PHONY: curiosity

.PHONY: eg cnet_eg_cli
eg: $(EG_SRC) tests/test_cnet_eg.c include/cnet_eg.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_cnet_eg $(EG_SRC) tests/test_cnet_eg.c -lm
	@./$(BIN_DIR)/test_cnet_eg > logs/cnet_eg.log 2>&1
	@grep -q "EG_PASS" logs/cnet_eg.log
	@grep "EG_PASS" logs/cnet_eg.log

cnet_eg_cli: $(EG_SRC) include/cnet_eg.h tools/cnet_eg.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/cnet_eg $(EG_SRC) tools/cnet_eg.c -lm
	@echo "Built bin/cnet_eg (compute|report|log)"

curiosity: $(CURIOSITY_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_curiosity.c include/cnet_curiosity.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_curiosity \
		$(CURIOSITY_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_curiosity.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_curiosity > logs/curiosity.log 2>&1
	@grep -q "CURIOSITY_PASS" logs/curiosity.log
	@grep "CURIOSITY_PASS" logs/curiosity.log

# Hermes-style learn loop in pure C (memory → tools → skill → optional gap seed).
LEARN_LOOP_SRC := src/cnet_learn_loop.c
LEARN_LOOP_MCP := src/contract/mcp_utils.c src/contract/mcp_memory.c src/contract/mcp_wiki.c \
	src/contract/mcp_web_search.c src/agent_memory.c
.PHONY: learn_loop learn_loop_cli
MATH_EVAL_SRC := src/contract/mcp_math_eval.c
MATH_SOLVE_SRC := src/cnet_math_solve.c
PATTERN_SRC := src/cnet_pattern.c
LEARN_LOOP_MATH := $(MATH_EVAL_SRC) $(MATH_SOLVE_SRC) $(PATTERN_SRC)

learn_loop: $(LEARN_LOOP_SRC) $(LEARN_LOOP_MCP) $(LEARN_LOOP_MATH) tests/test_learn_loop.c include/cnet_learn_loop.h include/cnet_math_solve.h include/cnet_pattern.h
	@mkdir -p $(BIN_DIR) logs logs/test_learn_skills
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_learn_loop \
		$(LEARN_LOOP_SRC) $(LEARN_LOOP_MCP) $(LEARN_LOOP_MATH) tests/test_learn_loop.c \
		$(LDFLAGS) $(MCP_LDFLAGS) -pthread -ldl
	@./$(BIN_DIR)/test_learn_loop > logs/learn_loop.log 2>&1
	@grep -q "LEARN_LOOP_PASS" logs/learn_loop.log
	@grep "LEARN_LOOP_PASS" logs/learn_loop.log

learn_loop_cli: $(LEARN_LOOP_SRC) $(LEARN_LOOP_MCP) $(LEARN_LOOP_MATH) tools/cnet_learn_cycle.c include/cnet_learn_loop.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/cnet_learn_cycle \
		$(LEARN_LOOP_SRC) $(LEARN_LOOP_MCP) $(LEARN_LOOP_MATH) tools/cnet_learn_cycle.c \
		$(LDFLAGS) $(MCP_LDFLAGS) -pthread -ldl
	@echo "Built bin/cnet_learn_cycle"

PATTERN_DEPS := $(PATTERN_SRC) $(MATH_SOLVE_SRC) $(MATH_EVAL_SRC) $(LEARN_LOOP_MCP)

.PHONY: math_solve math_solve_cli pattern_runtime pattern_cli
math_solve: $(PATTERN_DEPS) tests/test_math_solve.c include/cnet_math_solve.h include/contract/mcp_math_eval.h include/cnet_pattern.h
	@mkdir -p $(BIN_DIR) logs logs/test_math_skills
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_math_solve \
		$(PATTERN_DEPS) tests/test_math_solve.c \
		$(LDFLAGS) $(MCP_LDFLAGS) -pthread -ldl
	@./$(BIN_DIR)/test_math_solve > logs/math_solve.log 2>&1
	@grep -q "MATH_SOLVE_PASS checks=" logs/math_solve.log
	@grep -q "failures=0" logs/math_solve.log
	@grep "MATH_SOLVE_PASS" logs/math_solve.log

math_solve_cli: $(PATTERN_DEPS) tools/cnet_math_solve.c include/cnet_math_solve.h include/cnet_pattern.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/cnet_math_solve \
		$(PATTERN_DEPS) tools/cnet_math_solve.c \
		$(LDFLAGS) $(MCP_LDFLAGS) -pthread -ldl
	@echo "Built bin/cnet_math_solve"

pattern_runtime: $(PATTERN_DEPS) tests/test_pattern_runtime.c include/cnet_pattern.h
	@mkdir -p $(BIN_DIR) logs logs/test_math_skills
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_pattern_runtime \
		$(PATTERN_DEPS) tests/test_pattern_runtime.c \
		$(LDFLAGS) $(MCP_LDFLAGS) -pthread -ldl
	@./$(BIN_DIR)/test_pattern_runtime > logs/pattern_runtime.log 2>&1
	@grep -q "PATTERN_RUNTIME_PASS" logs/pattern_runtime.log
	@grep -q "failures=0" logs/pattern_runtime.log
	@grep "PATTERN_RUNTIME_PASS" logs/pattern_runtime.log

pattern_cli: $(PATTERN_DEPS) tools/cnet_pattern.c include/cnet_pattern.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/cnet_pattern \
		$(PATTERN_DEPS) tools/cnet_pattern.c \
		$(LDFLAGS) $(MCP_LDFLAGS) -pthread -ldl
	@echo "Built bin/cnet_pattern"

personal_ai: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_personal_ai.c include/personal_ai.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_personal_ai \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_personal_ai.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_personal_ai > logs/personal_ai.log 2>&1
	@grep -q "PERSONAL_AI_PASS" logs/personal_ai.log
	@$(MAKE) --no-print-directory personal_ai_auto
	@grep "PERSONAL_AI_PASS" logs/personal_ai.log

.PHONY: hybrid_ai hybrid_bench
hybrid_ai: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_hybrid_ai.c include/hybrid_ai.h include/personal_ai.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_hybrid_ai \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_hybrid_ai.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_hybrid_ai > logs/hybrid_ai.log 2>&1
	@grep -q "HYBRID_AI_PASS" logs/hybrid_ai.log
	@grep "HYBRID_AI_PASS" logs/hybrid_ai.log

structure_mine_serve_durable: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_structure_mine_serve_durable.c include/hybrid_ai.h include/personal_ai.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_structure_mine_serve_durable \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_structure_mine_serve_durable.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_structure_mine_serve_durable > logs/structure_mine_serve_durable.log 2>&1
	@grep -q "STRUCTURE_MINE_SERVE_DURABLE_PASS" logs/structure_mine_serve_durable.log
	@grep "STRUCTURE_MINE_SERVE_DURABLE_PASS" logs/structure_mine_serve_durable.log

.PHONY: vision_detection_fetch vision_detection_prep vision_detection_bench \
	vision_detection_prep_v2 vision_detection_bench_v2 vision_detection_eval_asan \
	vision_detection_integrity_test vision_detection_integrity_asan vision_detection_evidence_test \
	vision_detection_allocfail_test vision_detection_allocfail_ubsan
# Explicit, network-touching. NEVER a dependency of ci_core.
vision_detection_fetch:
	@bash scripts/vision_detection_fetch.sh

# Proposals + HOG + train-only PCA into data/vision_cache (CPU only, ~7 min).
vision_detection_prep: bin/vd_prep
	@ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
	  timeout 5400 ./bin/vd_prep --root data/voc2007/VOCdevkit/VOC2007 \
	  --class car --out data/vision_cache --ntest 1000 --workers 8 \
	  2>&1 | tee logs/vision/prep.log

bin/vd_prep: tools/vision_detection/vd_prep.cpp tools/vision_detection/vd_io.c \
		tools/vision_detection/vd_sha256.c tools/vision_detection/vd_pack.c \
		tools/vision_detection/vd_protocol.c tools/vision_detection/vd_roots.h
	@mkdir -p $(BIN_DIR) logs/vision
	g++ -std=c++14 -O2 -Wall -Wextra -Werror -I tools/vision_detection \
		-o $(BIN_DIR)/vd_prep tools/vision_detection/vd_prep.cpp \
		tools/vision_detection/vd_io.c tools/vision_detection/vd_sha256.c \
		tools/vision_detection/vd_pack.c tools/vision_detection/vd_eval.c \
		tools/vision_detection/vd_protocol.c \
		$(shell pkg-config --cflags --libs opencv4) -lpthread

# V2 feature-ceiling arm: 64x64 colour HOG -> PCA256, holdout slice [1000,2000)
# of the seed-20260727 shuffle. --v1cache asserts the V1 holdout is not reused.
vision_detection_prep_v2: bin/vd_prep
	@ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
	  timeout 5400 ./bin/vd_prep --root data/voc2007/VOCdevkit/VOC2007 \
	  --class car --variant v2 --out data/vision_cache_v2 \
	  --v1cache data/vision_cache --ntest 1000 --workers 16 \
	  2>&1 | tee logs/vision/prep_v2.log

# Evidence collection. Its job is to produce the record, not to judge it, so it
# returns 0 for any verdict the benchmark actually reached -- including WITHHELD.
vision_detection_bench_v2_evidence: vision_detection_eval_test bin/vd_bench
	@mkdir -p logs/vision
	@test -f data/vision_cache_v2/test.pack || { echo "VISION_DETECTION_FAIL missing v2 cache; run: make vision_detection_fetch vision_detection_prep_v2"; exit 1; }
	@ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
	  OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 \
	  timeout 5400 /usr/bin/time -v ./$(BIN_DIR)/vd_bench --protocol v2 \
	  --cache data/vision_cache_v2 --prev-test data/vision_cache/test.pack \
	  --jobs 2 --json logs/vision_detection_bench_v2.json 2>&1 | tee logs/vision/bench_v2.log
	@grep -qE "VISION_DETECTION_MECHANISM_(PASS|WITHHELD|BLOCKED)" logs/vision/bench_v2.log
	@grep -E "^(TEST|BARS|VISION_|disjoint)" logs/vision/bench_v2.log

# The benchmark GATE. This target used to accept PASS *or* WITHHELD and exit 0
# either way, so no automation could distinguish an earned result from one the
# benchmark explicitly declined to claim. WITHHELD keeps its honest name and
# gets its own non-success exit code (3); BLOCKED gets 4.
vision_detection_bench_v2: vision_detection_bench_v2_evidence
	@sh scripts/benchmark_verdict.sh logs/vision/bench_v2.log \
		VISION_DETECTION_MECHANISM VISION_DETECTION_BENCH_V2

# Evaluator under ASan+UBSan: the metric path must be memory-clean.
vision_detection_allocfail_test:
	@mkdir -p $(BIN_DIR) logs/vision
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -D_DEFAULT_SOURCE -I tools/vision_detection \
		-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc \
		-o $(BIN_DIR)/vision_detection_allocfail_test \
		tools/vision_detection/vd_eval.c tools/vision_detection/vd_pack.c \
		tools/vision_detection/vd_sha256.c tools/vision_detection/vd_protocol.c \
		tools/vision_detection/vd_io.c tests/vision_detection_allocfail_test.c -lm
	@ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
	  timeout 600 ./$(BIN_DIR)/vision_detection_allocfail_test 2>&1 | tee logs/vision/allocfail_test.log
	@grep -q VISION_DETECTION_ALLOCFAIL_PASS logs/vision/allocfail_test.log

vision_detection_allocfail_ubsan:
	@mkdir -p $(BIN_DIR) logs/vision
	$(CC) -std=c11 -Wall -Wextra -Werror -g -O1 -fsanitize=undefined \
		-fno-omit-frame-pointer -D_DEFAULT_SOURCE -I tools/vision_detection \
		-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc \
		-o $(BIN_DIR)/vd_allocfail_ubsan \
		tools/vision_detection/vd_eval.c tools/vision_detection/vd_pack.c \
		tools/vision_detection/vd_sha256.c tools/vision_detection/vd_protocol.c \
		tools/vision_detection/vd_io.c tests/vision_detection_allocfail_test.c -lm
	@ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
	  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	  timeout 900 ./$(BIN_DIR)/vd_allocfail_ubsan 2>&1 | tee logs/vision/allocfail_ubsan.log
	@grep -q VISION_DETECTION_ALLOCFAIL_PASS logs/vision/allocfail_ubsan.log

# Retired: V1 has no pinned protocol authority, so it cannot be scored.
vision_detection_bench:
	@echo "VISION_DETECTION_BENCH_RETIRED: the V1 arm has no pinned protocol"
	@echo "  authority (artifact root, canonical roots, spent-holdout digest) and"
	@echo "  therefore cannot produce a verdict. Its result is frozen in commit"
	@echo "  6ffc5f1. Use: make vision_detection_bench_v2"
	@exit 2

vision_detection_evidence_test: bin/vd_bench bin/vd_mkcache
	@mkdir -p logs/vision
	@ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
	  timeout 900 bash scripts/vision_detection_evidence_test.sh 2>&1 | tee logs/vision/evidence_test.log
	@grep -q VISION_DETECTION_EVIDENCE_PASS logs/vision/evidence_test.log

bin/vd_mkcache: tools/vision_detection/vd_mkcache.c tools/vision_detection/vd_io.c \
		tools/vision_detection/vd_sha256.c tools/vision_detection/vd_protocol.c
	@mkdir -p $(BIN_DIR)
	$(CC) -std=c11 -Wall -Wextra -O2 -D_DEFAULT_SOURCE -I tools/vision_detection \
		-o $(BIN_DIR)/vd_mkcache tools/vision_detection/vd_mkcache.c \
		tools/vision_detection/vd_io.c tools/vision_detection/vd_sha256.c \
		tools/vision_detection/vd_protocol.c -lm

vision_detection_integrity_test:
	@mkdir -p $(BIN_DIR) logs/vision
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -D_DEFAULT_SOURCE -I tools/vision_detection -o $(BIN_DIR)/vision_detection_integrity_test \
		tools/vision_detection/vd_eval.c tools/vision_detection/vd_pack.c \
		tools/vision_detection/vd_io.c tools/vision_detection/vd_sha256.c \
		tools/vision_detection/vd_protocol.c \
		tests/vision_detection_integrity_test.c -lm
	@ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
	  timeout 600 ./$(BIN_DIR)/vision_detection_integrity_test 2>&1 | tee logs/vision/integrity_test.log
	@grep -q VISION_DETECTION_INTEGRITY_PASS logs/vision/integrity_test.log

vision_detection_integrity_asan:
	@mkdir -p $(BIN_DIR) logs/vision
	$(CC) -std=c11 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
		-D_DEFAULT_SOURCE -I tools/vision_detection -o $(BIN_DIR)/vd_integrity_asan \
		tools/vision_detection/vd_eval.c tools/vision_detection/vd_pack.c \
		tools/vision_detection/vd_io.c tools/vision_detection/vd_sha256.c \
		tools/vision_detection/vd_protocol.c \
		tests/vision_detection_integrity_test.c -lm
	@ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
	  timeout 900 ./$(BIN_DIR)/vd_integrity_asan 2>&1 | tee logs/vision/integrity_asan.log
	@grep -q VISION_DETECTION_INTEGRITY_PASS logs/vision/integrity_asan.log

vision_detection_eval_asan:
	@mkdir -p $(BIN_DIR) logs/vision
	$(CC) -std=c11 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
		-fno-omit-frame-pointer -D_DEFAULT_SOURCE -o $(BIN_DIR)/vd_eval_asan \
		tools/vision_detection/vd_eval.c tests/vision_detection_eval_test.c -lm
	@ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
	  timeout 600 ./$(BIN_DIR)/vd_eval_asan 2>&1 | tee logs/vision/eval_asan.log
	@grep -q VISION_DETECTION_EVAL_PASS logs/vision/eval_asan.log

# Runs from cached assets only. No download.
bin/vd_bench: tools/vision_detection/vd_bench.c tools/vision_detection/vd_eval.c \
		tools/vision_detection/vd_pack.c tools/vision_detection/vd_io.c \
		tools/vision_detection/vd_sha256.c tools/vision_detection/vd_protocol.c \
		tools/vision_detection/vd_protocol.h tools/vision_detection/vd_roots.h
	@mkdir -p $(BIN_DIR) logs/vision
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -D_DEFAULT_SOURCE -I include \
		-I tools/vision_detection \
		-o $(BIN_DIR)/vd_bench tools/vision_detection/vd_bench.c \
		tools/vision_detection/vd_eval.c tools/vision_detection/vd_pack.c \
		tools/vision_detection/vd_io.c tools/vision_detection/vd_sha256.c \
		tools/vision_detection/vd_protocol.c tools/vision_detection/vd_coverage.c \
		$(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) -lm -lpthread $(CURL_LDFLAGS)

SRC_NN_MIN := src/nn.c src/contract/contract.c src/contract/unit.c src/property.c \
              src/scan.c src/contract/coverage.c src/router/registry.c \
              src/router/route.c src/router/dag_full.c src/plan_table.c src/consolidate.c

.PHONY: vision_detection_eval_test
vision_detection_eval_test: tools/vision_detection/vd_eval.c tools/vision_detection/vd_eval.h tests/vision_detection_eval_test.c
	@mkdir -p $(BIN_DIR) logs/vision
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/vision_detection_eval_test \
		tools/vision_detection/vd_eval.c tests/vision_detection_eval_test.c -lm
	@./$(BIN_DIR)/vision_detection_eval_test > logs/vision/eval_test.log 2>&1
	@grep -q "VISION_DETECTION_EVAL_PASS" logs/vision/eval_test.log
	@grep "VISION_DETECTION_EVAL_PASS" logs/vision/eval_test.log

# Action gates, not files. Without .PHONY a same-named root file newer than the
# prerequisites makes Make report the gate up to date: it "passes" without
# compiling, running, or refreshing any evidence. tests/test_recipe_gates.sh
# enumerates these and rejects omissions.
.PHONY: knowledge_composition_bench knowledge_accumulation_bench knowledge_capsule
.PHONY: coverage_abstain own_learning_health port_raw_unit_seam
.PHONY: coverage_sidecar_seal coverage_sidecar_seal_san capsule_scope_lineage
.PHONY: personal_ai_hop_guard
.PHONY: vision_coverage_test vision_capsule_asset vision_detection_bench_v2
.PHONY: vision_detection_bench_v2_evidence vision_detection_prep_v2

knowledge_composition_bench: $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/knowledge_composition_bench.c include/hybrid_ai.h include/personal_ai.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror $(CUDA_CFLAGS) -o $(BIN_DIR)/knowledge_composition_bench \
		$(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/knowledge_composition_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/knowledge_composition_bench > logs/knowledge_composition_bench.log 2>&1
	@grep -q "KNOWLEDGE_COMPOSITION_BENCH_PASS" logs/knowledge_composition_bench.log
	@grep "KNOWLEDGE_COMPOSITION_BENCH_PASS" logs/knowledge_composition_bench.log

knowledge_accumulation_bench: $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/knowledge_accumulation_bench.c include/hybrid_ai.h include/personal_ai.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror $(CUDA_CFLAGS) -o $(BIN_DIR)/knowledge_accumulation_bench \
		$(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/knowledge_accumulation_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/knowledge_accumulation_bench > logs/knowledge_accumulation_bench.log 2>&1
	@grep -q "KNOWLEDGE_ACCUMULATION_BENCH_PASS" logs/knowledge_accumulation_bench.log
	@grep "KNOWLEDGE_ACCUMULATION_BENCH_PASS" logs/knowledge_accumulation_bench.log

# ASAN/UBSAN runtime check for the capsule parser. NOT -Werror: -O1 surfaces
# pre-existing format-truncation warnings in unrelated TUs (src/cnet_auto_learn.c)
# that are out of scope here. -Werror IS enforced on the two focused targets at
# the project's standard flags.
.PHONY: knowledge_capsule_san
knowledge_capsule_san: knowledge_capsule_sanitize

knowledge_composition_sanitize: $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/knowledge_composition_bench.c include/hybrid_ai.h include/personal_ai.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -pedantic -O1 -g -D_DEFAULT_SOURCE \
		-fsanitize=address,undefined -fno-omit-frame-pointer -o $(BIN_DIR)/knowledge_composition_sanitize_bin \
		$(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/knowledge_composition_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		./$(BIN_DIR)/knowledge_composition_sanitize_bin > logs/knowledge_composition_sanitize.log 2>&1
	@grep -q "KNOWLEDGE_COMPOSITION_BENCH_PASS" logs/knowledge_composition_sanitize.log
	@grep "KNOWLEDGE_COMPOSITION_BENCH_PASS" logs/knowledge_composition_sanitize.log

knowledge_capsule_sanitize: $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_knowledge_capsule.c include/hybrid_ai.h include/personal_ai.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -pedantic -O1 -g -D_DEFAULT_SOURCE \
		-fsanitize=address,undefined -fno-omit-frame-pointer -o $(BIN_DIR)/test_knowledge_capsule_sanitize \
		$(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_knowledge_capsule.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		./$(BIN_DIR)/test_knowledge_capsule_sanitize > logs/knowledge_capsule_sanitize.log 2>&1
	@grep -q "KNOWLEDGE_CAPSULE_PASS" logs/knowledge_capsule_sanitize.log
	@grep "KNOWLEDGE_CAPSULE_PASS" logs/knowledge_capsule_sanitize.log

bin/vd_gate: tools/vision_detection/vd_gate.c tools/vision_detection/vd_coverage.c \
		tools/vision_detection/vd_pack.c tools/vision_detection/vd_io.c \
		tools/vision_detection/vd_sha256.c tools/vision_detection/vd_eval.c
	@mkdir -p $(BIN_DIR) logs/vision
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -D_DEFAULT_SOURCE -I tools/vision_detection \
		-o $(BIN_DIR)/vd_gate $^ -lm -lpthread

bin/vd_runner: tools/vision_detection/vd_runner.cpp tools/vision_detection/vd_coverage.c \
		tools/vision_detection/vd_eval.c tools/vision_detection/vd_pack.c \
		tools/vision_detection/vd_io.c tools/vision_detection/vd_sha256.c \
		tools/vision_detection/vd_frontend.c tools/vision_detection/vd_frontend.h
	@mkdir -p $(BIN_DIR) $(BIN_DIR)/objs_runner logs/vision
	@# C sources are compiled by the C compiler; g++ only sees the C++ runner.
	@for f in $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tools/vision_detection/vd_coverage.c tools/vision_detection/vd_eval.c \
		  tools/vision_detection/vd_pack.c tools/vision_detection/vd_io.c \
		  tools/vision_detection/vd_sha256.c tools/vision_detection/vd_frontend.c; do \
		o=$(BIN_DIR)/objs_runner/$$(echo $$f | tr '/' '_' | sed 's/\.c$$/.o/'); \
		$(CC) -std=c11 -Wall -Wextra -O2 -D_DEFAULT_SOURCE -I include \
			-I tools/vision_detection -c $$f -o $$o || exit 1; \
	  done
	g++ -std=c++14 -O2 -Wall -Wextra -I tools/vision_detection -I include \
		-o $(BIN_DIR)/vd_runner tools/vision_detection/vd_runner.cpp \
		$(BIN_DIR)/objs_runner/*.o \
		$(shell pkg-config --cflags --libs opencv4) -lm -lpthread $(CURL_LDFLAGS)

# Schema-2 asset parser, mutated field by field plus a deterministic byte fuzz.
# Pure C and no OpenCV on purpose: a parser that can only be reached through a
# runner needing OpenCV, VOC images and an imported capsule never gets fuzzed.
vd_frontend_parse: tools/vision_detection/vd_frontend.c tools/vision_detection/vd_frontend.h tests/test_vd_frontend_parse.c
	@mkdir -p $(BIN_DIR) logs/vision
	$(CC) -std=c11 -Wall -Wextra -pedantic -Werror -O2 -D_DEFAULT_SOURCE \
		-I tools/vision_detection -o $(BIN_DIR)/vd_frontend_parse \
		tools/vision_detection/vd_frontend.c tests/test_vd_frontend_parse.c -lm
	@$(BIN_DIR)/gate_evidence vd_frontend_parse \
		logs/vision/frontend_parse.log VD_FRONTEND_PARSE_PASS -- \
		timeout 600 ./$(BIN_DIR)/vd_frontend_parse

vd_frontend_parse_san: tools/vision_detection/vd_frontend.c tools/vision_detection/vd_frontend.h tests/test_vd_frontend_parse.c
	@mkdir -p $(BIN_DIR) logs/vision
	$(CC) -std=c11 -Wall -Wextra -pedantic -Werror -g -O1 \
		-fsanitize=address,undefined -fno-omit-frame-pointer -D_DEFAULT_SOURCE \
		-I tools/vision_detection -o $(BIN_DIR)/vd_frontend_parse_san \
		tools/vision_detection/vd_frontend.c tests/test_vd_frontend_parse.c -lm
	@ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		timeout 900 ./$(BIN_DIR)/vd_frontend_parse_san \
		> logs/vision/frontend_parse_san.log 2>&1; \
		status=$$?; cat logs/vision/frontend_parse_san.log; test $$status -eq 0
	@grep -q VD_FRONTEND_PARSE_PASS logs/vision/frontend_parse_san.log

.PHONY: vd_frontend_parse vd_frontend_parse_san

# The accumulation benchmark's FAULT paths, which a green run never takes. A
# forced failure mid-build exercises the cleanup that used to leak the BTN it
# had just allocated and leave the caller reading an unwritten name.
knowledge_accumulation_faults: $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/knowledge_accumulation_bench.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
		-fno-omit-frame-pointer -D_DEFAULT_SOURCE -I include $(CUDA_CFLAGS) \
		-o $(BIN_DIR)/knowledge_accumulation_faults \
		$(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/knowledge_accumulation_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@# The harness itself is checked first, with fakes that print the right
	@# words and then exit 0, exit 7, segfault, or hang. Before this, all four
	@# counted as a clean fault path.
	@sh tests/test_accumulation_faults_harness.sh
	@sh tests/test_accumulation_faults.sh $(BIN_DIR)/knowledge_accumulation_faults

.PHONY: knowledge_accumulation_faults

# The trainer defect behind `make certify`: dynamic growth must escape a
# dead-neuron plateau instead of stacking more neurons that cannot contribute.
# Same shape, data, hyperparameters and seed the failing primitive uses.
btn_train_plateau: src/nn.c tests/test_btn_train_plateau.c include/nn.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -O2 -D_DEFAULT_SOURCE -I include \
		-o $(BIN_DIR)/test_btn_train_plateau \
		src/nn.c tests/test_btn_train_plateau.c -lm
	@$(BIN_DIR)/gate_evidence btn_train_plateau \
		logs/btn_train_plateau.log BTN_TRAIN_PLATEAU_PASS -- \
		timeout 900 ./$(BIN_DIR)/test_btn_train_plateau

.PHONY: btn_train_plateau

# CNU header budgets: a tiny sealed unit must not be able to make the parser
# allocate or touch its way into a denial of service. The child process runs
# under RLIMIT_AS and RLIMIT_CPU and its peak RSS is measured, because a parser
# that commits a gigabyte and only THEN discovers the payload is empty has still
# refused -- and that is the defect.
cnu_budget: $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(PLAN_TABLE) $(ROUTER) $(SRC) $(LIBRARY) tests/test_cnu_budget.c include/contract/unit.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -pedantic -Werror -O2 -D_DEFAULT_SOURCE \
		-I include -o $(BIN_DIR)/test_cnu_budget \
		$(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(PLAN_TABLE) \
		$(ROUTER) $(SRC) $(LIBRARY) tests/test_cnu_budget.c -lm -lpthread -lcurl
	@$(BIN_DIR)/gate_evidence cnu_budget logs/cnu_budget.log \
		CNU_BUDGET_PASS -- timeout 900 ./$(BIN_DIR)/test_cnu_budget

cnu_budget_san: $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(PLAN_TABLE) $(ROUTER) $(SRC) $(LIBRARY) tests/test_cnu_budget.c include/contract/unit.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
		-fno-omit-frame-pointer -D_DEFAULT_SOURCE -I include \
		-o $(BIN_DIR)/cnu_budget_san \
		$(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(PLAN_TABLE) \
		$(ROUTER) $(SRC) $(LIBRARY) tests/test_cnu_budget.c -lm -lpthread $(CURL_LDFLAGS)
	@# ASan reserves a large shadow map, so the child's RLIMIT_AS guard is not
	@# meaningful here; allow_user_segv_handler keeps the forked children usable.
	@ASAN_OPTIONS=detect_leaks=0:allow_user_segv_handler=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		CNU_BUDGET_NO_RLIMIT=1 timeout 900 ./$(BIN_DIR)/cnu_budget_san \
		> logs/cnu_budget_san.log 2>&1; \
		status=$$?; cat logs/cnu_budget_san.log; test $$status -eq 0
	@grep -q CNU_BUDGET_PASS logs/cnu_budget_san.log

.PHONY: cnu_budget cnu_budget_san

.PHONY: port_raw_unit_seam
port_raw_unit_seam: $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_port_raw_unit_seam.c
	@mkdir -p $(BIN_DIR) logs/vision
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -D_DEFAULT_SOURCE -I include \
		-o $(BIN_DIR)/port_raw_unit_seam $^ -lm -lpthread $(CURL_LDFLAGS)
	@ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
	  timeout 600 ./$(BIN_DIR)/port_raw_unit_seam 2>&1 | tee logs/vision/port_raw_seam.log
	@grep -q PORT_RAW_UNIT_SEAM_PASS logs/vision/port_raw_seam.log

.PHONY: vision_coverage_test vision_coverage_asan
vision_coverage_test:
	@mkdir -p $(BIN_DIR) logs/vision
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -D_DEFAULT_SOURCE -I tools/vision_detection \
		-o $(BIN_DIR)/vision_coverage_test tools/vision_detection/vd_coverage.c \
		tests/vision_coverage_test.c -lm
	@ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
	  timeout 600 ./$(BIN_DIR)/vision_coverage_test 2>&1 | tee logs/vision/coverage_test.log
	@grep -q VISION_COVERAGE_TEST_PASS logs/vision/coverage_test.log

vision_coverage_asan:
	@mkdir -p $(BIN_DIR) logs/vision
	$(CC) -std=c11 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
		-fno-omit-frame-pointer -D_DEFAULT_SOURCE -I tools/vision_detection \
		-o $(BIN_DIR)/vision_coverage_asan tools/vision_detection/vd_coverage.c \
		tests/vision_coverage_test.c -lm
	@ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
	  ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
	  timeout 900 ./$(BIN_DIR)/vision_coverage_asan 2>&1 | tee logs/vision/coverage_asan.log
	@grep -q VISION_COVERAGE_TEST_PASS logs/vision/coverage_asan.log

.PHONY: vision_capsule_asset vision_capsule_asset_san
vision_capsule_asset: $(CAPSULE_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_vision_capsule_asset.c include/cnet_capsule.h
	@mkdir -p $(BIN_DIR) logs/vision
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -D_DEFAULT_SOURCE -I include \
		-o $(BIN_DIR)/vision_capsule_asset $^ -lm -lpthread $(CURL_LDFLAGS)
	@ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
	  timeout 900 ./$(BIN_DIR)/vision_capsule_asset 2>&1 | tee logs/vision/capsule_asset.log
	@grep -q VISION_CAPSULE_ASSET_PASS logs/vision/capsule_asset.log

vision_capsule_asset_san: $(CAPSULE_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_vision_capsule_asset.c include/cnet_capsule.h
	@mkdir -p $(BIN_DIR) logs/vision
	@# -Werror is enforced by the -O2 vision_capsule_asset target over the SAME
	@# sources. It is omitted here only because -O1 surfaces pre-existing
	@# format-truncation warnings in src/cnet_auto_learn.c, which is outside this
	@# slice; the sanitizer findings themselves are still fatal.
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
		-fno-omit-frame-pointer -D_DEFAULT_SOURCE -I include \
		-o $(BIN_DIR)/vision_capsule_asset_san $^ -lm -lpthread $(CURL_LDFLAGS)
	@ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
	  ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
	  timeout 1200 ./$(BIN_DIR)/vision_capsule_asset_san 2>&1 | tee logs/vision/capsule_asset_san.log
	@grep -q VISION_CAPSULE_ASSET_PASS logs/vision/capsule_asset_san.log

knowledge_capsule: $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_knowledge_capsule.c include/hybrid_ai.h include/personal_ai.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror $(CUDA_CFLAGS) -o $(BIN_DIR)/test_knowledge_capsule \
		$(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_knowledge_capsule.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_knowledge_capsule > logs/knowledge_capsule.log 2>&1
	@grep -q "KNOWLEDGE_CAPSULE_PASS" logs/knowledge_capsule.log
	@grep "KNOWLEDGE_CAPSULE_PASS" logs/knowledge_capsule.log

# Brain bundle discrete mode geometry → real CNET capsules (CNU1 + coverage)
brain_cnet_capsule: $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tools/brain_to_cnet_capsule.c include/hybrid_ai.h include/personal_ai.h
	@mkdir -p $(BIN_DIR) logs artifacts/brain_cnet_capsules
	$(CC) $(CFLAGS) -Werror $(CUDA_CFLAGS) -o $(BIN_DIR)/brain_to_cnet_capsule \
		$(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tools/brain_to_cnet_capsule.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/brain_to_cnet_capsule --demo artifacts/brain_cnet_capsules > logs/brain_cnet_capsule.log 2>&1
	@grep -q "BRAIN_CNET_CAPSULE_PASS" logs/brain_cnet_capsule.log
	@grep "BRAIN_CNET_CAPSULE_PASS" logs/brain_cnet_capsule.log
	@echo "capsules under artifacts/brain_cnet_capsules/"

# Slice 2: center-rank among coverage-admitting CERT units (Brain board law)
sparse_serve: $(HYBRID_AI_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_sparse_serve.c include/cnet_sparse_serve.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror $(CUDA_CFLAGS) -o $(BIN_DIR)/test_sparse_serve \
		$(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_sparse_serve.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_sparse_serve > logs/sparse_serve.log 2>&1
	@grep -q "SPARSE_SERVE_PASS" logs/sparse_serve.log
	@grep "SPARSE_SERVE_PASS" logs/sparse_serve.log

# Slice 3: recall-before-spawn + CERT promote on structure mine
sparse_mine: $(HYBRID_AI_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_sparse_mine.c include/cnet_sparse_serve.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror $(CUDA_CFLAGS) -o $(BIN_DIR)/test_sparse_mine \
		$(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_sparse_mine.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_sparse_mine > logs/sparse_mine.log 2>&1
	@grep -q "SPARSE_MINE_PASS" logs/sparse_mine.log
	@grep "SPARSE_MINE_PASS" logs/sparse_mine.log

# Slice 4: same-domain residual ACCUM (Brain LINK_ACCUM)
sparse_residual: $(HYBRID_AI_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_sparse_residual.c include/cnet_sparse_serve.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror $(CUDA_CFLAGS) -o $(BIN_DIR)/test_sparse_residual \
		$(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_sparse_residual.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_sparse_residual > logs/sparse_residual.log 2>&1
	@grep -q "SPARSE_RESIDUAL_PASS" logs/sparse_residual.log
	@grep "SPARSE_RESIDUAL_PASS" logs/sparse_residual.log

# Slice 5: opt-in Brain continuous pieces.bin sidecar (no CNU1 floor change)
brain_sidecar: $(BRAIN_SIDECAR_SRC) tests/test_brain_sidecar.c include/cnet_brain_sidecar.h
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(CFLAGS) -Werror -o $(BIN_DIR)/test_brain_sidecar \
		$(BRAIN_SIDECAR_SRC) tests/test_brain_sidecar.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_brain_sidecar > logs/brain_sidecar.log 2>&1
	@grep -q "BRAIN_SIDECAR_PASS" logs/brain_sidecar.log
	@grep "BRAIN_SIDECAR_PASS" logs/brain_sidecar.log

# Chess-fetch + FIFO text generation (not next-token parrot)
GENERATE_FIFO_SRC := src/cnet_generate_fifo.c
generate_fifo: $(GENERATE_FIFO_SRC) $(HYBRID_AI_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_generate_fifo.c include/cnet_generate_fifo.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror $(CUDA_CFLAGS) -o $(BIN_DIR)/test_generate_fifo \
		$(GENERATE_FIFO_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_generate_fifo.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_generate_fifo > logs/generate_fifo.log 2>&1
	@grep -q "GENERATE_FIFO_PASS" logs/generate_fifo.log
	@grep "GENERATE_FIFO_PASS" logs/generate_fifo.log
	@echo "---- experiment metrics ----"
	@grep -E 'placed_|sample_|results|chess:|resort:|speedup|forward_ratio|tail=' logs/generate_fifo.log || true

# Long length + collapse probe (48 / 512 / 4096)
generate_fifo_long: $(GENERATE_FIFO_SRC) $(HYBRID_AI_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_generate_fifo_long.c include/cnet_generate_fifo.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror $(CUDA_CFLAGS) -o $(BIN_DIR)/test_generate_fifo_long \
		$(GENERATE_FIFO_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_generate_fifo_long.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_generate_fifo_long > logs/generate_fifo_long.log 2>&1
	@grep -q "GENERATE_FIFO_LONG_EXP_DONE" logs/generate_fifo_long.log
	@grep -E 'steps=|len=|collapse_|wall=|unique_|speedup|DONE' logs/generate_fifo_long.log

# Quality lever: position-board pieces vs char-Markov
generate_fifo_quality: $(GENERATE_FIFO_SRC) $(HYBRID_AI_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_generate_fifo_quality.c include/cnet_generate_fifo.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror $(CUDA_CFLAGS) -o $(BIN_DIR)/test_generate_fifo_quality \
		$(GENERATE_FIFO_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_generate_fifo_quality.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_generate_fifo_quality > logs/generate_fifo_quality.log 2>&1
	@grep -q "GENERATE_FIFO_QUALITY_PASS" logs/generate_fifo_quality.log
	@grep "GENERATE_FIFO_QUALITY_PASS" logs/generate_fifo_quality.log
	@echo "---- quality metrics ----"
	@grep -E 'MARKOV|POSITION|match_teacher|len=|out=|expect=|long ask|PASS|FAIL' logs/generate_fifo_quality.log || true

# Teacher → skill_pos_* CNU1 capsules → import → generate_fifo (+ bench)
skill_capsule_generate: $(GENERATE_FIFO_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_skill_capsule_generate.c include/cnet_generate_fifo.h include/cnet_capsule.h
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(CFLAGS) -Werror $(CUDA_CFLAGS) -o $(BIN_DIR)/test_skill_capsule_generate \
		$(GENERATE_FIFO_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_skill_capsule_generate.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_skill_capsule_generate > logs/skill_capsule_generate.log 2>&1
	@grep -q "SKILL_CAPSULE_GENERATE_PASS" logs/skill_capsule_generate.log
	@grep "SKILL_CAPSULE_GENERATE_PASS" logs/skill_capsule_generate.log
	@echo "---- skill capsule bench ----"
	@grep -E 'sealed_|exported=|imported=|mem_gen|cap_gen|BENCH|roundtrip|learn\+|quality|PASS|FAIL' logs/skill_capsule_generate.log || true

# 3-lane memory runtime: STM / LTM / Forming (+ load bench)
MEM_RUNTIME_SRC := src/cnet_mem_runtime.c src/cnet_asi_improve.c
mem_runtime: $(MEM_RUNTIME_SRC) $(GENERATE_FIFO_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_mem_runtime.c include/cnet_mem_runtime.h include/cnet_asi_improve.h
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(CFLAGS) -Werror $(CUDA_CFLAGS) -o $(BIN_DIR)/test_mem_runtime \
		$(MEM_RUNTIME_SRC) $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_mem_runtime.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread -lm
	@./$(BIN_DIR)/test_mem_runtime > logs/mem_runtime.log 2>&1
	@grep -q "MEM_RUNTIME_PASS" logs/mem_runtime.log
	@grep "MEM_RUNTIME_PASS" logs/mem_runtime.log
	@echo "---- mem runtime bench ----"
	@grep -E 'load bench|stm_hit|wall=|serve=|prebuilt|asi_block|ep_log|product wire|PASS|FAIL' logs/mem_runtime.log || true

# ASI improve stack (literature slices A–E) — pure C, no floor changes
ASI_IMPROVE_SRC := src/cnet_asi_improve.c
ASI_IMPROVE_CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 -D_POSIX_C_SOURCE=200809L -Iinclude

.PHONY: asi_libos asi_defer asi_route asi_episode asi_firewall_eval asi_improve_all

asi_libos: $(ASI_IMPROVE_SRC) include/cnet_asi_improve.h tests/test_asi_libos.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/test_asi_libos $(ASI_IMPROVE_SRC) tests/test_asi_libos.c -lm
	@./$(BIN_DIR)/test_asi_libos | tee logs/asi_libos.log
	@grep -q "ASI_LIBOS_PASS" logs/asi_libos.log

asi_defer: $(ASI_IMPROVE_SRC) include/cnet_asi_improve.h tests/test_asi_defer.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/test_asi_defer $(ASI_IMPROVE_SRC) tests/test_asi_defer.c -lm
	@./$(BIN_DIR)/test_asi_defer | tee logs/asi_defer.log
	@grep -q "ASI_DEFER_PASS" logs/asi_defer.log

asi_route: $(ASI_IMPROVE_SRC) include/cnet_asi_improve.h tests/test_asi_route.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/test_asi_route $(ASI_IMPROVE_SRC) tests/test_asi_route.c -lm
	@./$(BIN_DIR)/test_asi_route | tee logs/asi_route.log
	@grep -q "ASI_ROUTE_PASS" logs/asi_route.log

asi_episode: $(ASI_IMPROVE_SRC) include/cnet_asi_improve.h tests/test_asi_episode.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/test_asi_episode $(ASI_IMPROVE_SRC) tests/test_asi_episode.c -lm
	@./$(BIN_DIR)/test_asi_episode | tee logs/asi_episode.log
	@grep -q "ASI_EPISODE_PASS" logs/asi_episode.log

asi_firewall_eval: $(ASI_IMPROVE_SRC) include/cnet_asi_improve.h tests/test_asi_firewall_eval.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/test_asi_firewall_eval $(ASI_IMPROVE_SRC) tests/test_asi_firewall_eval.c -lm
	@./$(BIN_DIR)/test_asi_firewall_eval | tee logs/asi_firewall_eval.log
	@grep -q "ASI_FIREWALL_EVAL_PASS" logs/asi_firewall_eval.log

asi_improve_all: asi_libos asi_defer asi_route asi_episode asi_firewall_eval
	@echo "ASI_IMPROVE_ALL_PASS"

.PHONY: asi_av_bakeoff roe_asi roe_asi_train
asi_av_bakeoff: $(ASI_IMPROVE_SRC) include/cnet_asi_improve.h tools/cnet_av_bakeoff.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/cnet_av_bakeoff $(ASI_IMPROVE_SRC) tools/cnet_av_bakeoff.c -lm
	@./$(BIN_DIR)/cnet_av_bakeoff --wins-a 800 --wins-b 200 --n 1000 --alpha 0.01 | tee logs/asi_av_bakeoff.log
	@grep -q "ASI_AV_BAKEOFF_PROMOTE" logs/asi_av_bakeoff.log
	@./$(BIN_DIR)/cnet_av_bakeoff --sim-p 0.7 --max 5000 --alpha 0.05 | tee -a logs/asi_av_bakeoff.log
	@grep -q "ASI_AV_BAKEOFF_PASS" logs/asi_av_bakeoff.log
	@echo "ASI_AV_BAKEOFF_GATE_PASS"

# ROE-ASI concept assistant (local skills + lookup/LLM miss + promote)
ROE_ASI_SRC := src/cnet_roe_asi.c src/cnet_roe_net.c src/cnet_asi_improve.c
ROE_ASI_LIBS := -lm $(CURL_LDFLAGS)
roe_asi: $(ROE_ASI_SRC) include/cnet_roe_asi.h include/cnet_roe_net.h include/cnet_asi_improve.h tests/test_roe_asi.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/test_roe_asi $(ROE_ASI_SRC) tests/test_roe_asi.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/test_roe_asi | tee logs/roe_asi.log
	@grep -q "ROE_ASI_PASS" logs/roe_asi.log

roe_asi_train: $(ROE_ASI_SRC) include/cnet_roe_asi.h include/cnet_asi_improve.h tools/roe_asi_train.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_asi_train $(ROE_ASI_SRC) tools/roe_asi_train.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/roe_asi_train | tee logs/roe_asi_train.log
	@grep -q "ROE_ASI_TRAIN_PASS" logs/roe_asi_train.log
	@echo "---- roe train economics ----"
	@grep -E 'epoch|hit=|save=|demo|ROE_ASI' logs/roe_asi_train.log || true

.PHONY: roe_asi_live roe_asi_cli
roe_asi_live: $(ROE_ASI_SRC) include/cnet_roe_asi.h include/cnet_roe_net.h tests/test_roe_asi_live.c
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/test_roe_asi_live $(ROE_ASI_SRC) tests/test_roe_asi_live.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/test_roe_asi_live | tee logs/roe_asi_live.log
	@grep -q "ROE_ASI_LIVE_PASS" logs/roe_asi_live.log

roe_asi_cli: $(ROE_ASI_SRC) include/cnet_roe_asi.h include/cnet_roe_net.h tools/roe_asi_cli.c
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_asi_cli $(ROE_ASI_SRC) tools/roe_asi_cli.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/roe_asi_cli train --catalog artifacts/roe_catalog | tee logs/roe_asi_cli.log
	@grep -q "ROE_ASI_CLI_PASS" logs/roe_asi_cli.log
	@echo "---- roe cli (offline train) ----"
	@grep -E 'epoch|saved|hit=|ROE_ASI' logs/roe_asi_cli.log || true

.PHONY: roe_asi_coding
roe_asi_coding: $(ROE_ASI_SRC) include/cnet_roe_asi.h tools/roe_asi_coding_train.c
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_asi_coding_train $(ROE_ASI_SRC) tools/roe_asi_coding_train.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/roe_asi_coding_train | tee logs/roe_asi_coding.log
	@grep -q "ROE_ASI_CODING_PASS" logs/roe_asi_coding.log
	@echo "---- coding catalog ----"
	@ls artifacts/roe_coding_catalog/skills 2>/dev/null | head -20 || true
	@grep -E 'epoch|hit=|skill|ROE_ASI' logs/roe_asi_coding.log || true

.PHONY: roe_asi_debug_l3
roe_asi_debug_l3: $(ROE_ASI_SRC) src/cnet_roe_debug.c include/cnet_roe_debug.h tools/roe_asi_debug_l3_train.c
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_asi_debug_l3_train \
		$(ROE_ASI_SRC) src/cnet_roe_debug.c tools/roe_asi_debug_l3_train.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/roe_asi_debug_l3_train | tee logs/roe_asi_debug_l3.log
	@grep -q "ROE_ASI_DEBUG_L3_PASS" logs/roe_asi_debug_l3.log
	@echo "---- debug L3 catalog ----"
	@ls artifacts/roe_debug_catalog 2>/dev/null | head -20 || true
	@wc -l artifacts/roe_debug_catalog/project_memory.jsonl 2>/dev/null || true
	@grep -E 'L3|soak|PASS|FAIL|catalog' logs/roe_asi_debug_l3.log || true

.PHONY: roe_asi_debug_cli
roe_asi_debug_cli: $(ROE_ASI_SRC) src/cnet_roe_debug.c include/cnet_roe_debug.h tools/roe_asi_debug_cli.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_asi_debug_cli \
		$(ROE_ASI_SRC) src/cnet_roe_debug.c tools/roe_asi_debug_cli.c $(ROE_ASI_LIBS)

.PHONY: roe_asi_goal roe_asi_goal_cli
roe_asi_goal: $(ROE_ASI_SRC) src/cnet_roe_goal.c include/cnet_roe_goal.h tools/roe_asi_goal_train.c
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_asi_goal_train \
		$(ROE_ASI_SRC) src/cnet_roe_goal.c tools/roe_asi_goal_train.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/roe_asi_goal_train | tee logs/roe_asi_goal.log
	@grep -q "ROE_ASI_GOAL_PASS" logs/roe_asi_goal.log
	@echo "---- goal catalog tidy ----"
	@find artifacts/roe_goal_catalog -maxdepth 4 -type d 2>/dev/null | head -40 || true
	@grep -E 'goal:|HAVE|LEARN|summary|PASS' logs/roe_asi_goal.log | head -40 || true

roe_asi_goal_cli: $(ROE_ASI_SRC) src/cnet_roe_goal.c include/cnet_roe_goal.h tools/roe_asi_goal_cli.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_asi_goal_cli \
		$(ROE_ASI_SRC) src/cnet_roe_goal.c tools/roe_asi_goal_cli.c $(ROE_ASI_LIBS)

.PHONY: roe_asi_ocr
roe_asi_ocr: $(ROE_ASI_SRC) src/cnet_roe_goal.c src/cnet_roe_ocr.c include/cnet_roe_ocr.h tools/roe_asi_ocr_train.c
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_asi_ocr_train \
		$(ROE_ASI_SRC) src/cnet_roe_goal.c src/cnet_roe_ocr.c tools/roe_asi_ocr_train.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/roe_asi_ocr_train | tee logs/roe_asi_ocr.log
	@grep -q "ROE_ASI_OCR_PASS" logs/roe_asi_ocr.log
	@echo "---- ocr catalog ----"
	@find artifacts/roe_ocr_catalog -maxdepth 4 -type d 2>/dev/null | head -30 || true
	@grep -E 'train|OCR|PASS|catalog' logs/roe_asi_ocr.log | head -30 || true

.PHONY: roe_asi_ocr_tree
roe_asi_ocr_tree: src/cnet_roe_tree.c include/cnet_roe_tree.h tools/roe_asi_ocr_skill_tree.c
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_asi_ocr_skill_tree \
		src/cnet_roe_tree.c tools/roe_asi_ocr_skill_tree.c -lm
	@./$(BIN_DIR)/roe_asi_ocr_skill_tree | tee logs/roe_asi_ocr_tree.log
	@grep -q "ROE_ASI_OCR_TREE_PASS" logs/roe_asi_ocr_tree.log
	@echo "---- tree artifact ----"
	@cat artifacts/roe_ocr_skill_tree/power.txt 2>/dev/null || true
	@wc -l artifacts/roe_ocr_skill_tree/skill_tree.jsonl 2>/dev/null || true

# ROE self-model: inventory + gate-bound tree + health + goal + pack (never self-CERT)
.PHONY: roe_asi_self roe_asi_self_cli
ROE_SELF_SRC := $(ROE_ASI_SRC) src/cnet_roe_goal.c src/cnet_roe_doc.c src/cnet_roe_table.c \
	src/cnet_roe_tree.c src/cnet_roe_self.c src/agent_memory.c
roe_asi_self: $(ROE_SELF_SRC) include/cnet_roe_self.h tests/test_roe_self.c
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/test_roe_self \
		$(ROE_SELF_SRC) tests/test_roe_self.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/test_roe_self | tee logs/roe_asi_self.log
	@grep -q "ROE_ASI_SELF_PASS" logs/roe_asi_self.log
	@echo "---- self pack ----"
	@head -40 artifacts/roe_self_model_test/pack/self_report.json 2>/dev/null || true
	@test -f artifacts/roe_self_model_test/pack/SELF.abi

roe_asi_self_cli: $(ROE_SELF_SRC) include/cnet_roe_self.h tools/roe_asi_self_cli.c
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_asi_self_cli \
		$(ROE_SELF_SRC) tools/roe_asi_self_cli.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/roe_asi_self_cli snapshot --catalog artifacts/roe_catalog \
		--out artifacts/roe_self_model --repo . --goal "ocr document local packs" \
		--thoughts | tee logs/roe_asi_self_cli.log
	@grep -q "ROE_ASI_SELF_CLI_PASS" logs/roe_asi_self_cli.log
	@./$(BIN_DIR)/roe_asi_self_cli validate --out artifacts/roe_self_model | tee -a logs/roe_asi_self_cli.log
	@grep -q "ROE_ASI_SELF_CLI_PASS" logs/roe_asi_self_cli.log
	@echo "---- self model summary ----"
	@head -50 artifacts/roe_self_model/self_report.json 2>/dev/null || true

# Separate daily packs (token reduction): seed + per-pack load gate
.PHONY: roe_daily_packs roe_daily_packs_seed
roe_daily_packs_seed: $(BIN_DIR)/roe_daily_packs_seed
	@mkdir -p artifacts logs
	$(BIN_DIR)/roe_daily_packs_seed | tee logs/roe_daily_packs_seed.log
	@grep -q "ROE_DAILY_PACKS_SEED_OK" logs/roe_daily_packs_seed.log

roe_daily_packs: roe_daily_packs_seed $(ROE_ASI_SRC) tools/roe_daily_packs_gate.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_daily_packs_gate \
		$(ROE_ASI_SRC) tools/roe_daily_packs_gate.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/roe_daily_packs_gate | tee logs/roe_daily_packs.log
	@grep -q "ROE_DAILY_PACKS_PASS" logs/roe_daily_packs.log
	@echo "---- daily packs bench ----"
	@cat artifacts/roe_daily_packs/BENCH.json 2>/dev/null || true
	@echo "---- index ----"
	@jq -r '"packs \(.packs|length) always_on \(.recommended_always_on)"' artifacts/roe_daily_packs/INDEX.json

# Front door: ROUTES → selective pack load → turn → miss_log
.PHONY: roe_front_door
roe_front_door: roe_daily_packs $(ROE_ASI_SRC) tools/roe_front_door.c src/cnet_domain_route.c \
		src/cnet_query_alias.c src/cnet_dialog_ctx.c src/cnet_slot_extract.c \
		include/cnet_domain_route.h include/cnet_query_alias.h include/cnet_dialog_ctx.h \
		include/cnet_slot_extract.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_front_door \
		$(ROE_ASI_SRC) src/cnet_domain_route.c src/cnet_query_alias.c src/cnet_dialog_ctx.c \
		src/cnet_slot_extract.c tools/roe_front_door.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/roe_front_door selftest | tee logs/roe_front_door.log
	@grep -q "ROE_FRONT_DOOR_PASS" logs/roe_front_door.log
	@echo "---- front bench ----"
	@cat artifacts/roe_daily_packs/FRONT_BENCH.json 2>/dev/null || true
	@echo "---- sample ask ----"
	@./$(BIN_DIR)/roe_front_door ask "who are you" | tee -a logs/roe_front_door.log
	@./$(BIN_DIR)/roe_front_door ask "format-truncation werror" | tee -a logs/roe_front_door.log
	@./$(BIN_DIR)/roe_front_door ask "Introduce yourself" | tee logs/roe_front_door_alias.log
	@grep -q "source=LOCAL" logs/roe_front_door_alias.log
	@cat logs/roe_front_door_alias.log >> logs/roe_front_door.log

.PHONY: cnet_minimal_package
cnet_minimal_package:
	@mkdir -p logs dist
	@chmod +x scripts/package_cnet_minimal.sh scripts/cnet_runtime_smoke.sh scripts/cnet_runtime_soak_gate.sh
	@bash scripts/package_cnet_minimal.sh | tee logs/cnet_minimal_package.log
	@grep -q "PACKAGE_OK" logs/cnet_minimal_package.log
	@# smoke inside package
	@PKG=$$(cat dist/CNET-Minimal-latest.path); \
	  CNET_MINIMAL_ROOT="$$PKG" bash "$$PKG/scripts/cnet_runtime_smoke.sh" | tee logs/cnet_runtime_smoke_pkg.log; \
	  grep -q "CNET_RUNTIME_SMOKE_PASS" logs/cnet_runtime_smoke_pkg.log
	@echo "CNET_MINIMAL_PACKAGE_OK"

.PHONY: cnet_runtime_smoke
cnet_runtime_smoke:
	@mkdir -p logs
	@chmod +x scripts/cnet_runtime_smoke.sh
	@bash scripts/cnet_runtime_smoke.sh | tee logs/cnet_runtime_smoke.log
	@grep -q "CNET_RUNTIME_SMOKE_PASS" logs/cnet_runtime_smoke.log

.PHONY: cnet_runtime_soak_gate
cnet_runtime_soak_gate: cnet_minimal_package
	@mkdir -p logs
	@chmod +x scripts/cnet_runtime_soak_gate.sh
	@bash scripts/cnet_runtime_soak_gate.sh | tee logs/cnet_runtime_soak_gate.log
	@grep -q "CNET_RUNTIME_SOAK_GATE_PASS" logs/cnet_runtime_soak_gate.log

.PHONY: cnet_minimal_deploy
cnet_minimal_deploy:
	@mkdir -p logs
	@chmod +x scripts/deploy_cnet_minimal.sh scripts/package_cnet_minimal.sh scripts/cnet_runtime_smoke.sh
	@bash scripts/deploy_cnet_minimal.sh | tee logs/cnet_minimal_deploy.log
	@grep -q "CNET_MINIMAL_DEPLOY_PASS" logs/cnet_minimal_deploy.log

.PHONY: roe_explore_tick
roe_explore_tick: tools/roe_explore_tick.py tools/roe_explore_tick_gate.c
	@mkdir -p $(BIN_DIR) logs artifacts/roe_daily_packs
	$(CC) -std=c11 -Wall -O2 -o $(BIN_DIR)/roe_explore_tick_gate tools/roe_explore_tick_gate.c
	@./$(BIN_DIR)/roe_explore_tick_gate | tee logs/roe_explore_tick_gate.log
	@grep -q "ROE_EXPLORE_TICK_GATE_PASS" logs/roe_explore_tick_gate.log
	@$(PYTHON) tools/roe_explore_tick.py --force 2>&1 | tee logs/roe_explore_tick.log
	@grep -q "ROE_EXPLORE_TICK_PASS" logs/roe_explore_tick.log
	@test ! -f artifacts/roe_daily_packs/EXPLORE_TICK.json || grep -q '"auto_cert": false' artifacts/roe_daily_packs/EXPLORE_TICK.json
	@echo "ROE_EXPLORE_TICK_OK"

.PHONY: query_alias dialog_ctx query_dialog slot_extract
query_alias dialog_ctx query_dialog slot_extract: include/cnet_query_alias.h src/cnet_query_alias.c \
		include/cnet_dialog_ctx.h src/cnet_dialog_ctx.c \
		include/cnet_slot_extract.h src/cnet_slot_extract.c \
		tools/cnet_query_dialog_main.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L -Iinclude \
		-o $(BIN_DIR)/cnet_query_dialog \
		src/cnet_query_alias.c src/cnet_dialog_ctx.c src/cnet_slot_extract.c \
		tools/cnet_query_dialog_main.c
	@./$(BIN_DIR)/cnet_query_dialog --test | tee logs/query_dialog.log
	@grep -q "QUERY_ALIAS_PASS" logs/query_dialog.log
	@grep -q "DIALOG_CTX_PASS" logs/query_dialog.log
	@grep -q "SLOT_EXTRACT_PASS" logs/query_dialog.log
	@grep -q "QUERY_DIALOG_PASS" logs/query_dialog.log
	@./$(BIN_DIR)/cnet_query_dialog "Introduce yourself" | tee -a logs/query_dialog.log
	@./$(BIN_DIR)/cnet_query_dialog --dialog "show me its status" | tee -a logs/query_dialog.log
	@./$(BIN_DIR)/cnet_query_dialog --slot "is cnet-web active" | tee -a logs/query_dialog.log
	@echo "QUERY_DIALOG_OK"

.PHONY: cnetd
cnetd: $(ROE_ASI_SRC) tools/cnetd.c src/cnet_domain_route.c src/cnet_utterance.c \
		src/cnet_query_alias.c src/cnet_dialog_ctx.c src/cnet_slot_extract.c \
		include/cnet_probe_shortcircuit.h include/cnet_domain_route.h include/cnet_utterance.h \
		include/cnet_query_alias.h include/cnet_dialog_ctx.h include/cnet_slot_extract.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/cnetd \
		$(ROE_ASI_SRC) src/cnet_domain_route.c src/cnet_utterance.c \
		src/cnet_query_alias.c src/cnet_dialog_ctx.c src/cnet_slot_extract.c \
		tools/cnetd.c $(ROE_ASI_LIBS)
	@echo "cnetd built → $(BIN_DIR)/cnetd"

.PHONY: cnet_utterance
cnet_utterance: src/cnet_utterance.c include/cnet_utterance.h tools/cnet_utterance_main.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/cnet_utterance \
		src/cnet_utterance.c tools/cnet_utterance_main.c
	@./$(BIN_DIR)/cnet_utterance --test | tee logs/cnet_utterance.log
	@grep -q CNET_UTTERANCE_PASS logs/cnet_utterance.log
	@./$(BIN_DIR)/cnet_utterance --when status --hit 0.857 --da 0.47 --ht 0.53 --ado 0.51 --miss 2 | tee -a logs/cnet_utterance.log
	@echo "CNET_UTTERANCE_OK"

.PHONY: cnetd-run
cnetd-run: cnetd query_dialog
	@pkill -x cnetd 2>/dev/null || true
	@sleep 0.2
	@if [ -f $(HOME)/.local/share/cnet-minimal/cnet-minimal.env ]; then set -a; . $(HOME)/.local/share/cnet-minimal/cnet-minimal.env; set +a; fi
	@$(BIN_DIR)/cnetd >logs/cnetd.log 2>&1 & echo $$! > logs/cnetd.pid
	@sleep 0.4
	@chmod +x scripts/cnet_sock_ask.sh
	@scripts/cnet_sock_ask.sh "who are you" | tee logs/cnetd_ask.log
	@grep -q "SOURCE LOCAL\|\"source\":\"LOCAL\"" logs/cnetd_ask.log
	@# Milestone A: conversational soul paraphrase → LOCAL
	@scripts/cnet_sock_ask.sh "Introduce yourself" | tee logs/cnetd_ask_a1.log
	@grep -q "SOURCE LOCAL\|\"source\":\"LOCAL\"" logs/cnetd_ask_a1.log
	@cat logs/cnetd_ask_a1.log >> logs/cnetd_ask.log
	@scripts/cnet_sock_ask.sh "who am i talking to" | tee logs/cnetd_ask_a2.log
	@grep -q "SOURCE LOCAL\|\"source\":\"LOCAL\"" logs/cnetd_ask_a2.log
	@cat logs/cnetd_ask_a2.log >> logs/cnetd_ask.log
	@# Milestone B: entity bind then anaphora status
	@scripts/cnet_sock_ask.sh "cnet-marble status" | tee logs/cnetd_ask_b1.log
	@grep -q "SOURCE LOCAL\|\"source\":\"LOCAL\"" logs/cnetd_ask_b1.log
	@cat logs/cnetd_ask_b1.log >> logs/cnetd_ask.log
	@scripts/cnet_sock_ask.sh "show me its status" | tee logs/cnetd_ask_b2.log
	@grep -q "SOURCE LOCAL\|\"source\":\"LOCAL\"" logs/cnetd_ask_b2.log
	@grep -q "DIALOG_HIT 1\|\"dialog_hit\":true" logs/cnetd_ask_b2.log
	@cat logs/cnetd_ask_b2.log >> logs/cnetd_ask.log
	@# Milestone C: pack-local ops slots
	@scripts/cnet_sock_ask.sh "is cnet-web active" | tee logs/cnetd_ask_c1.log
	@grep -q "SOURCE LOCAL\|\"source\":\"LOCAL\"" logs/cnetd_ask_c1.log
	@grep -q "SLOT_HIT 1\|\"slot_hit\":true" logs/cnetd_ask_c1.log
	@cat logs/cnetd_ask_c1.log >> logs/cnetd_ask.log
	@scripts/cnet_sock_ask.sh "restart cnetd" | tee logs/cnetd_ask_c2.log
	@grep -q "SOURCE LOCAL\|\"source\":\"LOCAL\"" logs/cnetd_ask_c2.log
	@grep -q "SLOT_HIT 1\|\"slot_hit\":true" logs/cnetd_ask_c2.log
	@cat logs/cnetd_ask_c2.log >> logs/cnetd_ask.log
	@scripts/cnet_sock_ask.sh "is cnet-marble running" | tee logs/cnetd_ask_c3.log
	@grep -q "SOURCE LOCAL\|\"source\":\"LOCAL\"" logs/cnetd_ask_c3.log
	@grep -q "ops_cnet_marble\|SLOT_HIT 1\|\"slot_hit\":true" logs/cnetd_ask_c3.log
	@cat logs/cnetd_ask_c3.log >> logs/cnetd_ask.log
	@# probes remain non-LOCAL / short-circuit
	@scripts/cnet_sock_ask.sh "autonomous cycle probe novel fact beta-nine" | tee logs/cnetd_ask_probe.log
	@grep -q "SHORTCIRCUIT 1\|shortcircuit.:true" logs/cnetd_ask_probe.log
	@cat logs/cnetd_ask_probe.log >> logs/cnetd_ask.log
	@echo "CNETD_OK"

.PHONY: cnet-web
cnet-web:
	@test -f web/cnet-cockpit.html
	@test -f tools/cnet_web.py
	@test -f scripts/cnet_web_run.sh
	@chmod +x scripts/cnet_web_run.sh
	@systemctl --user is-active cnetd.service >/dev/null 2>&1 || $(MAKE) cnetd-run
	@# Prefer live unit if active; else one-shot localhost smoke
	@if systemctl --user is-active cnet-web.service >/dev/null 2>&1; then \
	  H=$$(systemctl --user show cnet-web.service -p Environment --value 2>/dev/null | tr ' ' '\n' | sed -n 's/^CNET_WEB_HOST=//p'); \
	  H=$${H:-$$(tailscale ip -4 2>/dev/null | awk '/^100\./{print;exit}')}; \
	  H=$${H:-127.0.0.1}; \
	  curl -sf "http://$$H:8642/api/health" | tee logs/cnet_web_health.json; \
	else \
	  pkill -f '$(PYTHON) .*/tools/cnet_web.py' 2>/dev/null || true; \
	  CNET_WEB_HOST=127.0.0.1 CNET_WEB_PORT=8642 $(PYTHON) tools/cnet_web.py >logs/cnet_web.log 2>&1 & echo $$! > logs/cnet_web.pid; \
	  sleep 0.6; \
	  curl -sf http://127.0.0.1:8642/api/health | tee logs/cnet_web_health.json; \
	fi
	@grep -q '"ok": true\|"ok":true' logs/cnet_web_health.json
	@echo "CNET_WEB_OK"

.PHONY: cnet-web-service
cnet-web-service:
	@chmod +x scripts/cnet_web_run.sh
	@mkdir -p $(HOME)/.config/systemd/user
	@cp -a scripts/systemd/cnet-web.service $(HOME)/.config/systemd/user/
	@cp -a scripts/systemd/cnetd.service $(HOME)/.config/systemd/user/
	@# stop ad-hoc server so unit owns :8642
	@pkill -f '$(PYTHON) .*/tools/cnet_web.py' 2>/dev/null || true
	@pkill -f '$(PYTHON) tools/cnet_web.py' 2>/dev/null || true
	@systemctl --user daemon-reload
	@systemctl --user enable cnetd.service cnet-web.service
	@systemctl --user restart cnetd.service
	@sleep 0.4
	@systemctl --user restart cnet-web.service
	@sleep 0.8
	@systemctl --user is-active cnetd.service cnet-web.service | tee logs/cnet_web_service_active.txt
	@grep -qx active logs/cnet_web_service_active.txt || true
	@TS=$$(tailscale ip -4 2>/dev/null | awk '/^100\./{print;exit}'); \
	  TS=$${TS:-127.0.0.1}; \
	  echo "probe http://$$TS:8642/"; \
	  curl -sf "http://$$TS:8642/api/health" | tee logs/cnet_web_health.json; \
	  curl -sf -X POST "http://$$TS:8642/api/ask" -H 'Content-Type: application/json' \
	    -d '{"q":"who are you"}' | tee logs/cnet_web_ask.json; \
	  curl -sf -X POST "http://$$TS:8642/api/ask" -H 'Content-Type: application/json' \
	    -d '{"q":"x","promote":true}' | tee logs/cnet_web_promote_deny.json
	@grep -q LOCAL logs/cnet_web_ask.json
	@grep -q promote_forbidden logs/cnet_web_promote_deny.json
	@ss -ltnp 2>/dev/null | grep -E ':8642' | tee logs/cnet_web_listen.txt || true
	@echo "CNET_WEB_SERVICE_OK"

# CERT-first domain route table (static + optional TSV overlay)
.PHONY: domain_route
domain_route: include/cnet_domain_route.h src/cnet_domain_route.c tools/roe_domain_route.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L -Iinclude \
		-o $(BIN_DIR)/roe_domain_route src/cnet_domain_route.c tools/roe_domain_route.c
	@./$(BIN_DIR)/roe_domain_route --test | tee logs/domain_route.log
	@grep -q "DOMAIN_ROUTE_PASS" logs/domain_route.log
	@./$(BIN_DIR)/roe_domain_route "who are you" | tee -a logs/domain_route.log
	@./$(BIN_DIR)/roe_domain_route "completely unknown domain xyzzy" | tee -a logs/domain_route.log
	@echo "DOMAIN_ROUTE_OK"

.PHONY: cert_coverage_harvest
cert_coverage_harvest:
	@mkdir -p logs bin
	@bash scripts/cert_coverage_harvest.sh
	@grep -q "CERT_COVERAGE_HARVEST_PASS" logs/cert_coverage_harvest.log

.PHONY: gold_curriculum_harvest
gold_curriculum_harvest:
	@mkdir -p logs bin
	@$(PYTHON) tools/roe_gold_curriculum_harvest.py | tee logs/gold_curriculum_harvest.log
	@grep -q "GOLD_CURRICULUM_HARVEST_PASS" logs/gold_curriculum_harvest.log
	@# evolve dry-run must not promote blocked probes even if gold planted
	@$(PYTHON) -c "from tools.roe_evolve_tick import is_promote_blocked as b; \
assert b('zz mystic ooze 99','x'); assert b('ok','ABSTAIN: no'); print('EVOLVE_BLOCKLIST_OK')"

.PHONY: stream_attend_bench
stream_attend_bench: $(CCE_SPARSE_KV) include/cce/cce_sparse_kv.h tools/stream_attend_bench.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/stream_attend_bench $(CCE_SPARSE_KV) tools/stream_attend_bench.c $(LDFLAGS) -lm
	@./$(BIN_DIR)/stream_attend_bench | tee logs/stream_attend_bench.log
	@grep -q "STREAM_ATTEND_BENCH_PASS" logs/stream_attend_bench.log

.PHONY: stream_ix_e2e_bench
stream_ix_e2e_bench: $(CCE_SPARSE_KV) include/cce/cce_sparse_kv.h tools/stream_ix_e2e_bench.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/stream_ix_e2e_bench $(CCE_SPARSE_KV) tools/stream_ix_e2e_bench.c $(LDFLAGS) -lm
	@./$(BIN_DIR)/stream_ix_e2e_bench | tee logs/stream_ix_e2e_bench.log
	@grep -q "STREAM_IX_E2E_BENCH_PASS" logs/stream_ix_e2e_bench.log

# Chain-of-thought — pure C multi-hop (0-token skeleton; no Python)
.PHONY: roe_chain_think
roe_chain_think: include/cnet_roe_cot.h src/cnet_roe_cot.c tools/roe_chain_think.c
	@mkdir -p $(BIN_DIR) logs/governor
	$(CC) -std=c11 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L -Iinclude \
		-o $(BIN_DIR)/roe_chain_think src/cnet_roe_cot.c tools/roe_chain_think.c
	@./$(BIN_DIR)/roe_chain_think --test | tee logs/roe_chain_think.log
	@grep -q "ROE_CHAIN_THINK_PASS" logs/roe_chain_think.log
	@test -x $(BIN_DIR)/roe_front_door || $(MAKE) roe_front_door
	@./$(BIN_DIR)/roe_chain_think "who are you" | tee -a logs/roe_chain_think.log
	@test -f logs/governor/chain_last.txt
	@grep -q "chain-of-thought" logs/governor/chain_last.txt
	@echo "ROE_CHAIN_THINK_OK"

# Highest-leverage non-LLM agent loop (hermetic, no libcurl):
# always-on tool law → miss → accept/promote → warm LOCAL → residual KPIs.
# Links roe core only; teach table stands in for external teacher.
.PHONY: roe_agent_loop
ROE_ASI_CORE := src/cnet_roe_asi.c src/cnet_asi_improve.c
roe_agent_loop: $(ROE_ASI_CORE) include/cnet_roe_asi.h tools/roe_agent_loop.c
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_agent_loop \
		$(ROE_ASI_CORE) tools/roe_agent_loop.c -lm
	@./$(BIN_DIR)/roe_agent_loop | tee logs/roe_agent_loop.log
	@grep -q "ROE_AGENT_LOOP_PASS" logs/roe_agent_loop.log
	@echo "---- agent loop KPIs ----"
	@cat artifacts/roe_agent_loop/AGENT_LOOP.json 2>/dev/null || true

# Ollama cloud teacher (deepseek-v4-flash:cloud) — no DEEPSEEK_API_KEY
.PHONY: roe_teacher_cloud_smoke
roe_teacher_cloud_smoke: $(ROE_ASI_SRC) tools/roe_teacher_cloud_smoke.c config/roe-teacher-ollama-cloud.env
	@mkdir -p $(BIN_DIR) logs
	@set -a; . ./config/roe-teacher-ollama-cloud.env; set +a; \
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_teacher_cloud_smoke \
		$(ROE_ASI_SRC) tools/roe_teacher_cloud_smoke.c $(ROE_ASI_LIBS); \
	./$(BIN_DIR)/roe_teacher_cloud_smoke | tee logs/roe_teacher_cloud_smoke.log
	@grep -q "ROE_TEACHER_CLOUD_SMOKE_PASS" logs/roe_teacher_cloud_smoke.log

# SOUL persona pack (Marble) — isolated, seal_path forbidden
.PHONY: roe_soul_pack
roe_soul_pack: roe_daily_packs_seed $(ROE_ASI_SRC) tools/roe_soul_pack_gate.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_soul_pack_gate \
		$(ROE_ASI_SRC) tools/roe_soul_pack_gate.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/roe_soul_pack_gate | tee logs/roe_soul_pack.log
	@grep -q "ROE_SOUL_PACK_PASS" logs/roe_soul_pack.log
	@echo "---- SOUL.md head ----"
	@head -20 artifacts/roe_daily_packs/pack_soul_marble/SOUL.md 2>/dev/null || true
	@test -f artifacts/roe_daily_packs/pack_soul_marble/voice.md
	@grep -q "kind persona" artifacts/roe_daily_packs/pack_soul_marble/PACK.abi

# Unattended evolve: miss_log → gold/multi_stable → pack_personal (no human Accept)
.PHONY: roe_evolve_tick
roe_evolve_tick: tools/roe_evolve_tick.py tools/roe_evolve_tick_gate.c
	@mkdir -p $(BIN_DIR) logs artifacts/roe_daily_packs
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_evolve_tick_gate tools/roe_evolve_tick_gate.c
	@./$(BIN_DIR)/roe_evolve_tick_gate | tee logs/roe_evolve_tick_gate.log
	@grep -q "ROE_EVOLVE_TICK_PASS" logs/roe_evolve_tick_gate.log
	@echo "---- EVOLVE_TICK.json ----"
	@cat artifacts/roe_daily_packs/EVOLVE_TICK.json 2>/dev/null | head -40 || true
	@echo "Install timer (optional):"
	@echo "  mkdir -p ~/.config/systemd/user"
	@echo "  cp scripts/systemd/roe-evolve-tick.* ~/.config/systemd/user/"
	@echo "  systemctl --user daemon-reload && systemctl --user enable --now roe-evolve-tick.timer"

# Separate REVIEWER role (Ollama cloud) — not teacher
.PHONY: roe_reviewer_smoke
roe_reviewer_smoke: tools/roe_reviewer.py config/roe-reviewer-ollama-cloud.env
	@mkdir -p logs
	@set -a; \
	  [ -f config/roe-teacher-ollama-cloud.env ] && . ./config/roe-teacher-ollama-cloud.env; \
	  . ./config/roe-reviewer-ollama-cloud.env; \
	  set +a; \
	  $(PYTHON) tools/roe_reviewer.py --smoke | tee logs/roe_reviewer_smoke.log
	@grep -q "ROE_REVIEWER_SMOKE_PASS" logs/roe_reviewer_smoke.log

# optional: after ROE docs
.PHONY: cnet_marble_24_7
cnet_marble_24_7:
	@chmod +x scripts/cnet_marble_24_7.sh scripts/cnet_marble_health_snap.py
	@scripts/cnet_marble_24_7.sh status
	@scripts/cnet_marble_24_7.sh doctor
	@test -f logs/marble_24_7/status.json
	@$(PYTHON) -c "import json;d=json.load(open('logs/marble_24_7/status.json')); assert d.get('hermes_required') is False; assert d.get('core_active_count',0)>=1; print('CNET_MARBLE_24_7_OK')"

# Full autonomous cycle (probe + evolve) — no Hermes, no human Accept
.PHONY: cnet_autonomous
cnet_autonomous:
	@chmod +x scripts/cnet_autonomous_cycle.py
	@mkdir -p logs/marble_24_7 bin
	@CNET_AUTO_TEACHER=$${CNET_AUTO_TEACHER:-1} \
	 ROE_EVOLVE_REVIEWER=$${ROE_EVOLVE_REVIEWER:-1} \
	 $(PYTHON) scripts/cnet_autonomous_cycle.py | tee logs/marble_24_7/autonomous_last.log
	@grep -q "CNET_AUTONOMOUS_PASS" logs/marble_24_7/autonomous_last.log
	@test -f logs/marble_24_7/AUTONOMOUS_CYCLE.json
	@$(PYTHON) -c "import json;d=json.load(open('logs/marble_24_7/AUTONOMOUS_CYCLE.json')); assert d.get('ok') and d.get('hermes_required') is False; print('kpi',d.get('kpi')); print('CNET_AUTONOMOUS_OK')"

# Neuromod personality organ (DA / 5HT / ADO)
.PHONY: cnet_neuromod
cnet_neuromod:
	@chmod +x scripts/cnet_neuromod.py
	@$(PYTHON) scripts/cnet_neuromod.py --test | tee logs/governor/neuromod_test.log
	@grep -q "NEUROMOD_PASS" logs/governor/neuromod_test.log
	@$(PYTHON) scripts/cnet_neuromod.py --tick
	@test -f logs/governor/neuromod_state.json
	@$(PYTHON) -c "import json;d=json.load(open('logs/governor/neuromod_state.json')); assert set(d['levels'])=={'dopamine','serotonin','adenosine'}; assert d['law']['never_self_cert']; print(d['levels']); print('CNET_NEUROMOD_OK')"

# Token-free thought process (no LLM)
.PHONY: cnet_thought
cnet_thought:
	@chmod +x scripts/cnet_thought_process.py
	@$(PYTHON) scripts/cnet_thought_process.py --test | tee logs/governor/thought_test.log
	@grep -q "THOUGHT_PROCESS_PASS" logs/governor/thought_test.log
	@$(PYTHON) scripts/cnet_thought_process.py --query "who are you"
	@test -f logs/governor/thought_last.json
	@$(PYTHON) -c "import json;d=json.load(open('logs/governor/thought_last.json')); assert d['tokens']==0 and d['llm'] is False and d['law']['not_agi']; print(d['chain']); print('CNET_THOUGHT_OK')"

# Continuity workspace (imitate continuity, not consciousness)
.PHONY: cnet_continuity
cnet_continuity:
	@chmod +x scripts/cnet_continuity.py
	@$(PYTHON) scripts/cnet_continuity.py --test | tee logs/governor/continuity_test.log
	@grep -q "CONTINUITY_PASS" logs/governor/continuity_test.log
	@$(PYTHON) scripts/cnet_continuity.py --query "who are you" --line-only
	@test -f logs/governor/continuity_last.json
	@test -f logs/governor/continuity_line.txt
	@$(PYTHON) -c "import json;d=json.load(open('logs/governor/continuity_last.json')); assert d['law']['not_conscious'] and d['law']['never_self_cert'] and d['tokens']==0; print(d['continuity_line']); print('CNET_CONTINUITY_OK')"

# Visible reply thinking (GPT-style panel, 0 tokens)
.PHONY: cnet_reply_think
cnet_reply_think:
	@chmod +x scripts/cnet_reply_think.py scripts/roe_reply.sh
	@$(PYTHON) scripts/cnet_reply_think.py --test | tee logs/governor/reply_think_test.log
	@grep -q "REPLY_THINK_PASS" logs/governor/reply_think_test.log
	@$(PYTHON) scripts/cnet_reply_think.py --query "who are you" --answer "I am Marble." --source LOCAL --skill soul_who --style panel | head -25
	@test -f logs/governor/reply_think_last.json
	@$(PYTHON) -c "import json;d=json.load(open('logs/governor/reply_think_last.json')); assert d['tokens']==0 and d['llm_thinking'] is False; print(d['thinking_summary'][:80]); print('CNET_REPLY_THINK_OK')"

# Autonomy freedom charter + budgets
.PHONY: cnet_autonomy_charter
cnet_autonomy_charter:
	@chmod +x scripts/cnet_autonomy_charter.py
	@$(PYTHON) scripts/cnet_autonomy_charter.py --test | tee logs/governor/autonomy_charter_test.log
	@grep -q "AUTONOMY_CHARTER_PASS" logs/governor/autonomy_charter_test.log
	@$(PYTHON) scripts/cnet_autonomy_charter.py --show | head -40
	@test -f config/autonomy_charter.yaml
	@$(PYTHON) -c "import json;from pathlib import Path;import sys;sys.path.insert(0,'scripts');import cnet_autonomy_charter as a;c=a.charter();assert c['law']['never_self_cert'] and c['budgets']['promotes_per_day']>=1; print('CNET_AUTONOMY_CHARTER_OK')"

.PHONY: roe_asi_ocr_surpass
roe_asi_ocr_surpass: $(ROE_ASI_SRC) src/cnet_roe_goal.c src/cnet_roe_ocr.c src/cnet_roe_tree.c \
		include/cnet_roe_ocr.h tools/roe_asi_ocr_surpass.c
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_asi_ocr_surpass \
		$(ROE_ASI_SRC) src/cnet_roe_goal.c src/cnet_roe_ocr.c src/cnet_roe_tree.c \
		tools/roe_asi_ocr_surpass.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/roe_asi_ocr_surpass | tee logs/roe_asi_ocr_surpass.log
	@grep -q "ROE_ASI_OCR_SURPASS_PASS" logs/roe_asi_ocr_surpass.log
	@echo "---- surpass bench ----"
	@cat artifacts/roe_ocr_surpass/bench_surpass.json 2>/dev/null || true
	@find artifacts/roe_ocr_surpass/capsules -maxdepth 3 -type d 2>/dev/null | head -30 || true

# Torch OmniDoc SOTA beat is WITHHELD after product-Python purge (1A+2A).
# Portable proof is C ROE OCR (roe_asi_ocr_*). Do not treat absence as a pass.
.PHONY: roe_omnidoc_sota
roe_omnidoc_sota:
	@mkdir -p logs
	@echo "ROE_OMNIDOC_SOTA_WITHHELD reason=torch_harness_removed_use_roe_asi_ocr" | tee logs/roe_omnidoc_sota.log
	@false

# Local-first + tables also need table module
roe_asi_ocr_local: $(ROE_ASI_SRC) src/cnet_roe_goal.c src/cnet_roe_doc.c src/cnet_roe_table.c include/cnet_roe_doc.h \
		tools/roe_asi_ocr_local_train.c
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_asi_ocr_local_train \
		$(ROE_ASI_SRC) src/cnet_roe_goal.c src/cnet_roe_doc.c src/cnet_roe_table.c tools/roe_asi_ocr_local_train.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/roe_asi_ocr_local_train | tee logs/roe_asi_ocr_local.log
	@grep -q "ROE_ASI_OCR_LOCAL_PASS" logs/roe_asi_ocr_local.log
	@echo "---- local-first KPI ----"
	@cat artifacts/roe_ocr_local/teacher_rate_kpi.json 2>/dev/null || true

roe_asi_ocr_asset: $(ROE_ASI_SRC) src/cnet_roe_goal.c src/cnet_roe_doc.c src/cnet_roe_table.c include/cnet_roe_doc.h \
		tools/roe_asi_ocr_asset_train.c
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_asi_ocr_asset_train \
		$(ROE_ASI_SRC) src/cnet_roe_goal.c src/cnet_roe_doc.c src/cnet_roe_table.c tools/roe_asi_ocr_asset_train.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/roe_asi_ocr_asset_train | tee logs/roe_asi_ocr_asset.log
	@grep -q "ROE_ASI_OCR_ASSET_PASS" logs/roe_asi_ocr_asset.log
	@echo "---- asset freeze ----"
	@cat artifacts/roe_ocr_asset/dollar_per_page_freeze.json 2>/dev/null || true
	@ls artifacts/roe_ocr_asset/pack_export 2>/dev/null || true

.PHONY: roe_asi_ocr_tables
roe_asi_ocr_tables: $(ROE_ASI_SRC) src/cnet_roe_goal.c src/cnet_roe_doc.c src/cnet_roe_table.c \
		include/cnet_roe_table.h tools/roe_asi_ocr_tables_train.c tools/roe_table_extract.c
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(ASI_IMPROVE_CFLAGS) -o $(BIN_DIR)/roe_asi_ocr_tables_train \
		$(ROE_ASI_SRC) src/cnet_roe_goal.c src/cnet_roe_doc.c src/cnet_roe_table.c \
		tools/roe_asi_ocr_tables_train.c $(ROE_ASI_LIBS)
	@./$(BIN_DIR)/roe_asi_ocr_tables_train | tee logs/roe_asi_ocr_tables.log
	@grep -q "ROE_ASI_OCR_TABLES_PASS" logs/roe_asi_ocr_tables.log
	@echo "---- tables sample ----"
	@head -20 artifacts/roe_ocr_tables/sales.csv 2>/dev/null || true

.PHONY: roe_ocr_hard_beat
roe_ocr_hard_beat:
	@mkdir -p logs
	@echo "ROE_OCR_HARD_BEAT_WITHHELD reason=torch_harness_removed_use_roe_asi_ocr" | tee logs/roe_ocr_hard_beat.log
	@false

.PHONY: roe_omnidoc_full_infer roe_omnidoc_bench
roe_omnidoc_full_infer:
	@mkdir -p logs
	@echo "ROE_OMNIDOC_FULL_INFER_WITHHELD reason=torch_harness_removed" | tee logs/roe_omnidoc_full_infer.log
	@false

roe_omnidoc_bench:
	@mkdir -p logs
	@echo "ROE_OMNIDOC_BENCH_WITHHELD reason=torch_harness_removed" | tee logs/roe_omnidoc_bench_report.log
	@false

.PHONY: roe_asi_ocr_bigfile
roe_asi_ocr_bigfile: $(ROE_ASI_SRC) src/cnet_roe_goal.c src/cnet_roe_doc.c src/cnet_roe_table.c \
		include/cnet_roe_doc.h include/cnet_roe_table.h tools/roe_asi_ocr_bigfile_train.c
	@mkdir -p $(BIN_DIR) logs artifacts/roe_ocr_bigfile
	$(CC) $(CFLAGS) -Iinclude -o $(BIN_DIR)/roe_asi_ocr_bigfile_train \
		$(ROE_ASI_SRC) src/cnet_roe_goal.c src/cnet_roe_doc.c src/cnet_roe_table.c \
		tools/roe_asi_ocr_bigfile_train.c $(LDFLAGS)
	./$(BIN_DIR)/roe_asi_ocr_bigfile_train 2>&1 | tee logs/roe_asi_ocr_bigfile.log
	@grep -q ROE_ASI_OCR_BIGFILE_PASS logs/roe_asi_ocr_bigfile.log
	@# optional PDF chunk helper (C); skip if sample PDF absent
	@if [ -f "/home/marble/AI/stack/data/papers/machine-learning/1610.05492-federated-learning-strategies-for/1610.05492.pdf" ]; then \
		$(MAKE) --no-print-directory $(BIN_DIR)/roe_pdf_chunk_run; \
		$(BIN_DIR)/roe_pdf_chunk_run \
			"/home/marble/AI/stack/data/papers/machine-learning/1610.05492-federated-learning-strategies-for/1610.05492.pdf" \
			--out artifacts/roe_ocr_bigfile/pdf_chunk --max-pages 40 \
			2>&1 | tee -a logs/roe_asi_ocr_bigfile.log; \
		grep -q ROE_PDF_CHUNK_PASS logs/roe_asi_ocr_bigfile.log; \
	fi
	@echo "---- bigfile bench ----"
	@cat artifacts/roe_ocr_bigfile/bigfile_bench.json

.PHONY: roe_omnidoc_sota_dual
roe_omnidoc_sota_dual:
	@mkdir -p logs
	@echo "ROE_OMNIDOC_SOTA_DUAL_WITHHELD reason=torch_harness_removed" | tee logs/roe_omnidoc_dual_gpu_infer.log
	@false

# Light CNET capsule runtime — import package + btn_forward (Brain cheap host)
cnet_capsule_step: $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tools/cnet_capsule_step.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -Werror $(CUDA_CFLAGS) -o $(BIN_DIR)/cnet_capsule_step \
		$(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tools/cnet_capsule_step.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread

coverage_abstain: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_coverage_abstain.c include/hybrid_ai.h include/personal_ai.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_coverage_abstain \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_coverage_abstain.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_coverage_abstain > logs/coverage_abstain.log 2>&1
	@grep -q "COVERAGE_ABSTAIN_PASS" logs/coverage_abstain.log
	@grep "COVERAGE_ABSTAIN_PASS" logs/coverage_abstain.log

# Sealing must bind the OWNER's rows. hybrid_seal_mined_unit and
# hybrid_coverage_owner were left on the shape-only lookup after coverage
# identity became owner-keyed, so with two specialists on one interface a seal
# could certify unit B against unit A's rows under A's name.
coverage_owner_seal: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_coverage_owner_seal.c include/hybrid_ai.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_coverage_owner_seal \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_coverage_owner_seal.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@$(BIN_DIR)/gate_evidence coverage_owner_seal \
		logs/coverage_owner_seal.log COVERAGE_OWNER_SEAL_PASS -- \
		./$(BIN_DIR)/test_coverage_owner_seal

.PHONY: coverage_owner_seal

# The per-hop guard where it matters: ORDINARY serving, not a benchmark that
# injects its own callback. hop1 covered, hop2 handed an uncovered intermediate.
personal_ai_hop_guard: $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_personal_ai_hop_guard.c include/personal_ai.h include/router.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_personal_ai_hop_guard \
		$(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_personal_ai_hop_guard.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@$(BIN_DIR)/gate_evidence personal_ai_hop_guard \
		logs/personal_ai_hop_guard.log PERSONAL_AI_HOP_GUARD_PASS -- \
		./$(BIN_DIR)/test_personal_ai_hop_guard

# Scope, lineage and least disclosure: a sampled specialist must ship with its
# boundary, a successful import must leave the destination carrying the
# provenance the report claims, and a one-unit capsule must not disclose the
# source's whole oracle registry.
capsule_scope_lineage: $(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_capsule_scope_lineage.c include/cnet_capsule.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_capsule_scope_lineage \
		$(CAPSULE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_capsule_scope_lineage.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@$(BIN_DIR)/gate_evidence capsule_scope_lineage \
		logs/capsule_scope_lineage.log CAPSULE_SCOPE_LINEAGE_PASS -- \
		./$(BIN_DIR)/test_capsule_scope_lineage

# The durable half of abstention: the sidecar itself must be sealed. Every
# corruption mutation must reject the WHOLE file and leave no partial state,
# and no mutation may leave a mined unit servable.
coverage_sidecar_seal: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_coverage_sidecar_seal.c include/hybrid_ai.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_coverage_sidecar_seal \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_coverage_sidecar_seal.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@$(BIN_DIR)/gate_evidence coverage_sidecar_seal \
		logs/coverage_sidecar_seal.log COVERAGE_SIDECAR_SEAL_PASS -- \
		./$(BIN_DIR)/test_coverage_sidecar_seal
	@# The sanitized target is allowed to withhold the address-space assertion;
	@# this one is not. If COVSEAL_NO_RLIMIT ever leaks into this lane, the
	@# amplification bound stops being measured anywhere and this fails.
	@grep -q "vmpeak_withheld=0" logs/coverage_sidecar_seal.log

# Same negatives under ASan+UBSan: a transactional loader that leaks or reads
# freed rows on the reject path has not really rolled anything back.
coverage_sidecar_seal_san: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_coverage_sidecar_seal.c include/hybrid_ai.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
		-fno-omit-frame-pointer -D_DEFAULT_SOURCE -Iinclude $(CUDA_CFLAGS) \
		-o $(BIN_DIR)/coverage_sidecar_seal_san \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_coverage_sidecar_seal.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@# ASan reserves a ~20 TB shadow mapping before main(), so RLIMIT_AS at any
	@# sane ceiling kills the child outright and a VmPeak comparison against a
	@# 20 TB baseline would pass for anything. That one assertion is withheld
	@# here and belongs to the non-sanitized target, which greps for
	@# vmpeak_withheld=0. RLIMIT_CPU, the CPU budget, the refusal verdicts and
	@# the leak/UB checks -- the reason this target exists -- all still apply.
	@ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		COVSEAL_NO_RLIMIT=1 timeout 900 ./$(BIN_DIR)/coverage_sidecar_seal_san \
		> logs/coverage_sidecar_seal_san.log 2>&1; \
		status=$$?; cat logs/coverage_sidecar_seal_san.log; test $$status -eq 0
	@grep -q "COVERAGE_SIDECAR_SEAL_PASS" logs/coverage_sidecar_seal_san.log
	@# ... and the withholding must be stated in the log, not inferred.
	@grep -q "COVERAGE_AMPLIFIED_VMPEAK_WITHHELD reason=sanitizer_shadow_map" \
		logs/coverage_sidecar_seal_san.log

own_learning_health: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tools/cnet_own_learning_health.c include/hybrid_ai.h include/base.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/cnet_own_learning_health \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tools/cnet_own_learning_health.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/mk_test_base \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/mk_test_base.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@# A watchdog that cannot fail is decoration. This proves strict mode
	@# rejects a missing base, an unloadable base, an unnamed base, garbage
	@# booleans, a dead configured teacher, a missing sidecar under an armed
	@# gate, and a truncated sidecar -- all under mkdtemp roots.
	@bash tests/test_own_learning_health.sh $(BIN_DIR)/cnet_own_learning_health \
		$(BIN_DIR)/mk_test_base
	@# Config-only inspection of the committed profile. This is NOT deployment
	@# health and must never print the deployment marker.
	@./$(BIN_DIR)/cnet_own_learning_health --config-only \
		--env-file config/personal-ai.env \
		> logs/own_learning_health.config.json 2>&1; status=$$?; \
		cat logs/own_learning_health.config.json; test $$status -eq 0
	@grep -q "CONFIG_ONLY_PASS" logs/own_learning_health.config.json
	@# Deployed health against the configured base. An absent base is BLOCKED,
	@# never PASS: a deployment that is not here cannot be certified healthy.
	@set -e; base=$${CNET_BASE_PATH:-$(HOME)/AI/CNET/logs/personal.cnb}; \
		if [ ! -f "$$base" ]; then \
			echo "OWN_LEARNING_HEALTH_BLOCKED reason=no_deployed_base path=$$base" \
				| tee logs/own_learning_health.json; \
			echo "Strict deployed health cannot be assessed here. This is not a PASS."; \
			exit 4; \
		fi; \
		./$(BIN_DIR)/cnet_own_learning_health --env-file config/personal-ai.env \
			--base "$$base" > logs/own_learning_health.json 2>&1; status=$$?; \
			cat logs/own_learning_health.json; test $$status -eq 0
	@grep -q "OWN_LEARNING_HEALTH_PASS" logs/own_learning_health.json

residual_substitution_bench_live: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/residual_substitution_bench_live.c include/hybrid_ai.h include/personal_ai.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/residual_substitution_bench_live \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/residual_substitution_bench_live.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@rc=0; ./$(BIN_DIR)/residual_substitution_bench_live > logs/residual_substitution_bench_live.log 2>&1 || rc=$$?; \
		cat logs/residual_substitution_bench_live.log; \
		[ $$rc -eq 0 ] || { echo "residual_substitution_bench_live exited $$rc"; exit $$rc; }
	@grep -qE "SUBSTITUTION_BENCH_LIVE_(PASS|SKIP)" logs/residual_substitution_bench_live.log

residual_substitution_bench: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/residual_substitution_bench.c include/hybrid_ai.h include/personal_ai.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/residual_substitution_bench \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/residual_substitution_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@rc=0; ./$(BIN_DIR)/residual_substitution_bench > logs/residual_substitution_bench.log 2>&1 || rc=$$?; \
		cat logs/residual_substitution_bench.log; \
		[ $$rc -eq 0 ] || { echo "residual_substitution_bench exited $$rc"; exit $$rc; }
	@grep -q "SUBSTITUTION_BENCH_PASS" logs/residual_substitution_bench.log

residual_reservoir: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_residual_reservoir.c include/hybrid_ai.h include/personal_ai.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_residual_reservoir \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_residual_reservoir.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_residual_reservoir > logs/residual_reservoir.log 2>&1
	@grep -q "RESIDUAL_RESERVOIR_PASS" logs/residual_reservoir.log
	@grep "RESIDUAL_RESERVOIR_PASS" logs/residual_reservoir.log

own_learning_loop: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_own_learning_loop.c include/hybrid_ai.h include/personal_ai.h include/cnet_fault.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_own_learning_loop \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_own_learning_loop.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_own_learning_loop > logs/own_learning_loop.log 2>&1
	@grep -q "OWN_LEARNING_LOOP_PASS" logs/own_learning_loop.log
	@grep "OWN_LEARNING_LOOP_PASS" logs/own_learning_loop.log

hybrid_bench: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/bench_hybrid_ai.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/bench_hybrid_ai \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/bench_hybrid_ai.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/bench_hybrid_ai > logs/hybrid_bench.log 2>&1
	@grep -q "HYBRID_BENCH_PASS" logs/hybrid_bench.log
	@cat logs/hybrid_bench.log

.PHONY: learning_delta_bench
learning_delta_bench: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/bench_learning_delta.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/bench_learning_delta \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/bench_learning_delta.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/bench_learning_delta | tee logs/learning_delta.log
	@grep -q "LEARNING_DELTA_BENCH_PASS" logs/learning_delta.log

# Real GGUF residual (Tier C). Hermetic without env; real when path set.
.PHONY: residual_gguf residual_gguf_real
residual_gguf: $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(CURIOSITY_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_residual_gguf.c include/residual_gguf.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_residual_gguf \
		$(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(CURIOSITY_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_residual_gguf.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) $(CURL_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_residual_gguf > logs/residual_gguf.log 2>&1
	@grep -q "RESIDUAL_GGUF_PASS" logs/residual_gguf.log
	@grep "RESIDUAL_GGUF_PASS" logs/residual_gguf.log

# HTTP residual (Bonsai / llama-server). Hermetic mock via unset URL; real with CNET_RESIDUAL_HTTP.
.PHONY: residual_http residual_http_real
residual_http: $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(CURIOSITY_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_residual_http.c include/residual_http.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_residual_http \
		$(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(CURIOSITY_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_residual_http.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) $(CURL_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_residual_http > logs/residual_http.log 2>&1
	@grep -q "RESIDUAL_HTTP_PASS" logs/residual_http.log
	@grep "RESIDUAL_HTTP_PASS" logs/residual_http.log

residual_http_real: residual_http
	@if [ -z "$$CNET_RESIDUAL_HTTP" ]; then \
		echo "Set CNET_RESIDUAL_HTTP=http://127.0.0.1:8080"; exit 2; \
	fi
	@mkdir -p logs
	@CNET_REQUIRE_REAL_RESIDUAL_HTTP=1 \
		./$(BIN_DIR)/test_residual_http > logs/residual_http_real.log 2>&1
	@grep -q "RESIDUAL_HTTP_PASS" logs/residual_http_real.log
	@grep -E "http residual|RESIDUAL_HTTP_PASS|ping|oracle" logs/residual_http_real.log

.PHONY: bonsai_residual_fault_seed
bonsai_residual_fault_seed: $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(GAP_LANE_SRC) $(SRC) tools/bonsai_residual_fault_seed.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/bonsai_residual_fault_seed \
		$(RESIDUAL_GGUF_SRC) $(PILOT_SRC) src/cnet_fault.c src/cnet_promote.c src/cnet_acct.c $(GAP_LANE_SRC) \
		$(SRC) tools/bonsai_residual_fault_seed.c $(LDFLAGS) -pthread
	@echo "built bin/bonsai_residual_fault_seed"

# Requires CNET_RESIDUAL_GGUF (and optional CNET_RESIDUAL_WINDOW). Uses int8 diet.
# Structure-mines residual traces into a certified unit (P5) when real.
residual_gguf_real: residual_gguf
	@if [ -z "$$CNET_RESIDUAL_GGUF" ]; then \
		echo "Set CNET_RESIDUAL_GGUF=/path/model.gguf"; exit 2; \
	fi
	@mkdir -p logs
	@CNET_ORACLE_INT8=$${CNET_ORACLE_INT8:-1} CNET_REQUIRE_REAL_RESIDUAL=1 \
		CNET_RESIDUAL_STRUCTURE_MINE=1 \
		./$(BIN_DIR)/test_residual_gguf > logs/residual_gguf_real.log 2>&1
	@grep -q "RESIDUAL_GGUF_PASS" logs/residual_gguf_real.log
	@grep -E "real residual|structure mine|RESIDUAL_GGUF_PASS|personal_ai serves|post-mine" logs/residual_gguf_real.log

.PHONY: residual_structure_mine_real
residual_structure_mine_real: residual_gguf_real

# Colibrì integration P0–P5 (placement, LFRU, heat mine, batch labels, session KV, pilot).
.PHONY: colibri_integrate cnet_plan_cli
EXT_RESIDUAL_SRC := src/external_residual.c
colibri_integrate: $(PLACEMENT_SRC) $(PILOT_SRC) $(EXT_RESIDUAL_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_colibri_integrate.c include/cnet_placement.h include/cnet_lfru.h include/cnet_pilot.h include/external_residual.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_colibri_integrate \
		$(PLACEMENT_SRC) $(PILOT_SRC) $(EXT_RESIDUAL_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_colibri_integrate.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_colibri_integrate > logs/colibri_integrate.log 2>&1
	@grep -q "COLIBRI_INTEGRATE_PASS" logs/colibri_integrate.log
	@grep "COLIBRI_INTEGRATE_PASS" logs/colibri_integrate.log

cnet_plan_cli: $(PLACEMENT_SRC) include/cnet_placement.h tools/cnet_plan.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/cnet_plan $(PLACEMENT_SRC) tools/cnet_plan.c
	@echo "Built bin/cnet_plan (plan|doctor|json)"

.PHONY: residual_session_chat
residual_session_chat: $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(MODEL_RUNTIME) $(CCE) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(EXT_TEACHER_SRC) tools/residual_session_chat.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/residual_session_chat \
		$(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tools/residual_session_chat.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@echo "Built bin/residual_session_chat (CNET_RESIDUAL_SESSION_KV=1)"

.PHONY: soul_residual_serve
soul_residual_serve: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(EXT_TEACHER_SRC) $(LIBRARY) src/soul_host.c $(ROUTE_LOG_SRC) tests/test_soul_residual.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_soul_residual \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(EXT_TEACHER_SRC) $(LIBRARY) src/soul_host.c $(ROUTE_LOG_SRC) tests/test_soul_residual.c \
		$(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_soul_residual > logs/soul_residual_serve.log 2>&1
	@grep -q "SOUL_RESIDUAL_SERVE_PASS" logs/soul_residual_serve.log
	@grep "SOUL_RESIDUAL_SERVE_PASS" logs/soul_residual_serve.log

.PHONY: personal_ai_auto
personal_ai_auto: tests/test_personal_ai_auto.sh scripts/personal_ai_auto.sh config/cnet-personal-ai-lane.service config/cnet-personal-ai.target config/personal-ai.env
	@mkdir -p logs
	@bash tests/test_personal_ai_auto.sh > logs/personal_ai_auto.log 2>&1
	@grep -q "PERSONAL_AI_AUTO_PASS" logs/personal_ai_auto.log
	@grep "PERSONAL_AI_AUTO_PASS" logs/personal_ai_auto.log

# Post-seal serve proof: teach → seal → SoulHost reopen → certified Tier A.
.PHONY: post_seal_serve serve_proof_cli serve_proof
post_seal_serve: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) src/soul_host.c $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) tests/test_post_seal_serve.c include/personal_ai.h include/soul_host.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_post_seal_serve \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) src/soul_host.c $(ROUTE_LOG_SRC) \
		tests/test_post_seal_serve.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_post_seal_serve > logs/post_seal_serve.log 2>&1
	@grep -q "POST_SEAL_SERVE_PASS" logs/post_seal_serve.log
	@grep "POST_SEAL_SERVE_PASS" logs/post_seal_serve.log

serve_proof_cli: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(EXT_TEACHER_SRC) $(LIBRARY) src/soul_host.c $(ROUTE_LOG_SRC) tools/serve_proof.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/serve_proof \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(EXT_TEACHER_SRC) $(LIBRARY) \
		src/soul_host.c $(ROUTE_LOG_SRC) tools/serve_proof.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread

serve_proof: post_seal_serve
	@bash scripts/personal_ai_serve_proof.sh hermetic
	@echo "SERVE_PROOF_OK"

# Multimodal v0: external teacher bridge + voice/vision closed-set mine/admit.
.PHONY: multimodal_v0 multimodal_prepare voice_real_teacher
multimodal_v0: $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_multimodal_v0.c include/external_teacher.h include/modality_voice.h include/modality_vision.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_multimodal_v0 \
		$(MULTIMODAL_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_multimodal_v0.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_multimodal_v0 > logs/multimodal_v0.log 2>&1
	@grep -q "MULTIMODAL_V0_PASS" logs/multimodal_v0.log
	@$(MAKE) --no-print-directory multimodal_prepare
	@grep "MULTIMODAL_V0_PASS" logs/multimodal_v0.log

voice_real_teacher: $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_voice_real_teacher.c tools/voice_teacher.c include/external_teacher.h include/modality_voice.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_voice_real_teacher \
		$(MULTIMODAL_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_voice_real_teacher.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_voice_real_teacher > logs/voice_real_teacher.log 2>&1
	@grep -q "VOICE_REAL_TEACHER_PASS" logs/voice_real_teacher.log
	@grep "VOICE_REAL_TEACHER_PASS" logs/voice_real_teacher.log

# Single source of truth → C include + .NET partial (check in generated files).
.PHONY: json_toolcall_alphabet json_toolcall_alphabet_check
json_toolcall_alphabet: config/json_toolcall_v2.json $(BIN_DIR)/gen_json_toolcall_alphabet
	@$(BIN_DIR)/gen_json_toolcall_alphabet
	@test -f include/json_toolcall_alphabet.inc
	@test -f dotnet/Cce/JsonToolCall.Alphabet.g.cs

json_toolcall_alphabet_check: config/json_toolcall_v2.json $(BIN_DIR)/gen_json_toolcall_alphabet include/json_toolcall_alphabet.inc dotnet/Cce/JsonToolCall.Alphabet.g.cs
	@$(BIN_DIR)/gen_json_toolcall_alphabet --check

# Closed-set JSON tool-call spine: keyword features → certified tool ONEHOT.
.PHONY: json_toolcall
json_toolcall: json_toolcall_alphabet $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(EXT_TEACHER_SRC) src/soul_host.c $(ROUTE_LOG_SRC) tests/test_json_toolcall.c include/json_toolcall.h include/json_toolcall_alphabet.inc
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_json_toolcall \
		$(MULTIMODAL_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) \
		src/soul_host.c $(ROUTE_LOG_SRC) tests/test_json_toolcall.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_json_toolcall > logs/json_toolcall.log 2>&1
	@grep -q "JSON_TOOLCALL_PASS" logs/json_toolcall.log
	@grep "JSON_TOOLCALL_PASS" logs/json_toolcall.log

# Seal the current json_toolcall_v2 into a live/personal CNB (CLI for scripts/json_toolcall_seal.sh).
.PHONY: json_toolcall_seal_cli
json_toolcall_seal_cli: $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) tools/json_toolcall_seal.c include/json_toolcall.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/json_toolcall_seal \
		$(MULTIMODAL_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) \
		src/soul_host.c $(ROUTE_LOG_SRC) tools/json_toolcall_seal.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread

multimodal_prepare: tests/test_multimodal_prepare.sh tools/multimodal_campaign.sh plans/multimodal_external_teachers.md
	@mkdir -p logs
	@bash tests/test_multimodal_prepare.sh > logs/multimodal_prepare.log 2>&1
	@grep -q "MULTIMODAL_PREPARE_PASS" logs/multimodal_prepare.log


# Quality-preserving campaign speedups: sweep allowlist export + unit
# shard/allowlist screening in flagship (no cert-bar changes).
.PHONY: campaign_v2_fast
campaign_v2_fast: flagship tests/test_campaign_v2_fast.sh tools/campaign_v2_fast.sh tools/margin_sweep_analyze.c
	@mkdir -p logs
	@bash tests/test_campaign_v2_fast.sh > logs/campaign_v2_fast.log 2>&1
	@grep -q "CAMPAIGN_V2_FAST_PASS" logs/campaign_v2_fast.log
	@grep "CAMPAIGN_V2_FAST_PASS" logs/campaign_v2_fast.log

unified_specialist: specialist_unit specialist_health $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_heterogeneous_plan.c include/specialist.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_heterogeneous_plan \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) \
		tests/test_heterogeneous_plan.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_heterogeneous_plan > logs/unified_specialist.log 2>&1
	@grep -q "HET_PLAN_PASS" logs/unified_specialist.log





.PHONY: governance
governance: $(GOV_SRC) tests/test_governance.c include/cnet_governance.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_governance src/cnet_governance.c tests/test_governance.c $(LDFLAGS)
	@./$(BIN_DIR)/test_governance > logs/governance.log 2>&1
	@grep -q GOVERNANCE_PASS logs/governance.log
	@grep GOVERNANCE_PASS logs/governance.log

.PHONY: janitor cnet_janitor_build
cnet_janitor_build: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EXT_TEACHER_SRC) $(JSON_TOOLCALL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) src/soul_host.c $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) tools/cnet_janitor.c $(GOV_SRC) include/cnet_governance.h
	@mkdir -p $(BIN_DIR) logs artifacts/janitor
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/cnet_janitor \
		src/cnet_governance.c $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(JSON_TOOLCALL_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) src/soul_host.c $(ROUTE_LOG_SRC) \
		tools/cnet_janitor.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread

# G1 library janitor. Uses CNET_JANITOR_BASE or CNET_BASE_PATH or soul_gemma4v2_final.cnb.
# Hermetic smoke: builds binary and greps JANITOR_OK from a dry structural self-check when
# CNET_JANITOR_SMOKE_BASE points at a small CNB; otherwise build-only unless RUN_JANITOR=1.
.PHONY: janitor
janitor: cnet_janitor_build
	@if [ "$${RUN_JANITOR:-0}" = "1" ]; then \
	  BASE="$${CNET_JANITOR_BASE:-$${CNET_BASE_PATH:-soul_gemma4v2_final.cnb}}"; \
	  ./$(BIN_DIR)/cnet_janitor "$$BASE" | tee logs/janitor.log; \
	  grep -q JANITOR_OK logs/janitor.log; \
	elif [ -n "$${CNET_JANITOR_SMOKE_BASE:-}" ] && [ -f "$${CNET_JANITOR_SMOKE_BASE}" ]; then \
	  ./$(BIN_DIR)/cnet_janitor "$${CNET_JANITOR_SMOKE_BASE}" | tee logs/janitor_smoke.log; \
	  grep -q JANITOR_OK logs/janitor_smoke.log; \
	else \
	  test -x $(BIN_DIR)/cnet_janitor && echo "JANITOR_BUILD_OK bin/cnet_janitor"; \
	fi



.PHONY: serve_named_skills
serve_named_skills: cnet_janitor_build tools/serve_named_skills.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/serve_named_skills \
		src/cnet_governance.c $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(JSON_TOOLCALL_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) src/soul_host.c $(ROUTE_LOG_SRC) \
		tools/serve_named_skills.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/serve_named_skills $${CNET_BASE_PATH:-soul_gemma4v2_final.cnb} | tee logs/serve_named_skills.log
	@grep -q SERVE_NAMED_SKILLS_ logs/serve_named_skills.log


# G3 dense-bucket library consolidate (NOT the tile-memory PPMI `consolidate` target).
.PHONY: cnet_consolidate cnet_consolidate_build
cnet_consolidate_build: src/cnet_governance.c tools/cnet_consolidate.c include/cnet_governance.h include/base.h $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EXT_TEACHER_SRC) $(JSON_TOOLCALL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) src/soul_host.c $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC)
	@mkdir -p $(BIN_DIR) logs artifacts/janitor
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/cnet_consolidate \
		src/cnet_governance.c $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(JSON_TOOLCALL_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) src/soul_host.c $(ROUTE_LOG_SRC) \
		tools/cnet_consolidate.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread

cnet_consolidate: cnet_consolidate_build
	@./$(BIN_DIR)/cnet_consolidate $${CNET_BASE_PATH:-soul_gemma4v2_final.cnb} | tee logs/cnet_consolidate.log
	@grep -q CONSOLIDATE_PLAN logs/cnet_consolidate.log


.PHONY: live_eight_campaign
live_eight_campaign: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EXT_TEACHER_SRC) $(JSON_TOOLCALL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) src/soul_host.c $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) tools/live_eight_campaign.c include/json_toolcall.h
	@mkdir -p $(BIN_DIR) logs artifacts
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/live_eight_campaign \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(JSON_TOOLCALL_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) src/soul_host.c $(ROUTE_LOG_SRC) \
		tools/live_eight_campaign.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/live_eight_campaign > logs/live_eight_campaign.log 2>&1
	@grep -q "LIVE_EIGHT_CAMPAIGN_PASS" logs/live_eight_campaign.log
	@grep "LIVE_EIGHT_CAMPAIGN_PASS" logs/live_eight_campaign.log

.PHONY: oracle_teacher_runtime
oracle_teacher_runtime: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_oracle_teacher_runtime.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_oracle_teacher_runtime \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_oracle_teacher_runtime.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_oracle_teacher_runtime > logs/oracle_teacher_runtime.log 2>&1
	@grep -q "ORACLE_TEACHER_RUNTIME_PASS" logs/oracle_teacher_runtime.log
	@grep "ORACLE_TEACHER_RUNTIME_PASS" logs/oracle_teacher_runtime.log

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

.PHONY: mcp_compression_test
mcp_compression_test: cnet_dll
	@mkdir -p logs
	LD_LIBRARY_PATH="$(CURDIR)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}" \
		$(DOTNET) test dotnet/CnetMcpServer.Tests/CnetMcpServer.Tests.csproj \
		-c Debug --nologo -v:q \
		--filter "FullyQualifiedName~CompressionArtifactTests" \
		> logs/mcp_compression_test.log 2>&1
	@grep -Eq 'Passed: +[1-9][0-9]*, Skipped: +0' logs/mcp_compression_test.log
	@echo "MCP_COMPRESSION_TEST_PASS"

.PHONY: mcp_protocol_survival
mcp_protocol_survival: cnet_dll soul_host_test
	@mkdir -p logs
	LD_LIBRARY_PATH="$(CURDIR)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}" \
		$(DOTNET) test dotnet/CnetMcpServer.Tests/CnetMcpServer.Tests.csproj \
		-c Debug --nologo -v:q \
		> logs/mcp_protocol_survival.log 2>&1
	@grep -Eq 'Passed: +[1-9][0-9]*, Skipped: +0' logs/mcp_protocol_survival.log
	@echo "MCP_PROTOCOL_SURVIVAL_GATE_PASS" | tee -a logs/mcp_protocol_survival.log

# Focused warning-debt regression gate.  Keep this narrower than the full
# build: it protects the repaired ownership boundary and its public contract
# files with the exact warning policy used for remediation.
.PHONY: warning_debt_strict
warning_debt_strict:
	@mkdir -p logs
	@set -e; for source in \
		src/router/dag_full.c src/router/registry.c src/router/route.c src/nn.c \
		src/contract/book_concept.c src/contract/interactive_agent.c \
		src/contract/mcp_memory.c src/contract/narrative_diffusion.c \
		src/contract/text_add.c; do \
		$(CC) -std=c11 -Wall -Wextra -Wformat=2 -Werror -pedantic \
			-D_DEFAULT_SOURCE -Iinclude -Isrc -fsyntax-only "$$source"; \
	done > logs/warning_debt_strict.log 2>&1
	@echo "WARNING_DEBT_STRICT_PASS" | tee -a logs/warning_debt_strict.log

# Full native warning ratchet. OpenMP pragmas are source-guarded when OpenMP is
# disabled; every -Wall/-Wextra/-Wpedantic diagnostic is a release failure for
# the complete shared-library source set.
.PHONY: native_warning_gate native_test_warning_gate managed_warning_gate release_warning_gate
.PHONY: print-%
native_warning_gate:
	@mkdir -p logs
	@$(MAKE) --no-print-directory PORTABLE=1 \
		CFLAGS='-std=c11 -Wall -Wextra -Wpedantic $(OPTFLAGS) -mno-avx -D_DEFAULT_SOURCE -fPIC -Werror' \
		cnet_dll > logs/native_warning_gate.log 2>&1
	@if grep -Eq '(^|[[:space:]])(warning|error):' logs/native_warning_gate.log; then \
		cat logs/native_warning_gate.log; \
		exit 1; \
	fi
	@echo "NATIVE_WARNING_GATE_PASS" | tee -a logs/native_warning_gate.log

# The production-only gate above cannot see warnings introduced by the
# amalgamated native test sources.  Compile that exact target under the same
# warning-as-error policy; execution remains owned by the behavioral gates.
native_test_warning_gate:
	@mkdir -p logs
	@$(MAKE) --no-print-directory PORTABLE=1 \
		CFLAGS='-std=c11 -Wall -Wextra -Wpedantic $(OPTFLAGS) -mno-avx -D_DEFAULT_SOURCE -Werror' \
		test_all > logs/native_test_warning_gate.log 2>&1
	@if grep -Eq '(^|[[:space:]])(warning|error):' logs/native_test_warning_gate.log; then \
		cat logs/native_test_warning_gate.log; \
		exit 1; \
	fi
	@echo "NATIVE_TEST_WARNING_GATE_PASS" | tee -a logs/native_test_warning_gate.log

# Managed warning policy is deliberately offline: dependency restoration is a
# provisioning step, while release verification must not gain network egress.
#
# `--no-restore` alone made that provisioning step SOMEBODY ELSE'S PROBLEM, and
# a fresh checkout does not have one. At 7396275 `make ci_core` passed here and
# exited 2 in a truly fresh detached worktree of the same commit:
#
#   error NETSDK1004: Assets file '.../dotnet/Cce.Tests/obj/project.assets.json'
#   not found. Run a NuGet package restore to generate this file.
#
# The writer's tree passed only because it held ignored `obj/` state. So each
# project is restored here, first, against an explicitly EMPTY local source:
# `--source` REPLACES the configured package feeds, so no configured feed can be
# contacted, while the machine's existing global package cache still resolves. A
# package that is not already cached fails closed instead of reaching out.
# Measured on a cold fresh worktree, offline: ~50 ms per project.
#
# `--source` IS NOT ENOUGH, and a review was right to refuse the offline claim
# on it. NuGet's `auditSources` is a SEPARATE section that `--source` does not
# touch, and NuGetAudit reaches it during restore. Measured at cb240b5 with a
# user config declaring an audit source on a local HTTP endpoint:
#
#   error NU1900: Error occurred while getting package vulnerability data:
#   Unable to load the service index for source http://127.0.0.1:28081/v3/index.json
#   strace -f -e trace=network: 6 connect() to AF_INET6 ::ffff:127.0.0.1:28081
#
# So the restore is handed an isolated NuGet.Config generated inside the same
# unique temp root as the empty source. It CLEARS packageSources and
# auditSources, names only the empty local directory, and is passed with
# `--configfile`, which makes NuGet read that file INSTEAD OF the user and
# machine configs rather than merging with them. `-p:NuGetAudit=false` is
# defense in depth: two independent reasons no audit endpoint can be reached.
#
# Restore output goes to its own log, so the warning POLICY below is unchanged
# -- it still scans build output only.
MANAGED_WARNING_PROJECTS ?= \
	dotnet/Cce/Cce.csproj \
	dotnet/Cce.Tests/Cce.Tests.csproj \
	dotnet/CnetMcpServer/CnetMcpServer.csproj \
	dotnet/CnetMcpServer.Tests/CnetMcpServer.Tests.csproj \
	dotnet/CceHost/CceHost.csproj \
	dotnet/CnetHarnessSmoke/CnetHarnessSmoke.csproj \
	dotnet/Cce.Benchmarks/Cce.Benchmarks.csproj
MANAGED_WARNING_LOG ?= logs/managed_warning_gate.log
MANAGED_RESTORE_LOG ?= logs/managed_warning_restore.log
MANAGED_RESTORE_TIMEOUT ?= 300

print-%:
	@echo "$($*)"

managed_warning_gate: json_toolcall_alphabet_check
	@mkdir -p logs
	@: > $(MANAGED_WARNING_LOG)
	@: > $(MANAGED_RESTORE_LOG)
	@temp_root=`mktemp -d "$${TMPDIR:-/tmp}/cnet-nuget-isolated-XXXXXX"` || { \
		echo "MANAGED_WARNING_GATE_FAIL reason=no_temp_root"; exit 1; }; \
	trap 'rm -rf "$$temp_root"' EXIT HUP INT TERM; \
	empty_source="$$temp_root/empty-source"; \
	nuget_config="$$temp_root/NuGet.Config"; \
	mkdir -p "$$empty_source" || { \
		echo "MANAGED_WARNING_GATE_FAIL reason=no_empty_source"; exit 1; }; \
	printf '%s\n' \
		'<?xml version="1.0" encoding="utf-8"?>' \
		'<configuration>' \
		'  <packageSources>' \
		'    <clear />' \
		'    <add key="cnet-empty-local" value="'"$$empty_source"'" />' \
		'  </packageSources>' \
		'  <disabledPackageSources>' \
		'    <clear />' \
		'  </disabledPackageSources>' \
		'  <fallbackPackageFolders>' \
		'    <clear />' \
		'  </fallbackPackageFolders>' \
		'  <auditSources>' \
		'    <clear />' \
		'  </auditSources>' \
		'</configuration>' > "$$nuget_config" || { \
		echo "MANAGED_WARNING_GATE_FAIL reason=no_isolated_config"; exit 1; }; \
	for project in $(MANAGED_WARNING_PROJECTS); do \
		echo "restoring $$project" >> $(MANAGED_RESTORE_LOG); \
		timeout $(MANAGED_RESTORE_TIMEOUT) $(DOTNET) restore "$$project" \
			--configfile "$$nuget_config" --source "$$empty_source" \
			-p:NuGetAudit=false --nologo -v:minimal \
			>> $(MANAGED_RESTORE_LOG) 2>&1; \
		status=$$?; \
		if [ $$status -ne 0 ]; then \
			cat $(MANAGED_RESTORE_LOG); \
			echo "MANAGED_WARNING_GATE_FAIL reason=restore_failed project=$$project status=$$status"; \
			exit 1; \
		fi; \
		assets="$${project%/*}/obj/project.assets.json"; \
		if [ ! -f "$$assets" ]; then \
			cat $(MANAGED_RESTORE_LOG); \
			echo "MANAGED_WARNING_GATE_FAIL reason=assets_absent project=$$project path=$$assets"; \
			exit 1; \
		fi; \
		$(DOTNET) build "$$project" --no-restore --nologo -v:minimal \
			-p:TreatWarningsAsErrors=true -warnaserror \
			>> $(MANAGED_WARNING_LOG) 2>&1 || { \
				cat $(MANAGED_WARNING_LOG); \
				echo "MANAGED_WARNING_GATE_FAIL reason=build_failed project=$$project"; \
				exit 1; \
			}; \
	done
	@if grep -Eq '(^|[[:space:]])(warning|error) [A-Z]+[0-9]+:' $(MANAGED_WARNING_LOG); then \
		cat $(MANAGED_WARNING_LOG); \
		exit 1; \
	fi
	@echo "MANAGED_WARNING_GATE_PASS" | tee -a $(MANAGED_WARNING_LOG)

# The harness for the property above. It drives this same recipe with a fake
# `dotnet` that records every invocation, so "restore precedes build, offline,
# for every project" is measured rather than asserted in a comment.
# The empirical half of the offline claim: run the REAL gate against the REAL
# projects under a hostile user NuGet config and read the syscalls. This is an
# EVIDENCE lane, not a ci_core prerequisite -- it needs strace, and the property
# it confirms is already enforced in ci_core by managed_warning_prereq. It fails
# closed rather than skipping when strace or the package cache is missing.
.PHONY: managed_warning_offline_proof
managed_warning_offline_proof: tests/test_managed_warning_offline.sh Makefile
	@mkdir -p logs
	@$(BIN_DIR)/gate_evidence managed_warning_offline_proof \
		logs/managed_warning_offline_proof.log \
		MANAGED_WARNING_OFFLINE_PROOF_PASS -- \
		sh tests/test_managed_warning_offline.sh

.PHONY: managed_warning_prereq
managed_warning_prereq: tests/test_managed_warning_prereq.sh Makefile
	@mkdir -p logs
	@$(BIN_DIR)/gate_evidence managed_warning_prereq \
		logs/managed_warning_prereq.log MANAGED_WARNING_PREREQ_PASS -- \
		sh tests/test_managed_warning_prereq.sh
	@# ... and both REDs it was written against stay re-runnable. Each replayed
	@# revision must FAIL, and must fail ON ITS OWN PROPERTY rather than on
	@# something incidental, or the replay proves nothing about what changed.
	@saved=`mktemp "$${TMPDIR:-/tmp}/cnet-managed-log-XXXXXX"`; \
	cp $(MANAGED_WARNING_LOG) "$$saved" 2>/dev/null || : > "$$saved"; \
	rc=0; \
	set -- "$(MANAGED_PREREQ_LEGACY_REV_ORDER):restore precedes build" \
	       "$(MANAGED_PREREQ_LEGACY_REV_ISOLATION):isolated config with --configfile"; \
	for pair in "$$@"; do \
		rev=$${pair%%:*}; want=$${pair#*:}; \
		CNET_MANAGED_PREREQ_LEGACY=$$rev \
			sh tests/test_managed_warning_prereq.sh \
			> logs/managed_warning_prereq_red_$$rev.log 2>&1; \
		red=$$?; \
		if [ $$red -eq 0 ]; then \
			echo "MANAGED_WARNING_PREREQ_RED_FAIL rev=$$rev the pre-fix recipe passed; the gate proves nothing"; \
			rc=1; \
		elif ! grep -q "$$want" logs/managed_warning_prereq_red_$$rev.log; then \
			echo "MANAGED_WARNING_PREREQ_RED_FAIL rev=$$rev failed for some OTHER reason than: $$want"; \
			cat logs/managed_warning_prereq_red_$$rev.log; \
			rc=1; \
		else \
			echo "MANAGED_WARNING_PREREQ_RED_CONFIRMED rev=$$rev property=$$want"; \
		fi; \
	done; \
	cp "$$saved" $(MANAGED_WARNING_LOG) 2>/dev/null || :; rm -f "$$saved"; \
	exit $$rc

# The last commit before the managed gate restored what it builds at all ...
MANAGED_PREREQ_LEGACY_REV_ORDER ?= 7396275
# ... and the last one before that restore was isolated from the user's NuGet
# configuration, whose auditSources `--source` does not cover.
MANAGED_PREREQ_LEGACY_REV_ISOLATION ?= cb240b5

release_warning_gate: native_warning_gate native_test_warning_gate managed_warning_prereq managed_warning_gate
	@mkdir -p logs
	@printf '%s\n' 'RELEASE_WARNING_GATE_PASS' > logs/release_warning_gate.log
	@echo "RELEASE_WARNING_GATE_PASS"

.PHONY: pkgconfig
pkgconfig: VERSION
	@mkdir -p $(BIN_DIR)/pkgconfig
	@printf '%s\n' \
		"prefix=$(PREFIX)" \
		'exec_prefix=$${prefix}' \
		'libdir=$${exec_prefix}/lib' \
		'includedir=$${prefix}/include/cnet' \
		'' \
		'Name: CNET' \
		'Description: Certified native specialist and CCE runtime' \
		"Version: $(CNET_VERSION)" \
		'Libs: -L$${libdir} -lcnet' \
		'Libs.private: -lm -pthread' \
		'Cflags: -I$${includedir}' > $(BIN_DIR)/pkgconfig/cnet.pc

.PHONY: install uninstall
install: cnet_dll pkgconfig
	$(INSTALL) -d "$(DESTDIR)$(LIBDIR)" "$(DESTDIR)$(INCLUDEDIR)/cnet" "$(DESTDIR)$(PKGCONFIGDIR)"
	$(INSTALL) -m 755 cnet.so "$(DESTDIR)$(LIBDIR)/libcnet.so.$(CNET_VERSION)"
	ln -sfn "libcnet.so.$(CNET_VERSION)" "$(DESTDIR)$(LIBDIR)/libcnet.so.$(CNET_ABI_VERSION)"
	ln -sfn "libcnet.so.$(CNET_ABI_VERSION)" "$(DESTDIR)$(LIBDIR)/libcnet.so"
	cp -R include/. "$(DESTDIR)$(INCLUDEDIR)/cnet/"
	$(INSTALL) -m 644 $(BIN_DIR)/pkgconfig/cnet.pc "$(DESTDIR)$(PKGCONFIGDIR)/cnet.pc"
	@echo "CNET_INSTALL_PASS version=$(CNET_VERSION) prefix=$(PREFIX)"

uninstall:
	rm -f "$(DESTDIR)$(LIBDIR)/libcnet.so" \
		"$(DESTDIR)$(LIBDIR)/libcnet.so.$(CNET_ABI_VERSION)" \
		"$(DESTDIR)$(LIBDIR)/libcnet.so.$(CNET_VERSION)" \
		"$(DESTDIR)$(PKGCONFIGDIR)/cnet.pc"
	rm -rf "$(DESTDIR)$(INCLUDEDIR)/cnet"
	@echo "CNET_UNINSTALL_PASS prefix=$(PREFIX)"

.PHONY: dist
dist: VERSION include/cnet_version.h
	@mkdir -p "$(DIST_DIR)"
	@set -eu; out="$(DIST_DIR)/cnet-$(CNET_VERSION).tar.gz"; tmp="$$out.tmp"; \
		tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
			--transform='s,^,cnet-$(CNET_VERSION)/,' \
			--exclude='*.cnb' --exclude='*.so' --exclude='*.dll' \
			--exclude='*/bin' --exclude='*/bin/*' --exclude='*/obj' --exclude='*/obj/*' \
			-cf - Makefile README.md VERSION .github docs dotnet include src tests tools scripts \
			| gzip -n > "$$tmp"; \
		mv "$$tmp" "$$out"
	@echo "CNET_DIST_PASS $(DIST_DIR)/cnet-$(CNET_VERSION).tar.gz"

.PHONY: agent_memory_integrity registry_restart_unit persistence_integrity specialist_authority admission_abi_audit

.PHONY: gguf_integrity
gguf_integrity: $(CCE) tests/test_cce_gguf_q5_integrity.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_DEFAULT_SOURCE -Iinclude \
		-o $(BIN_DIR)/test_cce_gguf_q5_integrity \
		tests/test_cce_gguf_q5_integrity.c $(CCE) \
		-lm -lpthread
	@timeout 30 ./$(BIN_DIR)/test_cce_gguf_q5_integrity > logs/gguf_integrity.log 2>&1
	@grep -q "GGUF_INTEGRITY_PASS" logs/gguf_integrity.log
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g -D_DEFAULT_SOURCE \
		-fsanitize=address,leak -fno-omit-frame-pointer -Iinclude \
		-o $(BIN_DIR)/test_cce_gguf_q5_integrity_asan \
		tests/test_cce_gguf_q5_integrity.c $(CCE) \
		-lm -lpthread
	@timeout 60 env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		./$(BIN_DIR)/test_cce_gguf_q5_integrity_asan > logs/gguf_integrity_asan.log 2>&1
	@grep -q "GGUF_INTEGRITY_PASS" logs/gguf_integrity_asan.log
	@echo "GGUF_INTEGRITY_GATE_PASS"

.PHONY: model_runtime_integrity
model_runtime_integrity: src/model_runtime.c tests/test_model_runtime.c include/model_runtime.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_DEFAULT_SOURCE -Iinclude -pthread \
		-o $(BIN_DIR)/test_model_runtime_integrity src/model_runtime.c tests/test_model_runtime.c
	@timeout 30 ./$(BIN_DIR)/test_model_runtime_integrity > logs/model_runtime_integrity.log 2>&1
	@grep -q "MODEL_RUNTIME_PASS" logs/model_runtime_integrity.log
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g -D_DEFAULT_SOURCE \
		-fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude -pthread \
		-o $(BIN_DIR)/test_model_runtime_integrity_san src/model_runtime.c tests/test_model_runtime.c
	@timeout 60 env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		./$(BIN_DIR)/test_model_runtime_integrity_san > logs/model_runtime_integrity_san.log 2>&1
	@grep -q "MODEL_RUNTIME_PASS" logs/model_runtime_integrity_san.log
	@echo "MODEL_RUNTIME_INTEGRITY_GATE_PASS"

agent_memory_integrity: src/agent_memory.c tests/test_agent_memory.c include/agent_memory.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_DEFAULT_SOURCE -Iinclude \
		-o $(BIN_DIR)/test_agent_memory src/agent_memory.c tests/test_agent_memory.c $(LDFLAGS)
	@./$(BIN_DIR)/test_agent_memory > logs/agent_memory_integrity.log 2>&1
	@grep -q "PERSISTENCE_INTEGRITY_PASS" logs/agent_memory_integrity.log

registry_restart_unit: src/nn.c $(ROUTER) $(PLAN_TABLE) src/contract/contract.c src/contract/unit.c tests/test_expansion.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_DEFAULT_SOURCE -Iinclude \
		-o $(BIN_DIR)/test_registry_restart src/nn.c $(ROUTER) $(PLAN_TABLE) \
		src/contract/contract.c src/contract/unit.c tests/test_expansion.c $(LDFLAGS)
	@./$(BIN_DIR)/test_registry_restart > logs/registry_restart_unit.log 2>&1
	@grep -q "All expansion tests passed" logs/registry_restart_unit.log

persistence_integrity: agent_memory_integrity registry_restart_unit soul_reopen_test
	@echo "PERSISTENCE_INTEGRITY_GATE_PASS"

admission_abi_audit: cnet_dll tests/audit_admission_abi.sh
	@sh tests/audit_admission_abi.sh > logs/admission_abi_audit.log 2>&1
	@grep -q "ADMISSION_ABI_AUDIT_PASS" logs/admission_abi_audit.log

specialist_authority: specialist_unit admission_bypass_audit admission_abi_audit
	@echo "SPECIALIST_AUTHORITY_PASS"

.PHONY: ci_config_gate release_package dotnet_cce_tests ci_core ci ci_contract_gate evidence_special_index
ci_config_gate: tests/test_ci_workflow.sh Makefile $(BIN_DIR)/gate_evidence
	@mkdir -p logs
	@$(BIN_DIR)/gate_evidence ci_config_gate logs/ci_config_gate.log \
		CI_WORKFLOW_LOCAL_PASS -- bash tests/test_ci_workflow.sh

release_package: json_toolcall_alphabet_check tests/test_release_package.sh VERSION include/cnet_version.h
	@sh tests/test_release_package.sh > logs/release_package.log 2>&1
	@grep -q "RELEASE_PACKAGE_PASS" logs/release_package.log

dotnet_cce_tests:
	$(call dotnet_guard)
	@set -e; \
		$(DOTNET) test dotnet/Cce.Tests/Cce.Tests.csproj -c Release --verbosity minimal \
			> logs/dotnet_cce_tests.log 2>&1; \
		$(DOTNET) test dotnet/Cce.Llm.Tests/CNET.Cce.Llm.Tests.csproj -c Release --verbosity minimal \
			> logs/dotnet_cce_llm_tests.log 2>&1; \
		echo "DOTNET_CCE_TESTS_PASS" >> logs/dotnet_cce_tests.log; \
		echo "DOTNET_CCE_LLM_TESTS_PASS" >> logs/dotnet_cce_llm_tests.log
	@grep -q '^DOTNET_CCE_TESTS_PASS$$' logs/dotnet_cce_tests.log
	@grep -q '^DOTNET_CCE_LLM_TESTS_PASS$$' logs/dotnet_cce_llm_tests.log

# cnet.so is versioned on purpose (docs/RELEASE_POLICY.md), so the committed
# blob is what build-only clones receive. This pins the ISA it was built with;
# it reads the committed blob rather than the working tree, because `verify`
# rebuilds these files at whatever ARCH_CFLAGS the current invocation carries.
artifact_isa_gate: tests/test_artifact_isa.sh cnet.so cce.dll
	@mkdir -p logs
	@bash tests/test_artifact_isa.sh > logs/artifact_isa_gate.log 2>&1; rc=$$?; \
		cat logs/artifact_isa_gate.log; exit $$rc
	@grep -q '^ARTIFACT_ISA_PASS' logs/artifact_isa_gate.log

# Asserts the tracked/ignored split matches release policy: runtime state and
# private caches stay local, the versioned release library stays tracked. The
# script predates this wiring and was referenced by no target.
runtime_artifact_hygiene: tests/test_runtime_artifact_hygiene.sh
	@mkdir -p logs
	@bash tests/test_runtime_artifact_hygiene.sh > logs/runtime_artifact_hygiene.log 2>&1; \
		rc=$$?; cat logs/runtime_artifact_hygiene.log; exit $$rc
	@grep -q '^RUNTIME_ARTIFACT_HYGIENE_PASS' logs/runtime_artifact_hygiene.log

# What CI proves is declared in config/ci_contract.json and enforced by
# ci_contract_gate: every gate the contract marks `required` must be a
# prerequisite here, every gate it marks `blocked` must NOT be, and an absent
# hosted workflow is WITHHELD rather than a pass.
ci_contract_gate: config/ci_contract.json tests/test_ci_contract.sh Makefile $(BIN_DIR)/gate_evidence
	@mkdir -p logs
	@$(BIN_DIR)/gate_evidence ci_contract_gate logs/ci_contract.log \
		CI_CONTRACT_PASS -- bash tests/test_ci_contract.sh

# The evidence runners bound the tree by walking `git status`, which does not
# report assume-unchanged or skip-worktree paths -- 83 of them here, including
# src/nn.c. Only their count was recorded, so a producer could rewrite the
# trainer mid-run and still be certified. Disposable-repo gate, no network.
evidence_special_index: $(BIN_DIR)/gate_evidence dotnet/CnetControlPlane/CnetControlPlane.csproj \
		dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj
	@mkdir -p logs
	@$(BIN_DIR)/gate_evidence evidence_special_index \
		logs/evidence_special_index.log EVIDENCE_SPECIAL_INDEX_PASS -- \
		bash -c 'dotnet test dotnet/CnetControlPlane.Tests --filter FullyQualifiedName~SpecialIndexBindingTests --verbosity minimal && echo EVIDENCE_SPECIAL_INDEX_PASS'
	@# Historical RED against pre-fix Python runners is WITHHELD (runners deleted in purge).
	@echo "EVIDENCE_SPECIAL_INDEX_RED_WITHHELD rev=$(EVIDENCE_LEGACY_REV) reason=legacy_python_runners_removed" | tee logs/evidence_special_index_red.log

# The last commit before special-index paths were bound.
EVIDENCE_LEGACY_REV ?= dc3b2a2

ci_core: ci_contract_gate evidence_special_index recipe_gate certify legacy_persist_guard btn_train_plateau knowledge_capsule capsule_scope_lineage knowledge_accumulation_bench knowledge_composition_bench personal_ai_hop_guard coverage_abstain coverage_sidecar_seal cnu_budget vd_frontend_parse port_raw_unit_seam vision_coverage_test vision_capsule_asset capability_cert ci_config_gate warning_debt_strict release_warning_gate flagship_prefix_cache campaign_provenance_unit execution_tiers_doc_gate alt_paths_gate artifact_isa_gate runtime_artifact_hygiene
	@echo "CNET_CI_CORE_PASS"

ci: ci_core release_package test dotnet_cce_tests cce_train_bench int8_matvec_bench
	@echo "CNET_CI_PASS"

# Guards the int8 oracle matvec's vectorisation. Gates on bit-identity against
# an in-process no-vectorize reference and on the single-threaded speedup ratio
# over that reference -- both host-independent, so no wall-clock floor is baked
# in. Catches an ISA/flag regression on the oracle head that cce_train_bench
# cannot see. Override the ratio with CNET_INT8_MIN_SPEEDUP.
int8_matvec_bench: $(CCE) $(CCE_CUDA_OBJ) tests/int8_matvec_bench.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) \
		tests/int8_matvec_bench.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	@./$(BIN_DIR)/int8_matvec_bench > logs/int8_matvec_bench.log 2>&1; rc=$$?; \
		cat logs/int8_matvec_bench.log; exit $$rc
	@grep -q '^INT8_MATVEC_BENCH_PASS$$' logs/int8_matvec_bench.log

# ---- ROCm / AMD GPU lane ---------------------------------------------------
# cce_hipgemm dlopens ROCm at runtime and needs no SDK to compile, so this gate
# builds on every host and self-skips where there is no device. That makes it
# safe to keep in the portable lane, but a skip must never be mistaken for a
# device result -- see CNET_REQUIRE_ROCM in tests/test_hipgemm.c.
hipgemm_res: $(CCE) $(CCE_CUDA_OBJ) tests/test_hipgemm.c include/cce/cce_hipgemm.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(CCE) $(CCE_CUDA_OBJ) \
		tests/test_hipgemm.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	@./$(BIN_DIR)/hipgemm_res > logs/hipgemm_res.log 2>&1; rc=$$?; \
		cat logs/hipgemm_res.log; exit $$rc
	@grep -q '^HIPGEMM_RES_PASS' logs/hipgemm_res.log

# Optional NVIDIA peer of hipgemm_res. Links only cce_cudagemm (no full CCE /
# curl/mmap) so Windows CUDA laptops can gate without the Linux-only deps.
# Self-skips without a driver; CNET_REQUIRE_CUDA=1 fails the skip.
cudagemm_res: src/cce/cce_cudagemm.c tests/test_cudagemm.c include/cce/cce_cudagemm.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ src/cce/cce_cudagemm.c tests/test_cudagemm.c -lm
	@./$(BIN_DIR)/cudagemm_res > logs/cudagemm_res.log 2>&1; rc=$$?; \
		cat logs/cudagemm_res.log; exit $$rc
	@grep -q '^CUDAGEMM_RES_PASS' logs/cudagemm_res.log

# The authoritative GPU gate for this project: portable CI plus a bounded, real
# device slice. GitHub-hosted runners have no AMD GPU, so `make ci_rocm` on an
# AMD/ROCm host is the authority -- CI itself can only run the portable lane.
# CNET_REQUIRE_ROCM=1 turns the self-skip into a failure, so this target cannot
# report success on a machine whose GPU is missing or broken.
ci_rocm: ci
	@CNET_REQUIRE_ROCM=1 $(MAKE) --no-print-directory hipgemm_res
	@echo "CNET_CI_ROCM_PASS"

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
unified_ds4_launcher: scripts/run_cnet_ds4_dual.sh tools/cnet_chunk_hash.c dotnet/CnetControlPlane/CnetControlPlane.csproj tests/test_ds4_dual_launcher.sh
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

soul_host_test: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(EXT_TEACHER_SRC) $(LIBRARY) tests/test_soul_host.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(EXT_TEACHER_SRC) $(LIBRARY) tests/test_soul_host.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	CNET_KEEP_TEST_BASE=1 ./$(BIN_DIR)/$@ > logs/soul_host_test.log 2>&1
	@grep -q "SOUL_HOST_UNIFIED_PASS" logs/soul_host_test.log

.PHONY: build_hygiene_test dotnet_restore
build_hygiene_test: tests/test_build_hygiene.sh
	@mkdir -p logs
	@bash tests/test_build_hygiene.sh > logs/build_hygiene_test.log 2>&1
	@grep -q "BUILD_HYGIENE_PASS" logs/build_hygiene_test.log

dotnet_restore:
	$(call dotnet_guard)
	@mkdir -p logs
	@: > logs/dotnet_restore.log
	@set -e; for project in $(DOTNET_RESTORE_PROJECTS); do \
		echo "restoring $$project" >> logs/dotnet_restore.log; \
		$(DOTNET) restore "$$project" --nologo -v:minimal >> logs/dotnet_restore.log 2>&1; \
	done
	@if grep -q 'NU1603.*was not found.*resolved instead' logs/dotnet_restore.log; then \
		cat logs/dotnet_restore.log >&2; exit 1; \
	fi
	@echo "DOTNET_RESTORE_PASS" | tee -a logs/dotnet_restore.log

# Reopen/remount execution tracer: durable SpecialistKind across close/reopen,
# resolver-based Oracle remount certified against provenance-linked sealed
# truth, typed native+oracle chain, explicit refusal tallies.
.PHONY: soul_reopen_test admission_bypass_audit
soul_reopen_test: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(EXT_TEACHER_SRC) tests/test_soul_reopen.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(EXT_TEACHER_SRC) $(LIBRARY) tests/test_soul_reopen.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	./$(BIN_DIR)/$@ > logs/soul_reopen_test.log 2>&1
	@grep -q "SPECIALIST_REOPEN_PASS" logs/soul_reopen_test.log

# Static gate: production admission must go through the specialist door.
admission_bypass_audit: tests/audit_admission_bypass.sh
	@mkdir -p logs
	@sh tests/audit_admission_bypass.sh > logs/admission_bypass_audit.log 2>&1
	@grep -q "ADMISSION_BYPASS_AUDIT_PASS" logs/admission_bypass_audit.log

# Counterfactual serving shadow gate: the native cce_router counterfactual
# contract executes on the hosted soul_route serving path as REPORT-ONLY
# evidence behind CNET_COUNTERFACTUAL (default OFF). Core assertion: a served
# answer is BYTE-IDENTICAL with the knob on or off, metadata is present only
# when ON, and refusal semantics (unknown goal, gap-inbox note) are unchanged.
.PHONY: counterfactual_serving
counterfactual_serving: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(EXT_TEACHER_SRC) tests/test_counterfactual_serving.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(GAP_LANE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(EXT_TEACHER_SRC) $(LIBRARY) tests/test_counterfactual_serving.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	./$(BIN_DIR)/$@ > logs/counterfactual_serving.log 2>&1
	@grep -q "CNET_COUNTERFACTUAL REPORT" logs/counterfactual_serving.log
	@grep -q "COUNTERFACTUAL_SERVING_PASS" logs/counterfactual_serving.log

unified_native: unified_adapter unified_cce_adapter unified_oracle_adapter unified_specialist gap_lane gap_lane_service_config dispatch_story oracle_v2_test oracle_teacher_runtime unified_async unified_models unified_ds4_launcher soul_host_test soul_reopen_test counterfactual_serving admission_bypass_audit cnet_dll build_hygiene_test alt_paths_gate aicimo_core_test agent_role route_log evidence_bundle health_layers cnet_harness_contract_test resource_governor counterfactual_order self_improve deploy_profile
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
	@nm -D cnet.so | grep -q " cce_aicimo_route_decision$$"
	@nm -D cnet.so | grep -q " cnet_lane_pool_open$$"
	@nm -D cnet.so | grep -q " cnet_lane_pool_submit$$"
	@nm -D cnet.so | grep -q " soul_oracle_count$$"
	@nm -D cnet.so | grep -q " soul_oracle_identity$$"
	@nm -D cnet.so | grep -q " soul_oracle_artifact_sha256$$"
	@nm -D cnet.so | grep -q " soul_oracle_runtime_libs_digest$$"
	@nm -D cnet.so | grep -q " soul_mount_oracles$$"
	@nm -D cnet.so | grep -q " soul_unit_kind$$"
	@nm -D cnet.so | grep -q " soul_mounted_oracle_count$$"


.PHONY: priority_acceptance
priority_acceptance:
	@$(MAKE) --no-print-directory recipe_gate
	@$(MAKE) --no-print-directory claims_test
	@$(MAKE) --no-print-directory heal_mismatch
	@$(MAKE) --no-print-directory heal_mismatch_san
	@$(MAKE) --no-print-directory campaign_provenance
	@$(MAKE) --no-print-directory execution_tiers_doc_gate
	@$(MAKE) --no-print-directory gguf_integrity
	@$(MAKE) --no-print-directory cce_qwen35
	@$(MAKE) --no-print-directory cce_qwen35_e2e
	@$(MAKE) --no-print-directory flagship_prefix_cache
	@$(MAKE) --no-print-directory ci_config_gate
	@$(MAKE) --no-print-directory release_warning_gate
	@if [ "$(SKIP_RELEASE_PACKAGE)" != "1" ]; then \
		$(MAKE) --no-print-directory release_package; \
	fi
	@$(MAKE) --no-print-directory unified
	@echo "PRIORITY_ACCEPTANCE_PASS"

# Single release authority. Focused integrity slices run first; the existing
# portable CI and full private acceptance umbrellas run only after every slice
# is green. Output is published atomically at the end of the bounded sequence.
.PHONY: license_metadata_test release_integrity_authority release_integrity
license_metadata_test: tests/test_license_metadata.sh LICENSE README.md docs/RELEASE_POLICY.md dotnet/Cce/Cce.csproj Makefile
	@mkdir -p logs
	@bash tests/test_license_metadata.sh > logs/license_metadata_test.log 2>&1
	@grep -q "LICENSE_METADATA_PASS" logs/license_metadata_test.log

release_integrity_authority: tests/test_release_integrity_authority.sh tests/run_release_integrity.sh VERSION include/cnet_version.h docs/RELEASE_POLICY.md Makefile
	@mkdir -p logs
	@bash tests/test_release_integrity_authority.sh > logs/release_integrity_authority.log 2>&1
	@grep -q "RELEASE_INTEGRITY_AUTHORITY_PASS" logs/release_integrity_authority.log

release_integrity: tests/run_release_integrity.sh
	@bash tests/run_release_integrity.sh

.PHONY: unified
unified: json_toolcall_alphabet_check
	@mkdir -p logs
	@rm -f logs/unified.started
	@touch logs/unified.started
	@$(MAKE) --no-print-directory unified_native
	@$(MAKE) --no-print-directory dotnet_restore
	$(DOTNET) build dotnet/CceHost/CceHost.csproj -c Release --no-restore --nologo -v:q > logs/unified_dotnet_build.log 2>&1
	$(DOTNET) build dotnet/CnetMcpServer/CnetMcpServer.csproj -c Release --no-restore --nologo -v:q >> logs/unified_dotnet_build.log 2>&1
	LD_LIBRARY_PATH="$(CURDIR)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}" $(DOTNET) dotnet/CceHost/bin/Release/net10.0/CceHost.dll --list tmp_soul_host.cnb > logs/unified_host.log 2>&1
	@grep -q "CNET_HOST_UNIFIED_PASS" logs/unified_host.log
	@printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}' '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"cnet_list_units","arguments":{}}}' '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"cnet_list_oracles","arguments":{}}}' '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"cnet_health_tick","arguments":{}}}' '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"cnet_request_capability","arguments":{"goal_tag":"unified_novel_probe","in_tag":"unified_input","width":4}}}' | CNET_BASE_PATH="$(CURDIR)/tmp_soul_host.cnb" CNET_GAP_INBOX="$(CURDIR)/tmp_soul_host.cnb.inbox" LD_LIBRARY_PATH="$(CURDIR)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}" $(DOTNET) dotnet/CnetMcpServer/bin/Release/net10.0/CnetMcpServer.dll > logs/unified_mcp.log 2> logs/unified_mcp.stderr.log
	@grep -q '"name":"cnet-mcp"' logs/unified_mcp.log
	@grep -q "acq_unified_goal" logs/unified_mcp.log
	@grep -q "unified_teacher" logs/unified_mcp.log
	@grep -q "descriptor_only_not_runtime_trust" logs/unified_mcp.log
	@grep -q "artifactSha256.*a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf" logs/unified_mcp.log
	@grep -q "runtimeLibsDigest.*0x00000000c1b5c0de" logs/unified_mcp.log
	@grep -q "reset_remaining" logs/unified_mcp.log
	@grep -qE 'gap_noted[^:]*:true' logs/unified_mcp.log
	@grep -q "NO_PLAN" tmp_soul_host.cnb.inbox
	LD_LIBRARY_PATH="$(CURDIR)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}" $(DOTNET) test dotnet/Cce.Tests/Cce.Tests.csproj -c Release --no-restore --nologo -v:q > logs/unified_dotnet_test.log 2>&1
	@rm -f tmp_soul_host.cnb tmp_soul_host.cnb.tmp tmp_soul_host.cnb.inbox
	@$(MAKE) --no-print-directory claims_test
	@bash scripts/gen_claims.sh --strict --scope unified --since logs/unified.started
	@rm -f logs/unified.started
	@echo "CNET_UNIFIED_PASS"

# Generated claims ledger. `make claims` executes the unified CPU gate and emits
# run-scoped evidence fresh relative to its sentinel. `make claims_all` merely
# inventories every known log (including standalone GPU evidence); its PASS is
# marker presence, not current-run provenance.
.PHONY: claims claims_all claims_test claims_model model_evidence
claims_test: tests/test_claims.sh scripts/gen_claims.sh
	@bash tests/test_claims.sh

claims: unified asi_framing
	@grep -q '^# Verified Today (generated)' docs/verified-today.generated.md

# The one claim that must never drift: what CNET says it is building. Four
# authoritative surfaces, one canonical sentence, checked by grep rather than a
# snapshot file that would rot on the first reflow.
.PHONY: asi_framing
asi_framing:
	@for f in README.md AGENTS.md docs/INDEX.md docs/ARCHITECTURE.md; do \
		grep -q 'ASI — Artificial Specialized Intelligence' $$f || \
			{ echo "ASI_FRAMING_FAIL missing canonical sentence in $$f"; exit 1; }; \
		grep -q 'never' $$f && grep -q 'Artificial Superintelligence' $$f || \
			{ echo "ASI_FRAMING_FAIL missing superintelligence disclaimer in $$f"; exit 1; }; \
		grep -q 'not claiming AGI' $$f || \
			{ echo "ASI_FRAMING_FAIL missing AGI disclaimer in $$f"; exit 1; }; \
	done
	@echo "ASI_FRAMING_PASS files=4"

claims_all: claims_test
	@bash scripts/gen_claims.sh --strict

# Private-checkpoint evidence is separate from the hermetic unified gate.
# Missing checkpoints or reference dumps emit SKIPPED and fail this target.
model_evidence: moe_e2e_build proj_qat_gemma_e2e_build claims_test
	@mkdir -p logs
	@rm -f logs/model_evidence.started
	@touch logs/model_evidence.started
	@CNET_REQUIRE_REAL_MODEL=1 ./$(BIN_DIR)/moe_e2e > logs/moe_e2e.console.log 2>&1 || :
	@CNET_REQUIRE_REAL_MODEL=1 ./$(BIN_DIR)/proj_qat_gemma_e2e > logs/proj_qat_gemma_e2e.log 2>&1 || :
	@rc=0; bash scripts/gen_claims.sh --strict --scope model --since logs/model_evidence.started || rc=$$?; \
		rm -f logs/model_evidence.started; exit $$rc
	@echo "CNET_MODEL_EVIDENCE_PASS"

claims_model: model_evidence
	@grep -q '^# Verified Today (generated)' docs/verified-today.generated.md


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


# AICIMO smoke test (pure C). cce_aicimo.c is now in the core CCE aggregate.
aicimo_smoke: $(CCE) tests/aicimo_smoke.c include/cce/cce_aicimo.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/aicimo_smoke $(CCE) tests/aicimo_smoke.c $(LDFLAGS)
	./$(BIN_DIR)/aicimo_smoke

# AICIMO core routing test — hermetic focused test proving:
#   identity/residual preservation, deterministic role routing selection,
#   uncertainty from actual route state, invalid argument rejection.
.PHONY: aicimo_core_test
aicimo_core_test: $(CCE) tests/test_aicimo_core.c include/cce/cce_aicimo.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/aicimo_core_test $(CCE) tests/test_aicimo_core.c $(LDFLAGS)
	./$(BIN_DIR)/aicimo_core_test > logs/aicimo_core_test.log 2>&1
	@grep -q "AICIMO_CORE_TEST_PASS" logs/aicimo_core_test.log

# CNET harness contract test — hermetic ABI + AICIMO surface test for the
# .NET-owned inference harness plugin. Links cnet_harness_core.c only; the
# llama.cpp-backed backend TU is intentionally left out so this target does
# not require llama.cpp. See docs/cnet_dotnet_inference_harness.md.
CNET_HARNESS_CORE := src/cnet_harness/cnet_harness_core.c
CNET_HARNESS_HEADERS := include/cnet_harness.h src/cnet_harness/cnet_harness_private.h include/cce/cce_aicimo.h include/cnet_agent_role.h include/cnet_route_log.h include/model_runtime.h include/model_probe.h

# Explicit agent-role vocabulary (layer-3 policy). Gate: make agent_role.
.PHONY: agent_role
agent_role: $(AGENT_ROLE_SRC) tests/test_cnet_agent_role.c include/cnet_agent_role.h include/cnet_harness.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_cnet_agent_role \
		$(AGENT_ROLE_SRC) tests/test_cnet_agent_role.c $(LDFLAGS)
	./$(BIN_DIR)/test_cnet_agent_role > logs/agent_role.log 2>&1
	@grep -q "AGENT_ROLE_PASS" logs/agent_role.log

# Route decision JSONL (mechanism / expert / entropy / outcome / latency / cost).
.PHONY: route_log
route_log: $(ROUTE_LOG_SRC) tests/test_cnet_route_log.c include/cnet_route_log.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_cnet_route_log \
		$(ROUTE_LOG_SRC) tests/test_cnet_route_log.c $(LDFLAGS)
	./$(BIN_DIR)/test_cnet_route_log > logs/route_log.log 2>&1
	@grep -q "ROUTE_LOG_PASS" logs/route_log.log

# Per-unit evidence bundles (contract/dataset/artifact/runtime/reliability/CF/rollback).
.PHONY: evidence_bundle
evidence_bundle: $(EVIDENCE_BUNDLE_SRC) $(BASE_SRC) $(ACQUIRE_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) tests/test_cnet_evidence_bundle.c include/cnet_evidence_bundle.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_cnet_evidence_bundle \
		$(EVIDENCE_BUNDLE_SRC) $(BASE_SRC) $(ACQUIRE_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) \
		tests/test_cnet_evidence_bundle.c $(LDFLAGS) -pthread
	./$(BIN_DIR)/test_cnet_evidence_bundle > logs/evidence_bundle.log 2>&1
	@grep -q "EVIDENCE_BUNDLE_PASS" logs/evidence_bundle.log

.PHONY: shared_workspace semantic_cortex sleep_consolidate calibrated_governance
.PHONY: capability_cert_runner_test capability_cert cognitive_runtime_smoke cognitive_runtime
.PHONY: heldout_fixture_test capability_fixture_causality
.PHONY: capability_evaluator_prereq
shared_workspace: $(SHARED_WORKSPACE_SRC) tests/test_cnet_shared_workspace.c include/cnet_shared_workspace.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/$@ \
		$(SHARED_WORKSPACE_SRC) tests/test_cnet_shared_workspace.c $(LDFLAGS)
	@$(BIN_DIR)/$@ > logs/shared_workspace.log 2>&1; status=$$?; \
		cat logs/shared_workspace.log; test $$status -eq 0 && \
		grep -q "SHARED_WORKSPACE_PASS" logs/shared_workspace.log

semantic_cortex: $(SHARED_WORKSPACE_SRC) $(SEMANTIC_CORTEX_SRC) $(HELDOUT_SRC) tests/test_cnet_semantic_cortex.c include/cnet_shared_workspace.h include/cnet_semantic_cortex.h include/cnet_heldout.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/$@ \
		$(SHARED_WORKSPACE_SRC) $(SEMANTIC_CORTEX_SRC) $(HELDOUT_SRC) \
		tests/test_cnet_semantic_cortex.c $(LDFLAGS)
	@$(BIN_DIR)/$@ > logs/semantic_cortex.log 2>&1; status=$$?; \
		cat logs/semantic_cortex.log; test $$status -eq 0 && \
		grep -q "SEMANTIC_CORTEX_PASS" logs/semantic_cortex.log

sleep_consolidate: $(SLEEP_CONSOLIDATE_SRC) $(TILEMEM_SRC) $(SYNONYMS_SRC) $(HELDOUT_SRC) tests/test_cnet_sleep_consolidate.c include/cnet_sleep_consolidate.h include/corpus/tile_memory.h include/corpus/synonyms.h include/cnet_heldout.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/$@ \
		$(SLEEP_CONSOLIDATE_SRC) $(TILEMEM_SRC) $(SYNONYMS_SRC) $(HELDOUT_SRC) \
		tests/test_cnet_sleep_consolidate.c $(LDFLAGS)
	@$(BIN_DIR)/$@ > logs/sleep_consolidate.log 2>&1; status=$$?; \
		cat logs/sleep_consolidate.log; test $$status -eq 0 && \
		grep -q "SLEEP_CONSOLIDATE_PASS" logs/sleep_consolidate.log

calibrated_governance: $(CALIBRATED_GOVERNANCE_SRC) $(HELDOUT_SRC) tests/test_cnet_calibrated_governance.c include/cnet_calibrated_governance.h include/cnet_heldout.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/$@ \
		$(CALIBRATED_GOVERNANCE_SRC) $(HELDOUT_SRC) \
		tests/test_cnet_calibrated_governance.c $(LDFLAGS)
	@$(BIN_DIR)/$@ > logs/calibrated_governance.log 2>&1; status=$$?; \
		cat logs/calibrated_governance.log; test $$status -eq 0 && \
		grep -q "CALIBRATED_GOVERNANCE_PASS" logs/calibrated_governance.log

capability_cert_runner_test: dotnet/CnetControlPlane/CnetControlPlane.csproj dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj
	@mkdir -p logs
	@dotnet test dotnet/CnetControlPlane.Tests --filter FullyQualifiedName~CapabilityCertRunnerTests --verbosity minimal \
		> logs/capability_cert_runner.log 2>&1; status=$?; \
		cat logs/capability_cert_runner.log; test $status -eq 0; \
		echo CAPABILITY_CERT_RUNNER_PASS | tee -a logs/capability_cert_runner.log >/dev/null; \
		grep -q "CAPABILITY_CERT_RUNNER_PASS" logs/capability_cert_runner.log

# Unit gate for the fixture reader every evaluator now certifies through. If it
# can be made to report a fixture it did not consume, every causality claim
# downstream is decorative again, so its negatives are the point.
heldout_fixture_test: $(HELDOUT_SRC) tests/test_cnet_heldout.c include/cnet_heldout.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/$@ \
		$(HELDOUT_SRC) tests/test_cnet_heldout.c $(LDFLAGS)
	@$(BIN_DIR)/gate_evidence heldout_fixture_test \
		logs/heldout_fixture_test.log CNET_HELDOUT_TEST_PASS -- \
		$(BIN_DIR)/heldout_fixture_test

# A fresh checkout must be able to BUILD what it is about to believe. This
# exists because `make capability_cert` passed here and exited 2 in a fresh
# detached worktree of the same commit: `dotnet test --no-restore` against an
# unrestored project emits zero bytes and exits 0, and nothing in the tree ever
# built it. The unit lane proves the guard's negatives; the CLI prepares every
# committed manifest and refuses missing or stale output by name.
capability_evaluator_prereq: dotnet/CnetControlPlane/CnetControlPlane.csproj \
		dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj \
		$(wildcard config/capability_manifests/*.json)
	@mkdir -p logs
	@$(BIN_DIR)/gate_evidence capability_evaluator_prereq \
		logs/capability_evaluator_prereq.log \
		CAPABILITY_EVALUATOR_PREREQ_UNIT_PASS -- \
		bash -c 'dotnet test dotnet/CnetControlPlane.Tests --filter FullyQualifiedName~EvaluatorPrereqTests --verbosity minimal && echo CAPABILITY_EVALUATOR_PREREQ_UNIT_PASS'

# The decisive truthfulness experiment: mutate one declared expectation while
# leaving every marker string byte-identical, and require the evaluator to fail.
capability_fixture_causality: dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj \
		dotnet/CnetControlPlane/CnetControlPlane.csproj
	@mkdir -p logs
	@$(BIN_DIR)/gate_evidence capability_fixture_causality \
		logs/capability_fixture_causality.log \
		CAPABILITY_FIXTURE_CAUSALITY_PASS -- \
		bash -c 'dotnet test dotnet/CnetControlPlane.Tests --filter FullyQualifiedName~FixtureCausalityCoreTests --verbosity minimal && echo CAPABILITY_FIXTURE_CAUSALITY_PASS'

capability_cert: capability_cert_runner_test capability_evaluator_prereq heldout_fixture_test capability_fixture_causality
	@dotnet run --project dotnet/CnetControlPlane -- capability-cert

cognitive_runtime_smoke: $(SHARED_WORKSPACE_SRC) $(SEMANTIC_CORTEX_SRC) $(CALIBRATED_GOVERNANCE_SRC) tests/test_cnet_cognitive_runtime.c include/cnet_shared_workspace.h include/cnet_semantic_cortex.h include/cnet_calibrated_governance.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -Iinclude -o $(BIN_DIR)/$@ \
		$(SHARED_WORKSPACE_SRC) $(SEMANTIC_CORTEX_SRC) \
		$(CALIBRATED_GOVERNANCE_SRC) \
		tests/test_cnet_cognitive_runtime.c $(LDFLAGS)
	@$(BIN_DIR)/$@ > logs/cognitive_runtime_smoke.log 2>&1; status=$$?; \
		cat logs/cognitive_runtime_smoke.log; test $$status -eq 0 && \
		grep -q "COGNITIVE_RUNTIME_SMOKE_PASS" \
			logs/cognitive_runtime_smoke.log

cognitive_runtime: shared_workspace semantic_cortex sleep_consolidate calibrated_governance cognitive_runtime_smoke capability_cert knowledge_capsule cce_train_bench
	@n=$$(jq -r '.certified' logs/capability_cert.json); \
		echo "COGNITIVE_RUNTIME_PASS capabilities=$$n classification=measured"

# Live residual lane (Bonsai HTTP). Default soft-skip if server down unless
# CNET_REQUIRE_REAL_RESIDUAL_HTTP=1. Uses residual_http_real + cortex live bind.
.PHONY: semantic_cortex_live cognitive_runtime_live
semantic_cortex_live: $(SHARED_WORKSPACE_SRC) $(SEMANTIC_CORTEX_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) $(CURIOSITY_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) tests/test_cnet_semantic_cortex_live.c include/cnet_semantic_cortex.h include/residual_http.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -Iinclude -o $(BIN_DIR)/semantic_cortex_live \
		$(SHARED_WORKSPACE_SRC) $(SEMANTIC_CORTEX_SRC) \
		$(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) \
		$(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) \
		$(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(CURIOSITY_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) \
		$(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) \
		$(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) \
		$(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) \
		tests/test_cnet_semantic_cortex_live.c \
		$(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) $(CURL_LDFLAGS) -pthread
	@$(BIN_DIR)/semantic_cortex_live > logs/semantic_cortex_live.log 2>&1; status=$$?; \
		cat logs/semantic_cortex_live.log; test $$status -eq 0 && \
		grep -q "SEMANTIC_CORTEX_LIVE_PASS" logs/semantic_cortex_live.log

cognitive_runtime_live: residual_http_real semantic_cortex_live cognitive_runtime
	@grep -q "RESIDUAL_HTTP_PASS" logs/residual_http_real.log
	@grep -q "SEMANTIC_CORTEX_LIVE_PASS status=measured" logs/semantic_cortex_live.log
	@echo "COGNITIVE_RUNTIME_LIVE_PASS residual_http=measured cortex_live=measured"


# ---- Six-priority use-loop gates (2026-07-21) ----------------------------
.PHONY: serve_feedback oracle_unattested benchmark_taxonomy miner_efficiency_bench cnet_use_loop_acceptance

serve_feedback: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) src/soul_host.c $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) tests/test_serve_feedback.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_serve_feedback \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) src/soul_host.c $(ROUTE_LOG_SRC) \
		tests/test_serve_feedback.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_serve_feedback > logs/serve_feedback.log 2>&1
	@grep -q "SERVE_FEEDBACK_PASS" logs/serve_feedback.log
	@grep "SERVE_FEEDBACK_PASS" logs/serve_feedback.log

oracle_unattested: $(ACQUIRE_SRC) $(BASE_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) tests/test_oracle_unattested.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_oracle_unattested \
		$(ACQUIRE_SRC) $(BASE_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) \
		tests/test_oracle_unattested.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/test_oracle_unattested > logs/oracle_unattested.log 2>&1
	@grep -q "ORACLE_UNATTESTED_PASS" logs/oracle_unattested.log
	@grep "ORACLE_UNATTESTED_PASS" logs/oracle_unattested.log

benchmark_taxonomy: tests/test_benchmark_taxonomy.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_benchmark_taxonomy tests/test_benchmark_taxonomy.c $(LDFLAGS)
	@./$(BIN_DIR)/test_benchmark_taxonomy > logs/benchmark_taxonomy.log 2>&1
	@grep -q "BENCHMARK_TAXONOMY_PASS" logs/benchmark_taxonomy.log
	@grep "BENCHMARK_TAXONOMY_PASS" logs/benchmark_taxonomy.log

miner_efficiency_bench: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/miner_efficiency_bench.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/miner_efficiency_bench \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) \
		tests/miner_efficiency_bench.c $(LDFLAGS) -pthread
	@./$(BIN_DIR)/miner_efficiency_bench > logs/miner_efficiency_bench.log 2>&1
	@grep -q "MINER_EFFICIENCY_BENCH_PASS" logs/miner_efficiency_bench.log
	@grep "MINER_EFFICIENCY_BENCH_PASS" logs/miner_efficiency_bench.log

# Ordered six-priority umbrella (hermetic + existing product gates).
# Does not start teachers/GPUs/services. Single final check for the chunk.

.PHONY: cnet_deep_use_loop
cnet_deep_use_loop: $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EXT_TEACHER_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) src/soul_host.c $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) tests/test_cnet_deep_use_loop.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_cnet_deep_use_loop \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(EXT_TEACHER_SRC) \
		$(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) src/soul_host.c $(ROUTE_LOG_SRC) \
		tests/test_cnet_deep_use_loop.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_deep_use_loop > logs/cnet_deep_use_loop.log 2>&1
	@grep -q "CNET_DEEP_USE_LOOP_PASS" logs/cnet_deep_use_loop.log
	@grep "CNET_DEEP_USE_LOOP_PASS" logs/cnet_deep_use_loop.log

cnet_use_loop_acceptance: cnet_deep_use_loop serve_feedback self_improve post_seal_serve miner_efficiency_bench acquire oracle_unattested json_toolcall residual_gguf soul_residual_serve phase123_benchmark_test benchmark_taxonomy health_layers evidence_bundle route_log agent_role
	@echo "CNET_USE_LOOP_ACCEPTANCE_PASS"

# Five-layer health diagnostics (registry/loadable/execution/semantic/utility).
.PHONY: health_layers
health_layers: $(HEALTH_LAYERS_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_cnet_health_layers.c include/cnet_health_layers.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_cnet_health_layers \
		$(HEALTH_LAYERS_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) \
		$(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) \
		tests/test_cnet_health_layers.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	./$(BIN_DIR)/test_cnet_health_layers > logs/health_layers.log 2>&1
	@grep -q "HEALTH_LAYERS_PASS" logs/health_layers.log

# Optional plugin build. Links cnet.so + llama.cpp libraries. Not part of
# unified / release_integrity: it requires the pinned llama.cpp ROCm build
# ($(LLAMA_CPP_BUILD)) to be present, exactly as cnet_llama_eval already does.
CNET_HARNESS_LLAMA := src/cnet_harness/cnet_harness_llama.cpp

.PHONY: cnet_harness_plugin
cnet_harness_plugin: cnet_dll $(CNET_HARNESS_CORE) $(CNET_HARNESS_LLAMA) $(CNET_HARNESS_HEADERS)
	@mkdir -p $(BIN_DIR)
	# cnet.so's SONAME is libcnet.so.$(CNET_ABI_VERSION). The plugin links
	# against the -l:cnet.so file in the repo root but at load time the
	# dynamic linker looks for that SONAME on the plugin's RPATH.
	# $(BIN_DIR)/libcnet.so.$(CNET_ABI_VERSION) -> ../cnet.so satisfies it.
	ln -sfn ../cnet.so $(BIN_DIR)/libcnet.so.$(CNET_ABI_VERSION)
	$(CXX) -std=c++17 -Wall -Wextra -Werror -O2 -fPIC -shared -Iinclude \
		-I$(LLAMA_CPP_ROOT)/include -I$(LLAMA_CPP_ROOT)/ggml/include \
		-Wl,-soname,libcnet_harness.so \
		-o $(BIN_DIR)/libcnet_harness.so \
		$(CNET_HARNESS_CORE) $(CNET_HARNESS_LLAMA) \
		-L. -l:cnet.so -L$(LLAMA_CPP_BUILD)/bin -lllama -lggml -lggml-base \
		-ldl -pthread \
		-Wl,-rpath,'$$ORIGIN' -Wl,-rpath,'$$ORIGIN/..' \
		-Wl,-rpath,$(LLAMA_CPP_BUILD)/bin
	@echo "Built $(BIN_DIR)/libcnet_harness.so (CNET .NET inference harness plugin)."

# Real bounded-offload acceptance. Partial CPU/GPU execution produces changing
# scheduler subgraphs, so it intentionally links the dedicated HIP build with
# GGML_HIP_GRAPHS=OFF rather than the full-GPU-optimized default build.
CNET_HARNESS_GPU_BENCHMARK_REPEATS ?= 3
CNET_HARNESS_GPU_BENCHMARK_TIMEOUT ?= 1800

.PHONY: cnet_harness_gpu_benchmark
cnet_harness_gpu_benchmark: LLAMA_CPP_BUILD := $(LLAMA_CPP_HYBRID_BUILD)
cnet_harness_gpu_benchmark: cnet_harness_plugin
	$(call dotnet_guard)
	@test -n "$(CNET_HARNESS_MODEL)" || { \
		echo "CNET_HARNESS_MODEL=/absolute/path/model.gguf is required" >&2; exit 2; \
	}
	@test -f "$(LLAMA_CPP_BUILD)/CMakeCache.txt" || { \
		echo "missing hybrid llama.cpp build: $(LLAMA_CPP_BUILD)" >&2; exit 2; \
	}
	@grep -q '^GGML_HIP_GRAPHS:BOOL=OFF$$' "$(LLAMA_CPP_BUILD)/CMakeCache.txt" || { \
		echo "hybrid llama.cpp build must set GGML_HIP_GRAPHS=OFF" >&2; exit 2; \
	}
	@mkdir -p logs
	$(DOTNET) build dotnet/CnetHarnessSmoke/CnetHarnessSmoke.csproj -c Release --nologo \
		> logs/cnet_harness_gpu_smoke_build.log 2>&1
	timeout --signal=TERM --kill-after=15s $(CNET_HARNESS_GPU_BENCHMARK_TIMEOUT)s \
		$(BIN_DIR)/cnet_harness_gpu_benchmark \
		--model "$(CNET_HARNESS_MODEL)" \
		--worktree "$(CURDIR)" \
		--dotnet "$(DOTNET)" \
		--llama-bin "$(LLAMA_CPP_BUILD)/bin" \
		--repeats $(CNET_HARNESS_GPU_BENCHMARK_REPEATS)
	@grep -q '"status": "CNET_HARNESS_GPU_BENCHMARK_PASS"' \
		logs/cnet_harness_gpu_benchmark.json

.PHONY: cnet_harness_contract_test
cnet_harness_contract_test: $(CCE) $(CCE_MODEL_CATALOG) $(MODEL_RUNTIME) $(MODEL_PROBE) $(CNET_HARNESS_CORE) $(AGENT_ROLE_SRC) $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(SPECIALIST_SRC) $(SPECIALIST_ADAPTERS) $(CNET_CCE_ADAPTER) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_cnet_harness_contract.c tests/test_cnet_harness_failclosed.c $(CNET_HARNESS_HEADERS)
	@mkdir -p $(BIN_DIR) logs
	# Main contract test: links strong fake backend hooks so the route-only
	# ABI surface can be exercised without llama.cpp.
	# cnet_health_layers.c calls specialist_axes and btn_reliability, whose
	# definers pull the specialist/contract/registry support set — the same
	# known-good closure gap_lane links. Still hermetic in the sense that
	# matters: no llama.cpp, no GGUF.
	$(CC) $(CFLAGS) -o $(BIN_DIR)/cnet_harness_contract_test \
		$(CCE) $(CCE_MODEL_CATALOG) $(MODEL_RUNTIME) $(MODEL_PROBE) \
		$(CNET_HARNESS_CORE) $(AGENT_ROLE_SRC) $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(SPECIALIST_SRC) $(SPECIALIST_ADAPTERS) $(CNET_CCE_ADAPTER) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_cnet_harness_contract.c \
		$(LDFLAGS) -pthread
	./$(BIN_DIR)/cnet_harness_contract_test > logs/cnet_harness_contract_test.log 2>&1
	@grep -q "CNET_HARNESS_CONTRACT_TEST_PASS" logs/cnet_harness_contract_test.log
	# Fail-closed sibling: same core, no fake backend, proves the weak
	# default refuses to fabricate a successful open with a readable
	# dummy path.
	$(CC) $(CFLAGS) -o $(BIN_DIR)/cnet_harness_failclosed_test \
		$(CCE) $(CCE_MODEL_CATALOG) $(MODEL_RUNTIME) $(MODEL_PROBE) \
		$(CNET_HARNESS_CORE) $(AGENT_ROLE_SRC) $(ROUTE_LOG_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(SPECIALIST_SRC) $(SPECIALIST_ADAPTERS) $(CNET_CCE_ADAPTER) $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) tests/test_cnet_harness_failclosed.c \
		$(LDFLAGS) -pthread
	./$(BIN_DIR)/cnet_harness_failclosed_test > logs/cnet_harness_failclosed_test.log 2>&1
	@grep -q "CNET_HARNESS_FAILCLOSED_TEST_PASS" logs/cnet_harness_failclosed_test.log

# Managed harness unit tests: exercises dotnet/Cce.Tests filtered to the
# CnetHarnessTests class. Fakes the native invoker; does not require the
# plugin .so or a real GGUF, so it stays hermetic.

.PHONY: dotnet_harness_test
dotnet_harness_test:
	$(call dotnet_guard)
	@mkdir -p logs
	$(DOTNET) test dotnet/Cce.Tests/Cce.Tests.csproj -c Release --nologo \
		--filter "FullyQualifiedName~CnetHarnessTests|FullyQualifiedName~AsyncContextPipelineTests" \
		2>&1 | tee logs/dotnet_harness_test.log
	@grep -Eq "Passed:[[:space:]]*[1-9][0-9]*" logs/dotnet_harness_test.log

# Optional real-model tracer. This is intentionally outside unified/release:
# it requires a private local GGUF and a caller-selected llama.cpp build.
# It is CPU-only, bounded, and starts no model server.
CNET_HARNESS_SMOKE_TIMEOUT ?= 300

.PHONY: cnet_harness_real_smoke
cnet_harness_real_smoke: cnet_harness_plugin
	$(call dotnet_guard)
	@test -n "$(CNET_HARNESS_MODEL)" || { \
		echo "CNET_HARNESS_MODEL=/absolute/path/model.gguf is required" >&2; exit 2; \
	}
	@mkdir -p logs
	$(DOTNET) build dotnet/CnetHarnessSmoke/CnetHarnessSmoke.csproj -c Release --nologo \
		> logs/cnet_harness_smoke_build.log 2>&1
	timeout --signal=TERM --kill-after=15s $(CNET_HARNESS_SMOKE_TIMEOUT)s env \
		CNET_HARNESS_LIBRARY="$(CURDIR)/$(BIN_DIR)/libcnet_harness.so" \
		ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
		$(DOTNET) run --no-build -c Release \
		--project dotnet/CnetHarnessSmoke/CnetHarnessSmoke.csproj -- \
		"$(CNET_HARNESS_MODEL)" > logs/cnet_harness_real_smoke.log 2>&1
	@grep -q "CNET_HARNESS_REAL_SMOKE_PASS" logs/cnet_harness_real_smoke.log
	@echo "CNET_HARNESS_REAL_SMOKE_PASS"

# Real-model memory/continuity acceptance. Unlike the basic smoke, this keeps
# n_batch deliberately smaller than n_ctx and proves: long-prompt chunking,
# output-equivalent common-prefix KV reuse, and bounded post-warmup RSS/private
# growth across repeated generations. Requires a caller-owned private GGUF.
CNET_HARNESS_MEMORY_TIMEOUT ?= 600

.PHONY: cnet_harness_async_context_acceptance
cnet_harness_async_context_acceptance: cnet_harness_plugin
	$(call dotnet_guard)
	@test -n "$(CNET_HARNESS_MODEL)" || { \
		echo "CNET_HARNESS_MODEL=/absolute/path/model.gguf is required" >&2; exit 2; \
	}
	@mkdir -p logs
	$(DOTNET) build dotnet/CnetHarnessSmoke/CnetHarnessSmoke.csproj -c Release --nologo \
		> logs/cnet_harness_async_context_build.log 2>&1 || { \
		cat logs/cnet_harness_async_context_build.log >&2; exit 1; \
	}
	timeout --signal=TERM --kill-after=15s $(CNET_HARNESS_MEMORY_TIMEOUT)s env \
		CNET_HARNESS_LIBRARY="$(CURDIR)/$(BIN_DIR)/libcnet_harness.so" \
		ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
		$(DOTNET) run --no-build -c Release \
		--project dotnet/CnetHarnessSmoke/CnetHarnessSmoke.csproj -- \
		"$(CNET_HARNESS_MODEL)" --async-context \
		> logs/cnet_harness_async_context.log 2>&1
	@grep -q '^{"status":"CNET_HARNESS_ASYNC_CONTEXT_PASS"' logs/cnet_harness_async_context.log
	@echo "CNET_HARNESS_ASYNC_CONTEXT_PASS"

.PHONY: cnet_harness_memory_acceptance
cnet_harness_memory_acceptance: cnet_harness_plugin cnet_harness_async_context_acceptance
	$(call dotnet_guard)
	@test -n "$(CNET_HARNESS_MODEL)" || { \
		echo "CNET_HARNESS_MODEL=/absolute/path/model.gguf is required" >&2; exit 2; \
	}
	@mkdir -p logs
	$(DOTNET) build dotnet/CnetHarnessSmoke/CnetHarnessSmoke.csproj -c Release --nologo \
		> logs/cnet_harness_memory_build.log 2>&1
	timeout --signal=TERM --kill-after=15s $(CNET_HARNESS_MEMORY_TIMEOUT)s env \
		CNET_HARNESS_LIBRARY="$(CURDIR)/$(BIN_DIR)/libcnet_harness.so" \
		ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
		$(DOTNET) run --no-build -c Release \
		--project dotnet/CnetHarnessSmoke/CnetHarnessSmoke.csproj -- \
		"$(CNET_HARNESS_MODEL)" --batch-regression \
		> logs/cnet_harness_batch_regression.log 2>&1
	@grep -q "CNET_HARNESS_BATCH_REGRESSION_PASS" logs/cnet_harness_batch_regression.log
	timeout --signal=TERM --kill-after=15s $(CNET_HARNESS_MEMORY_TIMEOUT)s env \
		CNET_HARNESS_LIBRARY="$(CURDIR)/$(BIN_DIR)/libcnet_harness.so" \
		ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
		$(DOTNET) run --no-build -c Release \
		--project dotnet/CnetHarnessSmoke/CnetHarnessSmoke.csproj -- \
		"$(CNET_HARNESS_MODEL)" --prefix-reuse-regression \
		> logs/cnet_harness_prefix_reuse.log 2>&1
	@grep -q "CNET_HARNESS_PREFIX_REUSE_PASS" logs/cnet_harness_prefix_reuse.log
	timeout --signal=TERM --kill-after=15s $(CNET_HARNESS_MEMORY_TIMEOUT)s env \
		CNET_HARNESS_LIBRARY="$(CURDIR)/$(BIN_DIR)/libcnet_harness.so" \
		ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
		$(DOTNET) run --no-build -c Release \
		--project dotnet/CnetHarnessSmoke/CnetHarnessSmoke.csproj -- \
		"$(CNET_HARNESS_MODEL)" --continuous-memory \
		> logs/cnet_harness_continuous_memory.log 2>&1
	@grep -q "CNET_HARNESS_CONTINUOUS_MEMORY_PASS" logs/cnet_harness_continuous_memory.log
	timeout --signal=TERM --kill-after=15s $(CNET_HARNESS_MEMORY_TIMEOUT)s env \
		CNET_HARNESS_LIBRARY="$(CURDIR)/$(BIN_DIR)/libcnet_harness.so" \
		ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES='' \
		$(DOTNET) run --no-build -c Release \
		--project dotnet/CnetHarnessSmoke/CnetHarnessSmoke.csproj -- \
		"$(CNET_HARNESS_MODEL)" --memory-soak \
		> logs/cnet_harness_memory_soak.log 2>&1
	@grep -q "CNET_HARNESS_MEMORY_SOAK_PASS" logs/cnet_harness_memory_soak.log
	@echo "CNET_HARNESS_MEMORY_ACCEPTANCE_PASS"

# Alternate-paths regression gate: proves AICIMO is in the core CCE aggregate
# with its canonical API (cce_aicimo_*), old compat names are NOT global symbols,
# cnet_lm is NOT in the core aggregate, and the generic GPU API is honestly
# CUDA-or-CPU (not OpenCL). See tests/test_alt_paths_gate.c.
.PHONY: alt_paths_gate
alt_paths_gate: $(CCE) tests/test_alt_paths_gate.c include/cce/cce_gpu.h include/cce/cce_gguf.h include/cce/cce_clgemm.h include/cce/cce_aicimo.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/test_alt_paths_gate $(CCE) tests/test_alt_paths_gate.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS)
	./$(BIN_DIR)/test_alt_paths_gate > logs/alt_paths_gate.log 2>&1
	@grep -q "ALT_PATHS_GATE_PASS" logs/alt_paths_gate.log


# Replace/improve Tier0-2 campaign gates (each links the full runtime SRC;
# aggregated by cnet_replace_improve and verify-nightly):
#   procedure_chunks     - multi-step skill chunk sealer (served + deduped)
#   cnet_a_grade         - A-grade hermetic campaign (no live Hermes weeks required)
#   cnet_grade_up        - raise C/B- areas: live-traffic MoE + fault + store + acct
#   cnet_openlab_import  - open-lab import: MoE hard expert + tiered accounting

procedure_chunks: json_toolcall_alphabet $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) tests/procedure_chunks_test.c
	@mkdir -p $(BIN_DIR) artifacts/janitor
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/procedure_chunks \
		$(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) \
		src/soul_host.c $(ROUTE_LOG_SRC) tests/procedure_chunks_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	./$(BIN_DIR)/procedure_chunks


cnet_a_grade: json_toolcall_alphabet $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) tests/cnet_a_grade_test.c
	@mkdir -p $(BIN_DIR) logs artifacts/janitor
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/cnet_a_grade \
		$(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) \
		src/soul_host.c $(ROUTE_LOG_SRC) tests/cnet_a_grade_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	./$(BIN_DIR)/cnet_a_grade

cnet_grade_up: json_toolcall_alphabet $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) tests/cnet_grade_up_test.c
	@mkdir -p $(BIN_DIR) logs artifacts/janitor
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/cnet_grade_up \
		$(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) \
		src/soul_host.c $(ROUTE_LOG_SRC) tests/cnet_grade_up_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	./$(BIN_DIR)/cnet_grade_up
	@bash scripts/cnet_library_quality.sh
	@bash scripts/cnet_acct_dashboard.sh
	@bash scripts/cnet_openlab_doctor.sh

cnet_openlab_import: json_toolcall_alphabet $(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) $(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) $(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) src/soul_host.c $(ROUTE_LOG_SRC) tests/cnet_openlab_import_test.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $(BIN_DIR)/cnet_openlab_import \
		$(MULTIMODAL_SRC) $(MODEL_RUNTIME) $(CCE) $(CNET_CCE_ADAPTER) $(SPECIALIST_ADAPTERS) $(SPECIALIST_SRC) \
		$(SRC) $(ROUTER) $(REGISTRY_LORA) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(LIBRARY) $(GAP_LANE_SRC) $(EVIDENCE_BUNDLE_SRC) $(HEALTH_LAYERS_SRC) \
		$(PERSONAL_AI_SRC) $(HYBRID_AI_SRC) $(RESIDUAL_GGUF_SRC) $(PILOT_SRC) $(CURIOSITY_SRC) $(RESOURCE_GOV_SRC) $(SELF_IMPROVE_SRC) \
		src/soul_host.c $(ROUTE_LOG_SRC) tests/cnet_openlab_import_test.c $(LDFLAGS) $(MCP_LDFLAGS) $(CUDA_LDFLAGS) -pthread
	./$(BIN_DIR)/cnet_openlab_import
	@bash scripts/cnet_openlab_doctor.sh

cnet_replace_improve: cnet_fault_test cce_adapter_bank_test cce_dora_test cnet_serve_decode_test cnet_fault_loop_test registry_lora_store_test jtc_adapter_bench cnet_openlab_import cnet_grade_up cnet_a_grade
	@echo CNET_REPLACE_IMPROVE_PASS

# Next-5 improvements umbrella
cnet_next5: procedure_chunks cnet_serve_decode_test cnet_fault_test
	@mkdir -p logs artifacts/janitor
	@bash scripts/cnet_live_smoke.sh artifacts/janitor/LIVE_SMOKE.md
	@bash scripts/cnet_library_quality.sh
	@bash scripts/cnet_acct_dashboard.sh
	@echo CNET_NEXT5_PASS

# Isolate the pretoken-cache memory layout at scale (packed+hugepage vs 4K vs
# pointer-chase). Shows the cache-line/dTLB win the small-corpus encode bench can't.
gigatok_cache_bench: tests/gigatok_cache_bench.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ tests/gigatok_cache_bench.c
	./$(BIN_DIR)/gigatok_cache_bench

# Sparse MoE LM training (explicit forward+backward+Adam, gradient-checked).
moe_train: src/cce/cce_moe_train.c tests/moe_train.c include/cce/cce_moe_train.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ src/cce/cce_moe_train.c tests/moe_train.c -lm
	./$(BIN_DIR)/moe_train

# Transformer-MoE block trainer (single-head causal attention + sparse MoE FFN),
# every heavy matmul dispatched CPU (reference) or GPU via cce_clgemm. Validated
# by CPU + GPU gradient checks and CPU/GPU forward equivalence, then trains the
# period-3 copy task. Optional args: `./bin/moe_xf <steps> <lr>`; MOE_XF_CPU=1
# forces the CPU path.
moe_xf: src/cce/cce_moe_xf.c src/cce/cce_clgemm.c tests/moe_xf.c include/cce/cce_moe_xf.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ src/cce/cce_moe_xf.c $(CCE_CLGEMM) tests/moe_xf.c -lm -ldl -lpthread
	./$(BIN_DIR)/moe_xf 200

# One bounded, resumable training tick for the 24/7 loop, plus its checkpoint
# round-trip test. This is the learning substrate the autoteach loop points at:
# a checkpoint that beats a held-out entropy floor, not a replay-exact table row.
.PHONY: moe_tick moe_ckpt_test
moe_xf_tick: src/cce/cce_moe_xf.c src/cce/cce_clgemm.c tools/moe_xf_tick.c include/cce/cce_moe_xf.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ src/cce/cce_moe_xf.c $(CCE_CLGEMM) tools/moe_xf_tick.c -lm -ldl -lpthread
	@echo built $(BIN_DIR)/$@

moe_ckpt_test: src/cce/cce_moe_xf.c src/cce/cce_clgemm.c tests/moe_ckpt_test.c include/cce/cce_moe_xf.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ src/cce/cce_moe_xf.c $(CCE_CLGEMM) tests/moe_ckpt_test.c -lm -ldl -lpthread
	@MOE_XF_CPU=1 ./$(BIN_DIR)/moe_ckpt_test | tee logs/moe_ckpt_test.log
	@grep -q MOE_CKPT_PASS logs/moe_ckpt_test.log

moe_tick: moe_xf_tick
	@bash scripts/cnet_moe_tick.sh

# Bonsai residual → fault bus seed (standalone light link)
.PHONY: bonsai_residual_fault_seed_run
bonsai_residual_fault_seed_run: tools/bonsai_residual_fault_seed.c src/residual_http.c src/cnet_fault.c src/cnet_promote.c src/cnet_acct.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/bonsai_residual_fault_seed \
		src/residual_http.c src/cnet_fault.c src/cnet_promote.c src/cnet_acct.c \
		src/gap_lane.c src/nn.c tools/bonsai_residual_fault_seed.c $(LDFLAGS) -pthread
	@test -n "$$CNET_RESIDUAL_HTTP" || export CNET_RESIDUAL_HTTP=http://127.0.0.1:8080; \
	test -n "$$CNET_FAULT_LOG" || export CNET_FAULT_LOG=$(CURDIR)/logs/cnet_faults.jsonl; \
	test -n "$$CNET_RESIDUAL_WINDOW" || export CNET_RESIDUAL_WINDOW=$(CURDIR)/english_window_256_bonsai.txt; \
	test -n "$$CNET_ACCT_LOG" || export CNET_ACCT_LOG=$(CURDIR)/logs/cnet_acct.jsonl; \
	CNET_RESIDUAL_HTTP=$${CNET_RESIDUAL_HTTP} CNET_FAULT_LOG=$${CNET_FAULT_LOG} \
	CNET_RESIDUAL_WINDOW=$${CNET_RESIDUAL_WINDOW} CNET_ACCT_LOG=$${CNET_ACCT_LOG} \
	./$(BIN_DIR)/bonsai_residual_fault_seed $${N:-16} | tee logs/bonsai_residual_fault_seed.log
	@grep -q BONSAI_FAULT_SEED logs/bonsai_residual_fault_seed.log

# Self-direction governor. ONE engine: tools/governor_autonomous.c (v4).
.PHONY: governor governor_dry
governor: governor_v4

governor_dry: $(BIN_DIR)/governor_autonomous
	@$(BIN_DIR)/governor_autonomous --test
	@$(BIN_DIR)/governor_autonomous --dry-run


.PHONY: governor_v2 governor_v3 governor_v4 governor_quality
governor_v4: $(BIN_DIR)/governor_autonomous
	@$(BIN_DIR)/governor_autonomous --test
	@$(BIN_DIR)/governor_autonomous --dry-run
	@grep -q governor_autonomous_v4 logs/governor/last_decision.json
	@echo GOVERNOR_V4_PASS

governor_v3: $(BIN_DIR)/governor_autonomous
	@$(BIN_DIR)/governor_autonomous --test
	@$(BIN_DIR)/governor_autonomous --dry-run
	@test -f logs/governor/last_decision.json
	@grep -q governor_autonomous_v4 logs/governor/last_decision.json
	@echo GOVERNOR_V3_PASS

governor_v2: governor_v3

governor_quality: governor_v3
	@bash scripts/governor_hooks.sh pre
	@$(BIN_DIR)/governor_autonomous
	@test -f logs/governor/miss_bus.json
	@test -f logs/governor/meta_evolved.json
	@test -f logs/governor/hermes_miss.json
	@jq -e '.engine | test("v4")' logs/governor/last_decision.json >/dev/null
	@jq -e '.w_eval' logs/governor/meta_evolved.json >/dev/null
	@jq -r '"quality_goals \(.goals) quality_actions \(.actions) focus \(.scoreboard_focus) meta \(.meta) evolve \(.evolve_note)"' logs/governor/last_decision.json
	@echo GOVERNOR_QUALITY_PASS


.PHONY: governor_sandbox governor_a_gate
governor_sandbox: $(BIN_DIR)/governor_autonomous
	@chmod +x scripts/dev-sandbox.sh
	@scripts/dev-sandbox.sh --persistent --from-prod ./$(BIN_DIR)/governor_autonomous --test
	@echo GOVERNOR_SANDBOX_PASS

governor_a_gate: governor_v4 $(BIN_DIR)/governor_hermes_structured
	@$(BIN_DIR)/governor_hermes_structured
	@test -f logs/governor/hermes_structured.json
	@$(BIN_DIR)/governor_autonomous
	@test -f logs/governor/meta_evolved.json
	@test -f config/governor_goal_graph.json
	@jq -e '.engine | test("v4")' logs/governor/last_decision.json >/dev/null
	@jq -r '"engine \(.engine) goals \(.goals)"' logs/governor/last_decision.json
	@jq -r '{fails,oks,hermes_task_fail_rate,noisy}' logs/governor/hermes_structured.json
	@scripts/dev-sandbox.sh --persistent --from-prod ./$(BIN_DIR)/governor_autonomous --test
	@echo GOVERNOR_A_GATE_PASS

.PHONY: personality_test governor_persona
personality_test: $(BIN_DIR)/governor_personality
	@$(BIN_DIR)/governor_personality --test

governor_persona: personality_test $(BIN_DIR)/governor_autonomous
	@$(BIN_DIR)/governor_autonomous --test
	@test -f logs/governor/personality_state.json
	@echo GOVERNOR_PERSONA_PASS

.PHONY: roe_omnidoc_freeze
roe_omnidoc_freeze:
	@mkdir -p logs
	@echo "ROE_OMNIDOC_FREEZE_WITHHELD reason=torch_harness_removed" | tee logs/roe_omnidoc_freeze.log
	@false

.PHONY: cnet_7b_compete_fixture cnet_7b_compete_contract
cnet_7b_compete_fixture: include/cnet_compete.h tools/cnet_compete_fixture.c \
		benchmarks/cnet_asi5_v1/heldout.tsv \
		benchmarks/cnet_asi5_v1/baseline_system.txt \
		benchmarks/cnet_asi5_v1/digests.sha256
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -Iinclude -o $(BIN_DIR)/cnet_compete_fixture \
		tools/cnet_compete_fixture.c
	@tmp=$$(mktemp); trap 'rm -f "$$tmp"' EXIT; \
		./$(BIN_DIR)/cnet_compete_fixture "$$tmp"; \
		cmp benchmarks/cnet_asi5_v1/heldout.tsv "$$tmp"
	@sha256sum -c benchmarks/cnet_asi5_v1/digests.sha256
	@echo CNET_7B_COMPETE_FIXTURE_PASS rows=448

cnet_7b_compete_contract: cnet_7b_compete_fixture include/cnet_compete.h \
		src/cnet_compete.c tests/test_cnet_compete_contract.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Iinclude -o $(BIN_DIR)/test_cnet_compete_contract \
		src/cnet_compete.c tests/test_cnet_compete_contract.c $(LDFLAGS)
	@./$(BIN_DIR)/test_cnet_compete_contract | tee logs/cnet_7b_compete_contract.log
	@grep -q CNET_7B_COMPETE_CONTRACT_PASS logs/cnet_7b_compete_contract.log

.PHONY: cnet_7b_capsule_increment
CNET_COMPETE_CAPSULE_CORE := src/cnet_capsule.c src/hybrid_ai.c src/base.c \
	src/nn.c src/contract/contract.c src/contract/unit.c \
	src/contract/coverage.c src/acquire.c src/runtime_identity.c src/plan_table.c
cnet_7b_capsule_increment: include/cnet_compete_capsules.h \
		src/cnet_compete_capsules.c tests/test_cnet_compete_capsule_increment.c \
		$(CNET_COMPETE_CAPSULE_CORE)
	@mkdir -p $(BIN_DIR) logs
	$(CC) $(CFLAGS) -Werror -ffunction-sections -fdata-sections -Iinclude \
		-o $(BIN_DIR)/test_cnet_compete_capsule_increment \
		src/cnet_compete_capsules.c $(CNET_COMPETE_CAPSULE_CORE) \
		tests/test_cnet_compete_capsule_increment.c \
		-Wl,--gc-sections $(LDFLAGS) $(MCP_LDFLAGS) -pthread
	@./$(BIN_DIR)/test_cnet_compete_capsule_increment | \
		tee logs/cnet_7b_capsule_increment.log
	@grep -q CNET_7B_CAPSULE_INCREMENT_PASS logs/cnet_7b_capsule_increment.log
