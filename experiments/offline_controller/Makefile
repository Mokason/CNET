CC ?= cc
ROOT := ../..
CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -I$(ROOT)/include
OUT ?= /tmp/cnet-controller-build
HIPCC ?= hipcc
.DEFAULT_GOAL := test
# Direct entry points must also work with a new OUT directory. Directory mtimes
# are order-only prerequisites so creating another binary never forces a rebuild.
$(OUT):
	mkdir -p "$@"
$(OUT)/cell_train_test.o $(OUT)/cell_train.o $(OUT)/test_cell \
$(OUT)/test_selector $(OUT)/selector_experiment $(OUT)/test_cell_capsule \
$(OUT)/test_candidate $(OUT)/test_host $(OUT)/test_worker_sandbox \
$(OUT)/gpu_worker $(OUT)/gpu_worker_test $(OUT)/test_workers \
$(OUT)/bench_cell_serving: | $(OUT)
.PHONY: test
test: $(OUT)/test_fixture $(OUT)/test_net $(OUT)/test_local worker-boundary-test
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
.PHONY: investigate-data-profile
$(OUT)/profile_data: profile_data.c investigate.c net.c fixture.c offline.c net.h fixture.h offline.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DCONTROLLER_CPU_ONLY profile_data.c net.c fixture.c offline.c -lm -o $@
investigate-data-profile: $(OUT)/profile_data
.PHONY: resident-build resident-test
$(OUT)/resident.o: resident.cpp resident.h net.h fixture.h
	mkdir -p $(OUT)
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include -c resident.cpp -o $@
$(OUT)/resident_test.o: resident.cpp resident.h net.h fixture.h
	mkdir -p $(OUT)
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -DCONTROLLER_TESTING -I$(ROOT)/include -c resident.cpp -o $@
$(OUT)/test_resident: test_resident.c resident.h net.c fixture.c $(OUT)/resident_test.o $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -DCONTROLLER_TESTING -I$(ROOT)/include -x c test_resident.c net.c fixture.c -x none $(OUT)/resident_test.o $(OUT)/amdmath.o -lm -o $@
resident-build: $(OUT)/test_resident
resident-test: $(OUT)/test_resident
	$(OUT)/test_resident
.PHONY: resident-benchmark
$(OUT)/bench_resident: bench_resident.c resident.h net.c fixture.c $(OUT)/resident.o $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include -x c bench_resident.c net.c fixture.c -x none $(OUT)/resident.o $(OUT)/amdmath.o -lm -o $@
$(OUT)/diagnose_resident: diagnose_resident.c resident.h net.c fixture.c $(OUT)/resident.o $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include -x c diagnose_resident.c net.c fixture.c -x none $(OUT)/resident.o $(OUT)/amdmath.o -lm -o $@
$(OUT)/profile_resident: profile_resident.c resident.h net.c fixture.c $(OUT)/resident.o $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include -x c profile_resident.c net.c fixture.c -x none $(OUT)/resident.o $(OUT)/amdmath.o -lm -o $@
resident-benchmark: $(OUT)/bench_resident $(OUT)/diagnose_resident $(OUT)/profile_resident
.PHONY: stream-test
$(OUT)/test_stream: test_stream.cpp $(ROOT)/include/cce/cce_amdmath.h $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include $< -x none $(OUT)/amdmath.o -o $@
stream-test: $(OUT)/test_stream
	$(OUT)/test_stream
.PHONY: resident-train
$(OUT)/train_resident: train_resident.c investigate.c resident.h net.c fixture.c offline.c $(OUT)/resident.o $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include -x c train_resident.c net.c fixture.c offline.c -x none $(OUT)/resident.o $(OUT)/amdmath.o -lm -o $@
resident-train: $(OUT)/train_resident
OPENBLAS_CFLAGS ?= $(shell pkg-config --cflags openblas)
OPENBLAS_LIBS ?= $(shell pkg-config --libs openblas)
.PHONY: optimized-benchmark
$(OUT)/blas_reference.o: blas_reference.c net.c net.h
	$(CC) $(CFLAGS) $(OPENBLAS_CFLAGS) -c $< -o $@
