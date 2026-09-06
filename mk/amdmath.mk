# mk/amdmath.mk -- AMD GPU math library (HIP + rocWMMA + packed VOP).
#
# Separate from libcce.a: device code must be compiled with hipcc.
# Not CERT. Not cce_hipgemm (forward-only hipBLAS).
#
#   make amdmath          → bin/libcnet_amdmath.a
#   make amdmath_test     → hermetic GPU tests (AMDMATH_PASS)
#   make amdmath_bench    → CPU vs FMA vs WMMA
#   make amdmath_vram     → 3-tier HOT/WARM/COLD train residency sweep

HIPCC ?= hipcc
HIP_ARCH ?= gfx1201
AMDMATH_SRC := src/cce/amdmath/cce_amdmath.cpp
AMDMATH_HDR := include/cce/cce_amdmath.h
AMDMATH_OBJ := build/amdmath/cce_amdmath.o
LIBAMDMATH := $(BIN_DIR)/libcnet_amdmath.a

HIPCC_FLAGS := -O3 -std=c++17 -fPIC --offload-arch=$(HIP_ARCH) -Iinclude

$(AMDMATH_OBJ): $(AMDMATH_SRC) $(AMDMATH_HDR)
	@mkdir -p $(dir $@)
	$(HIPCC) $(HIPCC_FLAGS) -c -o $@ $(AMDMATH_SRC)

$(LIBAMDMATH): $(AMDMATH_OBJ)
	@mkdir -p $(BIN_DIR)
	$(AR) rcs $@ $(AMDMATH_OBJ)
	@echo "Built $@"

.PHONY: amdmath amdmath_test amdmath_bench amdmath_vram

amdmath: $(LIBAMDMATH)

amdmath_test: $(AMDMATH_OBJ) tests/test_cce_amdmath.c $(AMDMATH_HDR)
	@mkdir -p $(BIN_DIR) logs
	$(HIPCC) -O3 --offload-arch=$(HIP_ARCH) -Iinclude -o $(BIN_DIR)/test_cce_amdmath \
		-x c tests/test_cce_amdmath.c -x none $(AMDMATH_OBJ) -lm
	HIP_VISIBLE_DEVICES=0 $(BIN_DIR)/test_cce_amdmath | tee logs/amdmath_test.log
	grep -q AMDMATH_PASS logs/amdmath_test.log

amdmath_bench: $(AMDMATH_OBJ) tools/cnet_amdmath_bench.c $(AMDMATH_HDR)
	@mkdir -p $(BIN_DIR) logs
	$(HIPCC) -O3 --offload-arch=$(HIP_ARCH) -Iinclude -o $(BIN_DIR)/cnet_amdmath_bench \
		-x c tools/cnet_amdmath_bench.c -x none $(AMDMATH_OBJ) -lm
	HIP_VISIBLE_DEVICES=0 $(BIN_DIR)/cnet_amdmath_bench 1024 1024 1024 3 | tee logs/amdmath_bench.log
	grep -q AMDMATH_BENCH_PASS logs/amdmath_bench.log

amdmath_vram: $(AMDMATH_OBJ) tools/cnet_amdmath_vram_bench.c $(AMDMATH_HDR)
	@mkdir -p $(BIN_DIR) logs
	$(HIPCC) -O3 --offload-arch=$(HIP_ARCH) -Iinclude -o $(BIN_DIR)/cnet_amdmath_vram_bench \
		-x c tools/cnet_amdmath_vram_bench.c -x none $(AMDMATH_OBJ) -lm
	HIP_VISIBLE_DEVICES=0 $(BIN_DIR)/cnet_amdmath_vram_bench 256 1024 128 80 | tee logs/amdmath_vram.log
	grep -q AMDMATH_VRAM_PASS logs/amdmath_vram.log

.PHONY: amdmath_dual
amdmath_dual: $(AMDMATH_OBJ) tools/cnet_amdmath_dual_runtime.c $(AMDMATH_HDR)
	@mkdir -p $(BIN_DIR)
	$(HIPCC) -O3 --offload-arch=$(HIP_ARCH) -Iinclude -o $(BIN_DIR)/cnet_amdmath_dual_runtime \
		-x c tools/cnet_amdmath_dual_runtime.c -x none $(AMDMATH_OBJ) -lm

# CPU n-gram hash table (not GPU, not CERT). Sibling of amdmath.
.PHONY: ngram_test
ngram_test: src/cce/cce_ngram.c include/cce/cce_ngram.h tests/test_cce_ngram.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -O2 -Iinclude -o $(BIN_DIR)/test_cce_ngram \
		tests/test_cce_ngram.c src/cce/cce_ngram.c
	$(BIN_DIR)/test_cce_ngram | tee logs/ngram_test.log
	grep -q NGRAM_PASS logs/ngram_test.log

