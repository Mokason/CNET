// Spike: the MoE prefill expert CONVEYOR — kill the copy-then-compute stall.
//
// The problem (measured on real MoE-offload prefill): past the batch
// threshold the GPU runs attention + router, then SITS IDLE while the
// selected experts stream RAM -> VRAM over PCIe, and only then computes
// them. Per layer: t = t_router + t_copy + t_compute, serialized. The bus
// is the villain: at prefill the copy dwarfs the compute.
//
// The fix this spike proves, on this box's real links (2x R9700, PCIe5 x8
// each, measured 28.5 GB/s solo / 46 GB/s aggregate, SDMA fully
// independent of the CUs):
//
//   1. CONVEYOR: a dedicated copy stream per GPU streams experts in a
//      fixed order through a ring of VRAM slots; the compute stream
//      chases per-expert readiness EVENTS — expert e computes while
//      expert e+1..e+S stream. Steady state: max(copy, compute), not
//      copy + compute. The router never gates the stream at prefill:
//      with hundreds of tokens x top-k, essentially every expert is hit,
//      so the conveyor runs router-blind in file order and never stops
//      at layer boundaries (layer L+1 streams while L computes).
//   2. TWO BUSES: experts split across both GPUs -> each PCIe link
//      carries half the bytes; partial results would merge over P2P
//      (measured 28 GB/s; activations are ~1000x smaller than weights).
//   3. Source is the hipHostRegister'd whole-file mmap (single-hop DMA,
//      proven by spike/dma_loader_spike.cpp; here at whole-model scale).
//
// "Experts" are equal-sized spans of the real GGUF's data region —
// byte-identical DMA behavior to real expert tensors; the consuming
// kernel's arithmetic intensity R emulates the prefill batch size.
//
// Build: hipcc -O2 --offload-arch=gfx1201 -o bin/expert_conveyor_spike spike/expert_conveyor_spike.cpp -lpthread
// Run:   ./bin/expert_conveyor_spike [gguf_path] [R]
//        R = FMA passes per element (arithmetic intensity knob; default 6)

#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <vector>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define CK(x) do{ hipError_t e_=(x); if(e_!=hipSuccess){ \
  fprintf(stderr,"HIP error %s at %s:%d\n",hipGetErrorString(e_),__FILE__,__LINE__); exit(1);} }while(0)