$(OUT)/bench_optimized: bench_resident.c net.c fixture.c $(OUT)/blas_reference.o $(OUT)/resident.o $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -DCONTROLLER_OPENBLAS -I$(ROOT)/include $(OPENBLAS_CFLAGS) -x c bench_resident.c net.c fixture.c -x none $(OUT)/blas_reference.o $(OUT)/resident.o $(OUT)/amdmath.o $(OPENBLAS_LIBS) -lm -o $@
optimized-benchmark: $(OUT)/bench_optimized
.PHONY: matrix-benchmark
$(OUT)/bench_matrix: bench_matrix.cpp $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include $(OPENBLAS_CFLAGS) $< -x none $(OUT)/amdmath.o $(OPENBLAS_LIBS) -lrocblas -o $@
matrix-benchmark: $(OUT)/bench_matrix
.PHONY: rocblas-test rocblas-benchmark rocblas-train
$(OUT)/resident_rocblas_test.o: resident.cpp resident.h net.h fixture.h
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -DCONTROLLER_TESTING -DCONTROLLER_ROCBLAS -I$(ROOT)/include -c $< -o $@
$(OUT)/test_resident_rocblas: test_resident.c net.c fixture.c $(OUT)/resident_rocblas_test.o $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -DCONTROLLER_TESTING -DCONTROLLER_ROCBLAS -I$(ROOT)/include -x c test_resident.c net.c fixture.c -x none $(OUT)/resident_rocblas_test.o $(OUT)/amdmath.o -lrocblas -lm -o $@
rocblas-test: $(OUT)/test_resident_rocblas
	$(OUT)/test_resident_rocblas
$(OUT)/resident_rocblas.o: resident.cpp resident.h net.h fixture.h
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -DCONTROLLER_ROCBLAS -I$(ROOT)/include -c $< -o $@
$(OUT)/bench_resident_rocblas: bench_resident.c net.c fixture.c $(OUT)/blas_reference.o $(OUT)/resident_rocblas.o $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -DCONTROLLER_OPENBLAS -I$(ROOT)/include $(OPENBLAS_CFLAGS) -x c bench_resident.c net.c fixture.c -x none $(OUT)/blas_reference.o $(OUT)/resident_rocblas.o $(OUT)/amdmath.o $(OPENBLAS_LIBS) -lrocblas -lm -o $@
rocblas-benchmark: $(OUT)/bench_resident_rocblas
$(OUT)/train_resident_rocblas: train_resident.c investigate.c net.c fixture.c offline.c $(OUT)/resident_rocblas.o $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include -x c train_resident.c net.c fixture.c offline.c -x none $(OUT)/resident_rocblas.o $(OUT)/amdmath.o -lrocblas -lm -o $@
rocblas-train: $(OUT)/train_resident_rocblas
$(OUT)/profile_resident_rocblas: profile_resident.c net.c fixture.c $(OUT)/resident_rocblas.o $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include -x c profile_resident.c net.c fixture.c -x none $(OUT)/resident_rocblas.o $(OUT)/amdmath.o -lrocblas -lm -o $@
.PHONY: cell-test
$(OUT)/cell_train_test.o: cell_train.cpp cell_train.h $(ROOT)/include/cnet_core_cell.h
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -DCONTROLLER_TESTING -I$(ROOT)/include -c $< -o $@
$(OUT)/cell_train.o: cell_train.cpp cell_train.h $(ROOT)/include/cnet_core_cell.h
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include -c $< -o $@
$(OUT)/test_cell: test_cell.c $(ROOT)/src/serve/cnet_core_cell.c $(OUT)/cell_train_test.o $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -DCONTROLLER_TESTING -I$(ROOT)/include -x c test_cell.c $(ROOT)/src/serve/cnet_core_cell.c -x none $(OUT)/cell_train_test.o $(OUT)/amdmath.o -lm -o $@
cell-test: $(OUT)/test_cell
	$(OUT)/test_cell
.PHONY: selector-test
$(OUT)/test_selector: test_selector.c $(ROOT)/src/serve/cnet_core_cell.c $(ROOT)/src/serve/cnet_core_selector.c $(ROOT)/include/cnet_core_selector.h
	$(CC) $(CFLAGS) test_selector.c $(ROOT)/src/serve/cnet_core_cell.c $(ROOT)/src/serve/cnet_core_selector.c -lm -o $@
selector-test: $(OUT)/test_selector
	$(OUT)/test_selector
.PHONY: selector-experiment
$(OUT)/selector_experiment: selector_experiment.c selector_fixture.c selector_fixture.h offline.c $(ROOT)/src/serve/cnet_core_cell.c $(ROOT)/src/serve/cnet_core_selector.c $(OUT)/cell_train.o $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include -x c selector_experiment.c selector_fixture.c offline.c $(ROOT)/src/serve/cnet_core_cell.c $(ROOT)/src/serve/cnet_core_selector.c -x none $(OUT)/cell_train.o $(OUT)/amdmath.o -lm -o $@
selector-experiment: $(OUT)/selector_experiment
.PHONY: cell-capsule
$(OUT)/test_cell_capsule: test_cell_capsule.c $(ROOT)/src/serve/cnet_cell_capsule.c $(ROOT)/src/serve/cnet_core_cell.c $(ROOT)/include/cnet_cell_capsule.h $(ROOT)/bin/libcnet_capsule_core.so
	$(CC) $(CFLAGS) test_cell_capsule.c $(ROOT)/src/serve/cnet_cell_capsule.c $(ROOT)/src/serve/cnet_core_cell.c -L$(ROOT)/bin -lcnet_capsule_core -Wl,-rpath,$(abspath $(ROOT)/bin) -lm -o $@
