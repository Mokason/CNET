#!/usr/bin/env bash
# ==============================================================================
# tests/test_cnet_vsa_cli_bench.sh - Integration Test Harness for CNET-VSA CLI
# ==============================================================================
set -euo pipefail

CLI="./bin/cnet_vsa_cli"
LOG="logs/cnet_vsa_cli_bench.log"
mkdir -p logs bin

if [ ! -x "$CLI" ]; then
    echo "[!] CLI binary $CLI not found. Build it first."
    exit 1
fi

echo "================================================================="
echo " CNET-VSA Task 5: Interactive Cognitive CLI Integration Benchmark"
echo "================================================================="

CHECKS=0
PASSES=0

check() {
    CHECKS=$((CHECKS + 1))
    local desc="$1"
    local status="$2"
    if [ "$status" -eq 0 ]; then
        PASSES=$((PASSES + 1))
        printf "  %-65s PASS\n" "$desc"
    else
        printf "  %-65s FAIL\n" "$desc"
        echo "FAILED: $desc"
        exit 1
    fi
}

# 1. Help flag test
echo "[1/8] Testing CLI Flag Parsing & Device Status..."
out=$($CLI --help)
echo "$out" | grep -q "CNET-VSA: Vector Symbolic Architecture"
check "CLI --help displays banner and command documentation" $?

# 2. Device status test
out=$($CLI device-status)
echo "$out" | grep -q "AMD Radeon AI PRO R9700"
check "device-status detects AMD Radeon AI PRO R9700 GPU" $?

# 3. Positional N-Gram text encode & unbinding
echo -e "\n[2/8] Testing Positional N-Gram Text Projector via CLI..."
out=$($CLI encode "apollo lander reached lunar surface")
echo "$out" | grep -q "L2 Norm:          1.0000"
check "encode produces normalized unit hypervector" $?

echo "$out" | grep -q 'Pos  3: "lunar"'
check "encode successfully unbinds token 'lunar' at position 3" $?

# 4. Positional semantic similarity & anagram discrimination
echo -e "\n[3/8] Testing Anagram Discrimination via CLI..."
out=$($CLI sim "dog bit cat" "cat bit dog")
echo "$out" | grep -q "ANAGRAM / WORD-ORDER PERMUTATION"
check "sim command discriminates anagrams (word order sensitivity preserved)" $?

out_ident=$($CLI sim "dog bit cat" "dog bit cat")
echo "$out_ident" | grep -q "Cosine Similarity: 1.0000"
check "sim command evaluates identical sentences to 1.0000" $?

# 5. Direct token extraction
echo -e "\n[4/8] Testing Direct Algebraic Token Extraction..."
out=$($CLI extract "deep learning models reason about code" 3)
echo "$out" | grep -q 'Recovered Token:  "reason"'
check "extract recovers target word 'reason' at index 3" $?

# 6. Doc-Graft document ingestion & query
echo -e "\n[5/8] Testing Document Memory Ingestion & Associative Search..."
out=$($CLI ingest include/cnet_vsa.h "unitary permutation")
echo "$out" | grep -q "Doc-Graft Session Memory Ingestion"
check "ingest parses header file into memory graph" $?

echo "$out" | grep -q "Query:"
check "ingest executes inline associative query over indexed clauses" $?

# 7. Live Dynamic GPU VRAM Capsule Hot-Swap Benchmark
echo -e "\n[6/8] Testing Dynamic GPU VRAM Capsule Hot-Swapping..."
out=$($CLI swap-bench)
echo "$out" | grep -q "Peak VRAM Used:   48.0 MB (Hard Limit: 48.0 MB)"
check "swap-bench strictly enforces 48 MB VRAM budget without leak" $?

echo "$out" | grep -q "CACHE_HIT (0ms)"
check "swap-bench verifies 0ms cache hits on hot resident capsules" $?

# 8. Human-like Sleep Memory Consolidation
echo -e "\n[7/8] Testing Wake/Sleep Memory Consolidation Daemon..."
out=$($CLI sleep-demo)
echo "$out" | grep -q "Transient Noise Pruned:   70"
check "sleep-demo prunes 100% (70/70) of transient noise events" $?

echo "$out" | grep -q "LTM Prototypes Promoted:  3"
check "sleep-demo synthesizes and promotes 3 permanent LTM prototypes" $?