static double now_s(){
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Consume one streamed expert: grid-stride read of the whole span, R FMA
// passes per element. R small = bus-bound (small prefill chunk), R large =
// compute-bound (big chunk). One float out per block defeats DCE.
__global__ void k_consume(const float4* __restrict__ w, size_t n4, int R, float* out){
    float acc = 0.f;
    for(size_t i = blockIdx.x*blockDim.x + threadIdx.x; i < n4; i += (size_t)gridDim.x*blockDim.x){
        float4 v = w[i];
        for(int r=0;r<R;r++){
            acc = fmaf(v.x, 1.0001f, acc); acc = fmaf(v.y, 0.9999f, acc);
            acc = fmaf(v.z, 1.0001f, acc); acc = fmaf(v.w, 0.9999f, acc);
        }
    }
    if(threadIdx.x==0) out[blockIdx.x] = acc;
}

static const size_t EXPERT_MB   = 16;                       // bytes per fake expert
static const size_t EXPERT_SZ   = EXPERT_MB<<20;
static const int    N_LAYER_GRP = 8;                        // "layers" for the serialized baseline
static const int    N_SLOTS     = 8;                        // conveyor ring depth per GPU

struct Plan {                       // one GPU's share of the stream
    int dev;
    std::vector<const uint8_t*> src;                        // host span per expert
};

// --- serialized baseline: per layer, copy ALL its experts, THEN compute ---
static double run_serialized(const Plan& p){
    CK(hipSetDevice(p.dev));
    hipStream_t s; CK(hipStreamCreate(&s));
    float* dout; CK(hipMalloc(&dout, 4096*sizeof(float)));
    std::vector<void*> slots(N_SLOTS);
    for(auto& b : slots) CK(hipMalloc(&b, EXPERT_SZ));
    int R = atoi(getenv("SPIKE_R") ? getenv("SPIKE_R") : "6");

    size_t n = p.src.size(), per = (n + N_LAYER_GRP - 1)/N_LAYER_GRP;
    double t0 = now_s();
    for(size_t l0=0; l0<n; l0+=per){
        size_t l1 = l0+per<n ? l0+per : n;
        // copy phase: the GPU receives, computes nothing (the villain)
        for(size_t i=l0;i<l1;i++){
            // reuse slots round-robin inside the layer; sync before overwrite
            if(i-l0 >= N_SLOTS) CK(hipStreamSynchronize(s));
            CK(hipMemcpyAsync(slots[(i-l0)%N_SLOTS], p.src[i], EXPERT_SZ, hipMemcpyHostToDevice, s));
            if((i-l0)%N_SLOTS == N_SLOTS-1 || i==l1-1){
                CK(hipStreamSynchronize(s));                 // copy-all barrier
                for(size_t j = i-(i-l0)%N_SLOTS; j<=i; j++)  // compute phase
                    hipLaunchKernelGGL(k_consume, dim3(256), dim3(256), 0, s,
                        (const float4*)slots[(j-l0)%N_SLOTS], EXPERT_SZ/16, R, dout);
                CK(hipStreamSynchronize(s));                 // compute barrier
            }
        }
    }
    double dt = now_s()-t0;
    for(auto b : slots) CK(hipFree(b));
    CK(hipFree(dout)); CK(hipStreamDestroy(s));
    return dt;
}

// --- conveyor: copy stream streams continuously; compute chases events ---
static double run_conveyor(const Plan& p){
    CK(hipSetDevice(p.dev));
    hipStream_t cs, ks;                                     // copy + compute
    CK(hipStreamCreate(&cs)); CK(hipStreamCreate(&ks));
    float* dout; CK(hipMalloc(&dout, 4096*sizeof(float)));
    std::vector<void*> slots(N_SLOTS);
    for(auto& b : slots) CK(hipMalloc(&b, EXPERT_SZ));
    std::vector<hipEvent_t> ev_ready(N_SLOTS), ev_done(N_SLOTS);
    for(int i=0;i<N_SLOTS;i++){
        CK(hipEventCreateWithFlags(&ev_ready[i], hipEventDisableTiming));
        CK(hipEventCreateWithFlags(&ev_done[i],  hipEventDisableTiming));
    }
    int R = atoi(getenv("SPIKE_R") ? getenv("SPIKE_R") : "6");

    double t0 = now_s();
    for(size_t i=0;i<p.src.size();i++){
        int sl = (int)(i % N_SLOTS);
        if(i >= (size_t)N_SLOTS)
            CK(hipStreamWaitEvent(cs, ev_done[sl], 0));      // slot free?
        CK(hipMemcpyAsync(slots[sl], p.src[i], EXPERT_SZ, hipMemcpyHostToDevice, cs));
        CK(hipEventRecord(ev_ready[sl], cs));                // expert i landed
        CK(hipStreamWaitEvent(ks, ev_ready[sl], 0));         // compute chases
        hipLaunchKernelGGL(k_consume, dim3(256), dim3(256), 0, ks,
                           (const float4*)slots[sl], EXPERT_SZ/16, R, dout);
        CK(hipEventRecord(ev_done[sl], ks));                 // slot reusable
    }
    CK(hipStreamSynchronize(cs)); CK(hipStreamSynchronize(ks));
    double dt = now_s()-t0;
    for(int i=0;i<N_SLOTS;i++){ CK(hipEventDestroy(ev_ready[i])); CK(hipEventDestroy(ev_done[i])); }
    for(auto b : slots) CK(hipFree(b));
    CK(hipFree(dout)); CK(hipStreamDestroy(cs)); CK(hipStreamDestroy(ks));
    return dt;
}

int main(int argc, char** argv){
    const char* path = argc>1 ? argv[1] : "/home/marble/AI/Models/gemma4-v2-Q4_K_M.gguf";
    if(argc>2) setenv("SPIKE_R", argv[2], 1);
    int R = atoi(getenv("SPIKE_R") ? getenv("SPIKE_R") : "6");

    // devices: discrete gfx1201 only — never the iGPU
    int ndev=0; CK(hipGetDeviceCount(&ndev));
    std::vector<int> gpus;
    for(int i=0;i<ndev;i++){
        hipDeviceProp_t pr; CK(hipGetDeviceProperties(&pr,i));
        if(!pr.integrated && strncmp(pr.gcnArchName,"gfx1201",7)==0) gpus.push_back(i);
    }
    if(gpus.empty()){ fprintf(stderr,"no discrete gfx1201 GPU\n"); return 1; }

    // mmap the whole model file, register the WHOLE mapping (production scale)
    int fd = open(path, O_RDONLY);
    if(fd<0){ perror("open"); return 1; }
    struct stat st; fstat(fd,&st);
    uint8_t* base = (uint8_t*)mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if(base==MAP_FAILED){ perror("mmap"); return 1; }
    size_t usable = ((size_t)st.st_size) & ~(EXPERT_SZ-1);
    madvise(base, st.st_size, MADV_WILLNEED);
    volatile uint64_t sink=0;                                // prefault: charge disk once
    for(size_t o=0;o<(size_t)st.st_size;o+=4096) sink+=base[o];
    (void)sink;
    double treg0 = now_s();
    hipError_t rc = hipHostRegister(base, st.st_size, hipHostRegisterDefault);
    bool reg = (rc==hipSuccess);
    printf("mmap %s: %.2f GB, whole-file hipHostRegister: %s (%.2f s)\n",
           path, st.st_size/1e9, reg?"OK":hipGetErrorString(rc), now_s()-treg0);

    size_t n_expert = usable / EXPERT_SZ;
    printf("experts: %zu x %zu MB = %.2f GB streamed per pass, R=%d, ring=%d slots\n\n",
           n_expert, EXPERT_MB, n_expert*(double)EXPERT_SZ/1e9, R, N_SLOTS);

    auto make_plan=[&](int dev, size_t begin, size_t step)->Plan{
        Plan p; p.dev=dev;
        for(size_t i=begin;i<n_expert;i+=step) p.src.push_back(base + i*EXPERT_SZ);
        return p;
    };
    double bytes_total = (double)n_expert*EXPERT_SZ;

    // ---- 1 GPU, serialized (the status quo) ----
    Plan all0 = make_plan(gpus[0], 0, 1);
    double t_ser1 = run_serialized(all0);
    printf("1 GPU  serialized      : %6.2f s  (%5.1f GB/s effective)\n", t_ser1, bytes_total/t_ser1/1e9);

    // ---- 1 GPU, conveyor ----
    double t_con1 = run_conveyor(all0);
    printf("1 GPU  conveyor        : %6.2f s  (%5.1f GB/s effective)  %.2fx\n",
           t_con1, bytes_total/t_con1/1e9, t_ser1/t_con1);

    if(gpus.size()>=2){
        // ---- 2 GPUs, serialized halves (parallel but each still stalls) ----
        Plan h0 = make_plan(gpus[0], 0, 2), h1 = make_plan(gpus[1], 1, 2);
        double t0=now_s(), ta=0, tb=0;
        { std::thread A([&]{ ta=run_serialized(h0); }), B([&]{ tb=run_serialized(h1); }); A.join(); B.join(); }
        double t_ser2 = now_s()-t0;
        printf("2 GPUs serialized      : %6.2f s  (%5.1f GB/s effective)  %.2fx\n",
               t_ser2, bytes_total/t_ser2/1e9, t_ser1/t_ser2);

        // ---- 2 GPUs, conveyor (the design) ----
        t0=now_s();
        { std::thread A([&]{ ta=run_conveyor(h0); }), B([&]{ tb=run_conveyor(h1); }); A.join(); B.join(); }
        double t_con2 = now_s()-t0;
        printf("2 GPUs conveyor        : %6.2f s  (%5.1f GB/s effective)  %.2fx  <- the fix\n",
               t_con2, bytes_total/t_con2/1e9, t_ser1/t_con2);
    }

    if(reg) (void)hipHostUnregister(base);
    munmap(base, st.st_size); close(fd);
    printf("\nSPIKE OK: conveyor = continuous DMA + compute chasing events; serialized = the status-quo stall\n");
    return 0;
}