cell-capsule: $(OUT)/test_cell_capsule
.PHONY: candidate-test
$(OUT)/test_candidate: test_candidate.c $(ROOT)/src/serve/cnet_core_candidate.c $(ROOT)/src/serve/cnet_core_cell.c $(ROOT)/include/cnet_core_candidate.h $(ROOT)/src/cce/cce_campaign_provenance.c
	$(CC) $(CFLAGS) test_candidate.c $(ROOT)/src/serve/cnet_core_candidate.c $(ROOT)/src/serve/cnet_core_cell.c $(ROOT)/src/cce/cce_campaign_provenance.c -lm -o $@
candidate-test: $(OUT)/test_candidate
	$(OUT)/test_candidate
.PHONY: host-test-build
$(OUT)/test_host: test_host.c selector_fixture.c $(ROOT)/bin/libcnet_capsule_core.so $(ROOT)/include/cnet_core_host.h
	$(CC) $(CFLAGS) test_host.c selector_fixture.c -L$(ROOT)/bin -lcnet_capsule_core -Wl,-rpath,$(abspath $(ROOT)/bin) -lm -pthread -o $@
host-test-build: $(OUT)/test_host
.PHONY: worker-build worker-sandbox-test
$(OUT)/test_worker_sandbox: test_worker_sandbox.c worker_sandbox.c worker_sandbox.h offline.c
	$(CC) $(CFLAGS) test_worker_sandbox.c worker_sandbox.c offline.c -o $@
worker-sandbox-test: $(OUT)/test_worker_sandbox
	$(OUT)/test_worker_sandbox
$(OUT)/gpu_worker: worker_main.c worker.c worker.h worker_sandbox.c worker_sandbox.h offline.c $(ROOT)/src/serve/cnet_core_cell.c $(OUT)/cell_train.o $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include -x c worker_main.c worker.c worker_sandbox.c offline.c $(ROOT)/src/serve/cnet_core_cell.c -x none $(OUT)/cell_train.o $(OUT)/amdmath.o -lm -lrt -o $@
	chmod go-w $@
$(OUT)/gpu_worker_test: worker_main.c worker.c worker.h worker_sandbox.c worker_sandbox.h offline.c $(ROOT)/src/serve/cnet_core_cell.c $(OUT)/cell_train.o $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -DCONTROLLER_TESTING -I$(ROOT)/include -x c worker_main.c worker.c worker_sandbox.c offline.c $(ROOT)/src/serve/cnet_core_cell.c -x none $(OUT)/cell_train.o $(OUT)/amdmath.o -lm -lrt -o $@
	chmod go-w $@
$(OUT)/test_workers: test_workers.c worker.c worker.h $(ROOT)/src/serve/cnet_core_cell.c
	$(CC) $(CFLAGS) -DCONTROLLER_TESTING test_workers.c worker.c $(ROOT)/src/serve/cnet_core_cell.c -lm -o $@
worker-build: $(OUT)/gpu_worker $(OUT)/gpu_worker_test $(OUT)/test_workers
.PHONY: worker-boundary-test
$(OUT)/test_worker_boundary: test_worker_boundary.c worker.c worker.h $(ROOT)/src/serve/cnet_core_cell.c | $(OUT)
	$(CC) $(CFLAGS) -DCONTROLLER_TESTING test_worker_boundary.c worker.c $(ROOT)/src/serve/cnet_core_cell.c -Wl,--wrap=clock_gettime,--wrap=timer_create,--wrap=timer_settime -lm -pthread -lrt -o $@
	chmod go-w $@
worker-boundary-test: $(OUT)/test_worker_boundary
	$(OUT)/test_worker_boundary
.PHONY: product-test
$(OUT)/bench_cell_serving: bench_cell_serving.c $(ROOT)/bin/libcnet_capsule_core.so
	$(CC) $(CFLAGS) bench_cell_serving.c -L$(ROOT)/bin -lcnet_capsule_core -Wl,-rpath,$(abspath $(ROOT)/bin) -lm -o $@