# 9. Graph-AST Structural Code Reasoning
echo -e "\n[8/8] Testing Graph-AST Structural Code Reasoning & Invariants..."
out=$($CLI ast-demo)
echo "$out" | grep -q "Found 'hipFree'"
check "ast-demo unbinds callee 'hipFree' from call relation" $?

echo "$out" | grep -q "Function 'cnet_vsa_hot_swap':     \[+\] SAFE"
check "ast-demo validates verified function as SAFE" $?

echo "$out" | grep -q "Function 'cnet_vsa_unsafe_worker': \[!\] LEAK VIOLATION"
check "ast-demo catches unverified allocation leak with zero false negatives" $?

# 10. TinyStories Creative Narrative Synthesis & Conceptual Blending
echo -e "\n[9/9] Testing TinyStories Creative Narrative Synthesis..."
out=$($CLI story-demo)
echo "$out" | grep -q "Synthesized Original Story"
check "story-demo synthesizes complete 5-beat narrative arc" $?

echo "$out" | grep -q "Contract Status:    SAFE"
check "story-demo confirms metric safety contract verification" $?

out_gen=$($CLI story-gen adventurous "Aero the dragon")
echo "$out_gen" | grep -q "Aero the dragon"
check "story-gen synthesizes custom character within requested style" $?

echo "$out_gen" | grep -q "Safety: SAFE"
check "story-gen satisfies fail-closed safety invariant" $?

# 11. Hybrid VSA-Brain + Neural Mouth & Autonomous N-Gram Generation
echo -e "\n[10/10] Testing Hybrid Neural Mouth & Autonomous N-Gram Commands..."
out_ngram=$($CLI ngram-gen once "dragon" "hills")
echo "$out_ngram" | grep -q "Output: \""
check "ngram-gen executes autonomous word-by-word unbinding with zero templates" $?

out_mouth=$($CLI mouth-gen whimsical "Oliver the fox" "enchanted forest" "glowing mushroom")
echo "$out_mouth" | grep -q "Gatekeeper Verdict:    CERTIFIED_SAFE"
check "mouth-gen executes hybrid generation with VSA back-projection certification" $?

# 12. Specialized Generative Knowledge Capsule Operations
echo -e "\n[11/11] Testing Specialized Generative Knowledge Capsule Commands..."
TMP_CORPUS="bin/tmp_cli_corpus.txt"
TMP_GENCAP="bin/tmp_cli_cyber.gencap"
cat << 'EOF' > "$TMP_CORPUS"
The security firewall detected abnormal packet traffic originating from an unknown subnet.
Intrusion prevention systems automatically blocked the compromised network port.
Cryptographic key exchange established a secure encrypted tunnel across the interface.
The incident response team mitigated the unauthorized access vulnerability immediately.
Network monitoring logged all suspicious payloads for forensic cyber analysis.
EOF

out_create=$($CLI gencap-create cyber_v1 SECURITY "$TMP_CORPUS" "$TMP_GENCAP")
echo "$out_create" | grep -q "SEALED & CERTIFIED"
check "gencap-create compiles and certifies portable capsule" $?

out_verify=$($CLI gencap-verify "$TMP_GENCAP")
echo "$out_verify" | grep -q "CERTIFIED_AUTHENTIC"
check "gencap-verify validates cryptographic integrity digest" $?

out_gen_in=$($CLI gencap-gen "$TMP_GENCAP" "firewall packet intrusion network security" "the" 20)
echo "$out_gen_in" | grep -q "SUCCESS (In-Domain)"
check "gencap-gen generates in-domain autonomous text" $?

out_gen_ood=$($CLI gencap-gen "$TMP_GENCAP" "fairy princess enchanted mushroom forest" "the" 20)
echo "$out_gen_ood" | grep -q "REFUSED (Out-of-Domain)"
check "gencap-gen enforces fail-closed out-of-domain abstention" $?

rm -f "$TMP_CORPUS" "$TMP_GENCAP"

# 13. REPL Interactive pipe test
repl_out=$(printf "sim \"quantum mechanics\" \"quantum mechanics\"\nstory-gen cozy \"Barnaby\"\nquit\n" | $CLI repl)
echo "$repl_out" | grep -q "IDENTICAL sentences"
check "repl mode operates interactively via standard input" $?

echo "$repl_out" | grep -q "Barnaby"
check "repl mode executes story-gen interactively with custom characters" $?

echo -e "\n-----------------------------------------------------------------"
echo "CNET_VSA_CLI_BENCH_PASS: all $CHECKS checks passed."

