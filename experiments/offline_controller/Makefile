CC ?= cc
ROOT := ../..
CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -I$(ROOT)/include
OUT ?= /tmp/cnet-controller-build
HIPCC ?= hipcc
.PHONY: test
test: $(OUT)/test_fixture $(OUT)/test_net
	$(OUT)/test_fixture
	$(OUT)/test_net
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
