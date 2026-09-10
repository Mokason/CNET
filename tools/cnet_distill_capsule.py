#!/usr/bin/env python3
"""
tools/cnet_distill_capsule.py - Distill Knowledge from 27B Teacher into CNET .gencap Capsules
Parallel 4-way GPU Batched Extraction Pipeline.
"""

import sys
import os
import re
import json
import urllib.request
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

TEACHER_URL = "http://127.0.0.1:8081/v1/chat/completions"
CNET_CLI = "./bin/cnet_vsa_cli"

CURRICULA = {
    "cnet_vsa_core": {
        "domain_tag": "COGNITIVE_VSA",
        "system_prompt": "You are a specialized researcher in Vector Symbolic Architectures (VSA) and Hyperdimensional Computing (HDC). Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain hyperdimensional representation and how information is encoded in 10000-dimensional vectors.",
            "Explain vector bundling or superposition using vector addition and normalization.",
            "Explain vector binding using element-wise multiplication or Hadamard product.",
            "Explain positional sequence encoding using circular permutation and circular shift rolls.",
            "Explain algebraic unbinding to extract an item or token from a composite hypervector.",
            "Explain cosine similarity, Hamming distance, and how pseudo-orthogonality prevents cross-talk.",
            "Explain out-of-domain abstention and why metric distance to the centroid fail-closes unsafe queries.",
            "Explain why pure VSA text generation executes with zero neural transformer parameters."
        ],
        "test_in_domain": "hyperdimensional binding unbinding vector superposition",
        "test_out_domain": "chocolate cake vanilla frosting baking recipe ingredients"
    },
    "cyber_defense": {
        "domain_tag": "SECURITY_OPS",
        "system_prompt": "You are an elite cyber defense and network security architect. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain network firewall rules, stateful packet filtering, and anomaly detection.",
            "Explain intrusion prevention systems and how malicious traffic signatures are mitigated.",
            "Explain cryptographic key exchange, TLS handshakes, and encrypted tunneling protocols.",
            "Explain zero trust architecture, least privilege access control, and identity verification.",
            "Explain incident response workflows, digital forensics, and payload quarantine procedures.",
            "Explain memory safety vulnerabilities, buffer overflow mitigation, and ASLR protection."
        ],
        "test_in_domain": "firewall packet intrusion network security cryptographic handshake",
        "test_out_domain": "fairy castle glowing enchanted mushroom butterfly wings"
    },
    "hardware_embedded": {
        "domain_tag": "EMBEDDED_SYS",
        "system_prompt": "You are an expert embedded systems and computer architecture engineer. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain the difference between a microcontroller and a microprocessor architecture.",
            "Explain interrupt service routines, interrupt latency, and hardware priority nesting.",
            "Explain direct memory access controllers and high-speed peripheral data transfers without CPU intervention.",
            "Explain hardware communication buses including SPI, I2C, UART, and CAN bus protocols.",
            "Explain real-time operating system task scheduling, preemptive priorities, and mutex deadlocks.",
            "Explain firmware memory layouts, flash bootloaders, SRAM stack limits, and watchdog timers."
        ],
        "test_in_domain": "microcontroller interrupt service routine dma memory bus protocol",
        "test_out_domain": "romantic poetry sunset beach flowers ocean breeze"
    },
    "rocm_gpu_compute": {
        "domain_tag": "GPU_ROCM",
        "system_prompt": "You are a senior AMD GPU kernel optimization engineer. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain AMD ROCm HIP kernel execution model, grids, thread blocks, and wavefront scheduling.",
            "Explain GPU Local Data Share (LDS) shared memory and bank conflict avoidance.",
            "Explain global memory transaction coalescing and cache line alignment on RDNA and CDNA architectures.",
            "Explain asynchronous memory copies using hipMemcpyAsync and non-blocking compute streams.",
            "Explain atomic operations in GPU VRAM and warp-level reduction primitives.",
            "Explain register pressure, occupancy limits, and compute unit resource allocation in HIP kernels."
        ],
        "test_in_domain": "rocm hip kernel wavefront lds shared memory coalescing stream",
        "test_out_domain": "vintage wine tasting fermentation oak barrel vineyard"
    },
    "linux_kernel_internals": {
        "domain_tag": "LINUX_KERNEL",
        "system_prompt": "You are a Linux kernel core systems engineer. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain the Virtual File System (VFS) layer, inodes, dentries, and file descriptors.",
            "Explain page cache architecture, dirty page writeback, and memory-mapped files via mmap.",
            "Explain high-performance asynchronous I/O with epoll and io_uring event queues.",
            "Explain Linux kernel namespaces, cgroups resource controllers, and process isolation.",
            "Explain extended Berkeley Packet Filters (eBPF) in-kernel tracing and packet filtering.",
            "Explain kernel spinlocks, read-copy-update (RCU) synchronization, and mutex lock contention."
        ],
        "test_in_domain": "linux vfs inode page cache epoll cgroups ebpf rcu",
        "test_out_domain": "watercolor landscape painting brush techniques oil canvas"
    },
    "modern_c11_standards": {
        "domain_tag": "LANG_C11",
        "system_prompt": "You are a modern C systems standards expert. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain C11 atomic operations, atomic flags, and memory consistency orderings.",
            "Explain memory barrier fences and sequential consistency versus relaxed ordering.",
            "Explain static assertions with static_assert and compile-time invariant enforcement.",
            "Explain type-generic expressions using the _Generic keyword in C11.",
            "Explain alignment specifiers with alignas and cache-line aligned memory allocation.",
            "Explain bounded string handling, bounds-checking interfaces, and buffer overflow prevention."
        ],
        "test_in_domain": "c11 atomic memory order fence static assert generic alignas",
        "test_out_domain": "medieval castle architecture moat drawbridge stone siege"
    },
    "network_protocols": {
        "domain_tag": "NET_PROTOCOLS",
        "system_prompt": "You are a principal network systems architect. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain TCP congestion control algorithms including CUBIC, BBR, and window scaling.",
            "Explain QUIC protocol architecture, UDP multiplexing, and zero round-trip connection setup.",
            "Explain DNS hierarchical resolution, authoritative name servers, and recursive caching.",
            "Explain BGP routing, autonomous system path selection, and internet peering agreements.",
            "Explain Maximum Transmission Unit (MTU), IP packet fragmentation, and Path MTU Discovery.",
            "Explain transport layer security handshake latency, session tickets, and ALPN negotiation."
        ],
        "test_in_domain": "tcp congestion control quic udp dns bgp mtu routing",
        "test_out_domain": "haute cuisine gourmet pastry dessert chocolate soufflé"
    },
    "distributed_systems": {
        "domain_tag": "DISTRIBUTED_SYS",
        "system_prompt": "You are a distributed systems and consensus algorithm researcher. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain the Raft consensus protocol, leader election terms, and append-entries log replication.",
            "Explain Paxos consensus, quorum intersection, and safety guarantees under asynchronous networks.",
            "Explain vector clocks, Lamport timestamps, and partial event ordering in distributed networks.",
            "Explain the CAP theorem and the trade-offs between consistency, availability, and partition tolerance.",
            "Explain distributed transactions, two-phase commit protocol, and split-brain resolution.",
            "Explain gossip protocols, failure detectors, and eventual consistency in peer-to-peer clusters."
        ],
        "test_in_domain": "raft consensus leader election log replication paxos quorum cap theorem",
        "test_out_domain": "scuba diving coral reef tropical fish submarine depth"
    },
    "compiler_optimization": {
        "domain_tag": "COMPILER_TECH",
        "system_prompt": "You are an LLVM compiler backend engineer. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain Static Single Assignment (SSA) form and phi nodes in intermediate representations.",
            "Explain compiler optimization passes including dead code elimination and common subexpression elimination.",
            "Explain loop transformations such as loop unrolling, loop vectorization, and polyhedral optimization.",
            "Explain interprocedural optimization (IPO) and Link-Time Optimization (LTO) across translation units.",
            "Explain register allocation via graph coloring and register spilling algorithms.",
            "Explain instruction pipelining, superscalar execution scheduling, and branch prediction hints."
        ],
        "test_in_domain": "llvm compiler ssa dead code elimination loop vectorization lto register allocation",
        "test_out_domain": "astrology horoscope zodiac signs crystal healing energy"
    },
    "cryptography_foundations": {
        "domain_tag": "CRYPTO_MATH",
        "system_prompt": "You are a mathematical cryptographer and security researcher. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain Elliptic Curve Cryptography (ECC) and discrete logarithm hardness over finite fields.",
            "Explain symmetric cipher modes focusing on AES-GCM authenticated encryption with associated data.",
            "Explain cryptographic hash functions, the Merkle-Damgard construction, and SHA-3 sponge functions.",
            "Explain Diffie-Hellman ephemeral key exchange and forward secrecy guarantees.",
            "Explain constant-time cryptographic programming to mitigate timing side-channel attacks.",
            "Explain post-quantum lattice-based cryptography and the Learning With Errors (LWE) problem."
        ],
        "test_in_domain": "elliptic curve cryptography aes gcm sha3 diffie hellman forward secrecy",
        "test_out_domain": "fashion runway haute couture designer silk dress models"
    },
    "database_internals": {
        "domain_tag": "DB_INTERNALS",
        "system_prompt": "You are a database storage engine and query optimizer researcher. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain B-tree and B+ tree index structures, leaf node pointers, and page splitting.",
            "Explain Write-Ahead Logging (WAL), write serialization, and ARIES crash recovery protocols.",
            "Explain Multi-Version Concurrency Control (MVCC), snapshot isolation, and row visibility tuples.",
            "Explain database query optimization, cost-based planning, hash joins, and index scans.",
            "Explain Log-Structured Merge (LSM) trees, memtables, write buffers, and SSTable compaction.",
            "Explain ACID transaction isolation levels including read committed, repeatable read, and serializable."
        ],
        "test_in_domain": "database btree wal transaction isolation mvcc lsm query planner",
        "test_out_domain": "tropical coral reef scuba diving clownfish ocean depths"
    },
    "python_runtime": {
        "domain_tag": "PYTHON_VM",
        "system_prompt": "You are a CPython core developer and virtual machine internals researcher. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain CPython bytecode evaluation loop, opcode dispatch, and evaluation frame stacks.",
            "Explain the Global Interpreter Lock (GIL), thread scheduling, and GIL contention in multithreading.",
            "Explain CPython memory management, small object allocator obmalloc, arenas, pools, and blocks.",
            "Explain reference counting, cyclic garbage collector, generations, and tracked object lists.",
            "Explain Python descriptor protocol, __get__, __set__, and attribute lookup resolution order MRO.",
            "Explain Python C extension API, PyObject pointers, refcount macros, and PyTypeObject dispatch."
        ],
        "test_in_domain": "cpython bytecode gil garbage collector obmalloc pyobject mro",
        "test_out_domain": "baking sourdough artisan bread yeast flour hydration proofing oven"
    },
    "operating_systems_sched": {
        "domain_tag": "OS_SCHED",
        "system_prompt": "You are an operating system kernel scheduler and real-time systems researcher. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain the Completely Fair Scheduler (CFS), virtual runtime vruntime, and red-black tree task queue.",
            "Explain real-time scheduling policies including SCHED_FIFO, SCHED_RR, and deadline scheduling.",
            "Explain thread context switching, saving CPU registers, and restoring program counter and stack.",
            "Explain CPU affinity, NUMA memory node balancing, and cross-core thread migration overhead.",
            "Explain priority inversion, bounded priority inheritance, and priority ceiling protocols.",
            "Explain load balancing domains, scheduler tick interrupts, and CPU work stealing."
        ],
        "test_in_domain": "cfs vruntime context switch sched fifo numa priority inversion",
        "test_out_domain": "gardening organic compost tomato plants fertilizer soil pruning"
    },
    "quantum_computing": {
        "domain_tag": "QUANTUM_INFO",
        "system_prompt": "You are a quantum information theorist and quantum computing researcher. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain quantum bits (qubits), state vectors, superposition, and Bloch sphere representation.",
            "Explain quantum entanglement, Bell states, and non-local measurement correlations.",
            "Explain unitary quantum gates including Hadamard, Pauli-X, CNOT, and phase shift gates.",
            "Explain Shor's algorithm for polynomial-time integer factorization using quantum Fourier transform.",
            "Explain Grover's search algorithm and quadratic speedup via amplitude amplification.",
            "Explain quantum decoherence, T1 relaxation time, T2 dephasing, and quantum error correction codes."
        ],
        "test_in_domain": "qubit superposition entanglement unitary gate bloch sphere shor grover",
        "test_out_domain": "knitting wool sweater patterns yarn needles stitches crochet"
    },
    "digital_signal_proc": {
        "domain_tag": "DSP_MATH",
        "system_prompt": "You are a digital signal processing engineer and applied mathematician. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain the Discrete Fourier Transform (DFT) and Cooley-Tukey Fast Fourier Transform (FFT) algorithm.",
            "Explain the Nyquist-Shannon sampling theorem, aliasing, and anti-aliasing low-pass filtering.",
            "Explain Finite Impulse Response (FIR) filters, linear phase response, and convolution sums.",
            "Explain Infinite Impulse Response (IIR) filters, recursive feedback, and pole-zero stability in the Z-plane.",
            "Explain digital filtering windowing functions including Hamming, Hanning, and Blackman windows.",
            "Explain discrete cosine transform (DCT) and spectral compression in audio and image processing."
        ],
        "test_in_domain": "fourier transform fft nyquist sampling fir iir filter convolution",
        "test_out_domain": "astrology horoscope zodiac signs tarot cards crystal reading"
    },
    "cnet_kernel_arch": {
        "domain_tag": "CNET_ASI",
        "system_prompt": "You are the chief architect of CNET Specialized Intelligence and Vector Symbolic Architectures. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain CNET Specialized Intelligence architecture, isolated knowledge registries, and zero neural runtime.",
            "Explain CNET .gencap capsule structure, 512-dimensional domain centroid, and FNV-1a digest sealing.",
            "Explain CNET Vector Symbolic Architecture hypervector binding, unbinding, and associative permutation.",
            "Explain CNET fail-closed out-of-domain abstention when cosine distance exceeds safe radius limit.",
            "Explain CNET zero-LLM text synthesis via high-dimensional algebraic unbinding across word vocabularies.",
            "Explain CNET multi-capsule sub-microsecond intent routing across specialized certified technical domains."
        ],
        "test_in_domain": "cnet capsule gencap vsa hypervector unbinding abstention intent routing",
        "test_out_domain": "haute couture cocktail party luxury dresses red carpet celebrity"
    }
}

