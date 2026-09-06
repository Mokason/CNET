CC ?= cc
ROOT := ../..
CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -I$(ROOT)/include
OUT ?= /tmp/cnet-controller-build
HIPCC ?= hipcc
.PHONY: test
test: $(OUT)/test_fixture $(OUT)/test_net $(OUT)/test_local
	$(OUT)/test_fixture
	$(OUT)/test_net
	$(OUT)/test_local
$(OUT)/test_fixture: fixture.c fixture.h test_fixture.c
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) test_fixture.c fixture.c -o $@
$(OUT)/test_net: net.c net.h fixture.c test_net.c
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DCONTROLLER_CPU_ONLY test_net.c net.c fixture.c -lm -o $@
$(OUT)/amdmath.o: $(ROOT)/src/cce/amdmath/cce_amdmath.cpp $(ROOT)/include/cce/cce_amdmath.h
	mkdir -p $(OUT)
	$(HIPCC) -O2 -std=c++17 --offload-arch=gfx1201 -I$(ROOT)/include -c $< -o $@
$(OUT)/test_gpu: test_gpu.c net.c fixture.c offline.c net.h offline.h $(OUT)/amdmath.o
	$(HIPCC) -O2 --offload-arch=gfx1201 -I$(ROOT)/include -x c test_gpu.c net.c fixture.c offline.c -x none $(OUT)/amdmath.o -lm -o $@
.PHONY: gpu-test
gpu-test: $(OUT)/test_gpu
	$(OUT)/test_gpu
$(OUT)/controller: run.c net.c fixture.c offline.c net.h fixture.h offline.h $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include -x c run.c net.c fixture.c offline.c -x none $(OUT)/amdmath.o -lm -o $@
.PHONY: build
build: $(OUT)/controller
$(OUT)/local_baseline: local_baseline.c fixture.c offline.c fixture.h offline.h $(ROOT)/include/cnet_json_internal.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) local_baseline.c fixture.c offline.c -o $@
.PHONY: local-build
local-build: $(OUT)/local_baseline
.PHONY: investigate-build investigate-test
$(OUT)/test_investigate: test_investigate.c investigate.c net.c fixture.c offline.c net.h fixture.h offline.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DCONTROLLER_CPU_ONLY test_investigate.c net.c fixture.c offline.c -lm -o $@
investigate-test: $(OUT)/test_investigate
	$(OUT)/test_investigate
$(OUT)/investigate: investigate.c net.c fixture.c offline.c net.h fixture.h offline.h $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include -x c investigate.c net.c fixture.c offline.c -x none $(OUT)/amdmath.o -lm -o $@
investigate-build: $(OUT)/investigate
.PHONY: investigate-benchmark
$(OUT)/bench_train: bench_train.c net.c fixture.c net.h fixture.h $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include -x c bench_train.c net.c fixture.c -x none $(OUT)/amdmath.o -lm -o $@
investigate-benchmark: $(OUT)/bench_train
.PHONY: investigate-confirm
$(OUT)/confirm: confirm.c investigate.c net.c fixture.c offline.c net.h fixture.h offline.h $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include -x c confirm.c net.c fixture.c offline.c -x none $(OUT)/amdmath.o -lm -lcrypto -o $@
investigate-confirm: $(OUT)/confirm
$(OUT)/test_local: test_local.c local_baseline.c offline.c fixture.c offline.h fixture.h $(ROOT)/include/cnet_json_internal.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) test_local.c fixture.c offline.c -o $@