# Build the parent library first, before recursively resolving test prerequisites.
product-test:
	$(MAKE) -C $(ROOT) capsule_core
	$(MAKE) $(OUT)/test_cell $(OUT)/test_selector $(OUT)/test_candidate $(OUT)/test_host $(OUT)/test_cell_capsule $(OUT)/test_worker_sandbox worker-build $(OUT)/bench_cell_serving
	bash run_product_tests.sh $(abspath $(OUT))
$(OUT)/test_local: test_local.c local_baseline.c offline.c fixture.c offline.h fixture.h $(ROOT)/include/cnet_json_internal.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) test_local.c fixture.c offline.c -o $@

# Allocator objective: CPU build/tests never depend on HIP or launch a worker.
# AMD compilation and execution remain separate, explicit opt-ins.
ALLOCATOR_CORE_SOURCES := $(ROOT)/src/serve/cnet_core_allocator.c $(ROOT)/src/serve/cnet_core_cell.c
ALLOCATOR_CHECKPOINT_SOURCES := $(ROOT)/src/serve/cnet_core_candidate.c $(ROOT)/src/cce/cce_campaign_provenance.c
ALLOCATOR_HEADERS := $(ROOT)/include/cnet_core_allocator.h $(ROOT)/include/cnet_core_cell.h \
	$(ROOT)/include/cnet_core_candidate.h $(ROOT)/include/cce/cce_campaign_provenance.h
$(OUT)/allocator $(OUT)/allocator_campaign $(OUT)/allocator_test \
$(OUT)/allocator_data_test $(OUT)/allocator_gate_test \
$(OUT)/allocator_worker $(OUT)/allocator_worker_test: | $(OUT)

.PHONY: allocator-build allocator-test allocator-worker-build allocator-worker-test
$(OUT)/allocator: allocator_cli.c allocator_data.c allocator_data.h worker.c worker.h $(ALLOCATOR_CORE_SOURCES) $(ALLOCATOR_CHECKPOINT_SOURCES) $(ALLOCATOR_HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -lm -o $@
$(OUT)/allocator_campaign: allocator_campaign.c allocator_data.c allocator_data.h $(ALLOCATOR_CORE_SOURCES) $(ALLOCATOR_CHECKPOINT_SOURCES) $(ALLOCATOR_HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -lm -o $@
allocator-build: $(OUT)/allocator $(OUT)/allocator_campaign

$(OUT)/allocator_test: allocator_test.c $(ALLOCATOR_CORE_SOURCES) $(ALLOCATOR_CHECKPOINT_SOURCES) $(ALLOCATOR_HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -lm -o $@
$(OUT)/allocator_data_test: allocator_data_test.c allocator_data.c allocator_data.h $(ALLOCATOR_CORE_SOURCES) $(ROOT)/src/cce/cce_campaign_provenance.c $(ALLOCATOR_HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -lm -o $@
$(OUT)/allocator_gate_test: allocator_gate_test.c $(ALLOCATOR_CORE_SOURCES) $(ALLOCATOR_HEADERS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -lm -o $@
allocator-test: $(OUT)/allocator_test $(OUT)/allocator_data_test $(OUT)/allocator_gate_test $(OUT)/test_candidate
	$(OUT)/allocator_test
	$(OUT)/allocator_data_test
	$(OUT)/allocator_gate_test
	$(OUT)/test_candidate

$(OUT)/allocator_worker: allocator_worker.c worker.c worker.h cell_train.h worker_sandbox.c worker_sandbox.h offline.c offline.h $(ROOT)/src/serve/cnet_core_cell.c $(ROOT)/include/cnet_core_cell.h $(OUT)/cell_train.o $(OUT)/amdmath.o
	$(HIPCC) -O2 -Wall -Wextra -Werror --offload-arch=gfx1201 -I$(ROOT)/include -x c allocator_worker.c worker.c worker_sandbox.c offline.c $(ROOT)/src/serve/cnet_core_cell.c -x none $(OUT)/cell_train.o $(OUT)/amdmath.o -lm -lrt -o $@
	chmod go-w $@
$(OUT)/allocator_worker_test: allocator_worker_test.c worker.c worker.h $(ROOT)/src/serve/cnet_core_cell.c $(ROOT)/include/cnet_core_cell.h
	$(CC) $(CFLAGS) $(filter %.c,$^) -lm -o $@
allocator-worker-build: $(OUT)/allocator_worker $(OUT)/allocator_worker_test
allocator-worker-test: allocator-worker-build
	$(OUT)/allocator_worker_test $(abspath $(OUT)/allocator_worker)

# Metadata diagnostics only: no traffic generation, training or live mutation.
.PHONY: allocator-chronology-test
allocator-chronology-test:
	python3 -m unittest allocator_chronology_audit_test -v