def query_teacher(prompt, system_prompt):
    payload = {
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": prompt}
        ],
        "max_tokens": 256,
        "temperature": 0.2
    }
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(TEACHER_URL, data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            res = json.loads(resp.read().decode("utf-8"))
            content = res["choices"][0]["message"]["content"]
            # Clean up thinking tokens if present
            if "[Start thinking]" in content:
                content = content.split(">", 1)[-1]
            return content
    except Exception as e:
        print(f"[-] Teacher query error: {e}", file=sys.stderr)
        return ""

def clean_sentences(raw_text):
    sentences = []
    lines = raw_text.split("\n")
    for line in lines:
        line = line.strip()
        line = re.sub(r"^[\*\-\•\d+\.\s]+", "", line).strip()
        line = line.replace("**", "").replace("`", "")
        if not line:
            continue
        parts = re.split(r"(?<=[.!?])\s+", line)
        for part in parts:
            part = part.strip()
            if len(part) >= 20 and not part.startswith("Here") and not part.startswith("Note:"):
                if part[-1] not in ".!?":
                    part += "."
                sentences.append(part)
    return sentences

def distill_domain(domain_key, workers=4):
    if domain_key not in CURRICULA:
        print(f"Error: unknown domain '{domain_key}'. Available: {list(CURRICULA.keys())}")
        return False

    spec = CURRICULA[domain_key]
    domain_tag = spec["domain_tag"]
    print(f"\n=================================================================")
    print(f" Distilling Domain: {domain_key} (Tag: {domain_tag})")
    print(f" Teacher: 27B Ternary Bonsai (:8081) [Parallel Slots: {workers}]")
    print(f"=================================================================\n")

    corpus_dir = "var/distill"
    os.makedirs(corpus_dir, exist_ok=True)
    corpus_file = os.path.join(corpus_dir, f"{domain_key}_corpus.txt")
    capsule_file = f"bin/{domain_key}.gencap"

    t_start = time.time()
    all_sentences = []

    # Parallel query execution over the 4 teacher slots
    with ThreadPoolExecutor(max_workers=workers) as executor:
        future_to_probe = {
            executor.submit(query_teacher, probe, spec["system_prompt"]): (idx, probe)
            for idx, probe in enumerate(spec["probes"], 1)
        }
        for future in as_completed(future_to_probe):
            idx, probe = future_to_probe[future]
            raw = future.result()
            sents = clean_sentences(raw)
            print(f"  [Slot Done] Probe {idx}/{len(spec['probes'])}: \"{probe[:45]}...\" -> {len(sents)} sents")
            all_sentences.extend(sents)

    # Deduplicate while preserving order
    seen = set()
    unique_sentences = []
    for s in all_sentences:
        s_lower = s.lower()
        if s_lower not in seen:
            seen.add(s_lower)
            unique_sentences.append(s)

    print(f"\nTotal curated domain exemplars: {len(unique_sentences)}")
    with open(corpus_file, "w") as f:
        for s in unique_sentences:
            f.write(s + "\n")
    print(f"Saved domain corpus to '{corpus_file}'.")

    # Step 1: Create and Seal Capsule via native CNET CLI
    print(f"\nCompiling & sealing '{capsule_file}'...")
    cmd_create = [CNET_CLI, "gencap-create", domain_key, domain_tag, corpus_file, capsule_file]
    res_create = subprocess.run(cmd_create, capture_output=True, text=True)
    print(res_create.stdout)
    if res_create.returncode != 0:
        print(f"[-] Failed to create capsule: {res_create.stderr}")
        return False

    # Step 2: Verify Cryptographic Digest
    cmd_verify = [CNET_CLI, "gencap-verify", capsule_file]
    res_verify = subprocess.run(cmd_verify, capture_output=True, text=True)
    print(res_verify.stdout)
    if "CERTIFIED_AUTHENTIC" not in res_verify.stdout:
        print("[-] Capsule verification failed!")
        return False

    # Step 3: Test In-Domain Autonomous Generation
    cmd_gen_in = [CNET_CLI, "gencap-gen", capsule_file, spec["test_in_domain"], "the", "25"]
    res_gen_in = subprocess.run(cmd_gen_in, capture_output=True, text=True)
    print(res_gen_in.stdout)

    # Step 4: Test Out-of-Domain Abstention
    cmd_gen_ood = [CNET_CLI, "gencap-gen", capsule_file, spec["test_out_domain"], "the", "25"]
    res_gen_ood = subprocess.run(cmd_gen_ood, capture_output=True, text=True)
    print(res_gen_ood.stdout)

    elapsed = time.time() - t_start
    print(f"[✓] Successfully distilled '{domain_key}.gencap' in {elapsed:.1f}s.\n")
    return True

if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else "all"
    if target == "all":
        t0 = time.time()
        print(f"Starting parallel distillation across {len(CURRICULA)} domains...")
        for k in CURRICULA.keys():
            distill_domain(k, workers=4)
        total_time = time.time() - t0
        print(f"=================================================================")
        print(f" ALL {len(CURRICULA)} DOMAINS DISTILLED IN {total_time:.1f}s")
        print(f"=================================================================")
    else:
        distill_domain(target, workers=4)