.PHONY: hybrid_train
hybrid_train: tools/cnet_hybrid_train.c src/cce/cce_wordlm.c src/cce/cce_ngram.c \
		include/cce/cce_wordlm.h include/cce/cce_ngram.h
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -O2 -Iinclude -o $(BIN_DIR)/cnet_hybrid_train \
		tools/cnet_hybrid_train.c src/cce/cce_wordlm.c src/cce/cce_ngram.c -lm
	$(BIN_DIR)/cnet_hybrid_train | tee logs/hybrid_train.log
	grep -q HYBRID_TRAIN_PASS logs/hybrid_train.log

.PHONY: hybrid_gpu
hybrid_gpu: $(AMDMATH_OBJ) tools/cnet_hybrid_gpu_train.c src/cce/cce_ngram.c \
		include/cce/cce_amdmath.h include/cce/cce_ngram.h
	@mkdir -p $(BIN_DIR) logs
	$(HIPCC) -O3 --offload-arch=$(HIP_ARCH) -Iinclude -o $(BIN_DIR)/cnet_hybrid_gpu_train \
		-x c tools/cnet_hybrid_gpu_train.c src/cce/cce_ngram.c -x none $(AMDMATH_OBJ) -lm
	HIP_VISIBLE_DEVICES=0 $(BIN_DIR)/cnet_hybrid_gpu_train 1024 128 8192 3 | tee logs/hybrid_gpu.log
	grep -q HYBRID_GPU_PASS logs/hybrid_gpu.log

.PHONY: xfmr_test xfmr_gpu
xfmr_test: src/cce/cce_xfmr.c include/cce/cce_xfmr.h tests/test_cce_xfmr.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -O2 -DCCE_XFMR_CPU_ONLY -Iinclude -o $(BIN_DIR)/test_cce_xfmr \
		tests/test_cce_xfmr.c src/cce/cce_xfmr.c -lm
	$(BIN_DIR)/test_cce_xfmr | tee logs/xfmr_test.log
	grep -q XFMR_PASS logs/xfmr_test.log

xfmr_gpu: $(AMDMATH_OBJ) src/cce/cce_xfmr.c include/cce/cce_xfmr.h tests/test_cce_xfmr.c
	@mkdir -p $(BIN_DIR) logs
	$(HIPCC) -O3 --offload-arch=$(HIP_ARCH) -Iinclude -o $(BIN_DIR)/test_cce_xfmr_gpu \
		-x c tests/test_cce_xfmr.c src/cce/cce_xfmr.c -x none $(AMDMATH_OBJ) -lm
	HIP_VISIBLE_DEVICES=0 $(BIN_DIR)/test_cce_xfmr_gpu | tee logs/xfmr_gpu.log
	grep -q XFMR_PASS logs/xfmr_gpu.log

.PHONY: maptrain_test maptrain_gpu maptrain_bench
maptrain_test: src/cce/cce_maptrain.c include/cce/cce_maptrain.h tests/test_cce_maptrain.c
	@mkdir -p $(BIN_DIR) logs
	$(CC) -std=c11 -Wall -Wextra -O2 -DCCE_MAPTRAIN_CPU_ONLY -Iinclude -o $(BIN_DIR)/test_cce_maptrain \
		tests/test_cce_maptrain.c src/cce/cce_maptrain.c -lm
	$(BIN_DIR)/test_cce_maptrain | tee logs/maptrain_test.log
	grep -q MAPTRAIN_PASS logs/maptrain_test.log

maptrain_gpu: $(AMDMATH_OBJ) src/cce/cce_maptrain.c include/cce/cce_maptrain.h tests/test_cce_maptrain.c
	@mkdir -p $(BIN_DIR) logs
	$(HIPCC) -O3 --offload-arch=$(HIP_ARCH) -Iinclude -o $(BIN_DIR)/test_cce_maptrain_gpu \
		-x c tests/test_cce_maptrain.c src/cce/cce_maptrain.c -x none $(AMDMATH_OBJ) -lm
	HIP_VISIBLE_DEVICES=0 $(BIN_DIR)/test_cce_maptrain_gpu | tee logs/maptrain_gpu.log
	grep -q MAPTRAIN_PASS logs/maptrain_gpu.log

maptrain_bench: $(AMDMATH_OBJ) src/cce/cce_maptrain.c tools/cnet_maptrain_bench.c
	@mkdir -p $(BIN_DIR) logs
	$(HIPCC) -O3 --offload-arch=$(HIP_ARCH) -Iinclude -o $(BIN_DIR)/cnet_maptrain_bench \
		-x c tools/cnet_maptrain_bench.c src/cce/cce_maptrain.c -x none $(AMDMATH_OBJ) -lm
	HIP_VISIBLE_DEVICES=0 $(BIN_DIR)/cnet_maptrain_bench | tee logs/maptrain_bench.log
	grep -q MAPTRAIN_BENCH_PASS logs/maptrain_bench.log
