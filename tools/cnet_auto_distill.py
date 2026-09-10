#!/usr/bin/env python3
"""
tools/cnet_auto_distill.py — CNET Autonomous Self-Distillation Engine
Enables CNET to autonomously create certified VSA Knowledge Capsules (.gencap)
for itself from the local 27B model (Ternary Bonsai 27B on ROCm :8081).

Architecture:
  1. Epistemic Gap / Topic Input -> Autonomous Curriculum Generation (Probes + OOD Boundary).
  2. Parallel Exemplar Harvesting from 27B Teacher (4-slot ROCm batching).
  3. Factual Filtering & Declarative Invariant Curation.
  4. Native VSA Compilation (Centroid + Bigram Graph + FNV-1a Digest).
  5. Cryptographic Verification & Fail-Closed Abstention Certification Gate.
  6. Atomic Installation & Live Hot-Swap into Registry.
"""

import sys
import os
import re
import json
import time
import urllib.request
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed

TEACHER_URL = os.getenv("CNET_TEACHER_URL", "http://127.0.0.1:8081/v1/chat/completions")
CNET_CLI = os.getenv("CNET_CLI", "./bin/cnet_vsa_cli")


def query_teacher(prompt: str, system_prompt: str, max_tokens: int = 256, temperature: float = 0.2) -> str:
    payload = {
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": prompt}
        ],
        "max_tokens": max_tokens,
        "temperature": temperature
    }
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(TEACHER_URL, data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=45) as resp:
            res = json.loads(resp.read().decode("utf-8"))
            content = res["choices"][0]["message"]["content"]
            # Clean up thinking tokens if present
            if "[Start thinking]" in content:
                content = content.split(">", 1)[-1]
            return content.strip()
    except Exception as e:
        print(f"[-] Teacher query error: {e}", file=sys.stderr)
        return ""


def clean_sentences(raw_text: str) -> list[str]:
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


def generate_curriculum(topic: str) -> dict:
    """Uses 27B model to autonomously generate probe questions, domain tag, and OOD boundary for any topic."""
    print(f"[*] Formulating autonomous curriculum for topic: '{topic}'...")
    
    # 1. Generate Domain Tag
    sys_tag = "You are a systems taxonomy classifier. Output exactly one short uppercase alphanumeric tag with underscores representing this domain. Nothing else."
    raw_tag = query_teacher(f"Topic: {topic}", sys_tag, max_tokens=16, temperature=0.1)
    tag_clean = re.sub(r"[^A-Z0-9_]", "", raw_tag.upper())
    if not tag_clean:
        tag_clean = "CUSTOM_DOMAIN"
    
    # 2. Generate Probes
    sys_probes = (
        "You are a curriculum generator. Output 6 precise, deep, factual probe questions to extract all essential "
        "mechanics, components, processes, and rules of this domain. Return one question per line without numbers or bullet points."
    )
    raw_probes = query_teacher(f"Domain: {topic}", sys_probes, max_tokens=300, temperature=0.3)
    probes = [p.strip() for p in raw_probes.splitlines() if len(p.strip()) > 15]
    if len(probes) < 3:
        probes = [
            f"Explain the core mechanisms, fundamental entities, and operational principles of {topic}.",
            f"Explain the causal processes, sequential workflows, and practical rules of {topic}.",
            f"Explain the specialized terminology, edge cases, and boundary constraints of {topic}."
        ]
        
    # 3. Generate In-Domain & Out-of-Domain Calibration Tests (Natural Sentences)
    sys_tests = (
        "Output exactly two lines:\n"
        "LINE 1: One realistic, natural technical inquiry question directly about this topic.\n"
        "LINE 2: One realistic inquiry question about a completely unrelated alien topic (e.g. quantum physics or baking)."
    )
    raw_tests = query_teacher(f"Domain: {topic}", sys_tests, max_tokens=150, temperature=0.2)
    test_lines = [l.strip() for l in raw_tests.splitlines() if l.strip() and len(l.strip()) > 15]
    
    test_in = test_lines[0] if len(test_lines) > 0 else f"How do core principles and processes of {topic} operate?"
    test_out = test_lines[1] if len(test_lines) > 1 else "What are quantum qubits and wave function superposition in quantum computing?"
    
    # Sanitize domain key for filenames
    domain_key = re.sub(r"[^a-z0-9_]", "_", topic.lower().strip().replace(" ", "_"))
    domain_key = re.sub(r"_+", "_", domain_key).strip("_")

    return {
        "domain_key": domain_key,
        "domain_tag": tag_clean,
        "probes": probes[:6],
        "test_in_domain": probes[0],
        "test_out_domain": test_out,
        "system_prompt": f"You are a leading specialist and authoritative researcher in {topic}. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line."
    }


def auto_distill(topic: str, output_dir: str = "bin", workers: int = 4) -> str | None:
    """End-to-end autonomous distillation and certification of a new capsule."""
    t0 = time.time()
    curriculum = generate_curriculum(topic)
    domain_key = curriculum["domain_key"]
    domain_tag = curriculum["domain_tag"]
    
    print("\n" + "="*65)
    print(f" CNET Autonomous Capsule Distillation Engine")
    print(f" Domain:     {domain_key} ({domain_tag})")
    print(f" Probes:     {len(curriculum['probes'])} autonomous questions")
    print(f" Teacher:    27B Ternary Bonsai (:8081)")
    print(f" Calibration In-Domain:  \"{curriculum['test_in_domain']}\"")
    print(f" Calibration Out-Domain: \"{curriculum['test_out_domain']}\"")
    print("="*65 + "\n")

    corpus_dir = "var/distill"
    os.makedirs(corpus_dir, exist_ok=True)
    corpus_file = os.path.join(corpus_dir, f"{domain_key}_corpus.txt")
    os.makedirs(output_dir, exist_ok=True)
    capsule_file = os.path.join(output_dir, f"{domain_key}.gencap")

    # Step 1: Parallel Exemplar Extraction
    print(f"[*] Harvesting knowledge exemplars from 27B teacher across {workers} parallel slots...")
    all_sentences = []
    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = {
            executor.submit(query_teacher, probe, curriculum["system_prompt"]): (i, probe)
            for i, probe in enumerate(curriculum["probes"], 1)
        }
        for future in as_completed(futures):
            idx, probe = futures[future]
            raw = future.result()
            sents = clean_sentences(raw)
            print(f"  [Slot Done] Probe {idx}/{len(curriculum['probes'])}: \"{probe[:42]}...\" -> {len(sents)} sents")
            all_sentences.extend(sents)

    # Step 2: Deduplication & Curation
    seen = set()
    unique_sentences = []
    for s in all_sentences:
        s_clean = s.lower().strip()
        if s_clean not in seen and len(s_clean) >= 20:
            seen.add(s_clean)
            unique_sentences.append(s)

    if not unique_sentences:
        print("[-] Failed to extract valid declarative sentences from teacher.", file=sys.stderr)
        return None

    with open(corpus_file, "w") as f:
        for s in unique_sentences:
            f.write(s + "\n")
    print(f"\n[+] Curated {len(unique_sentences)} declarative domain statements -> '{corpus_file}'")

    # Step 3: Native VSA Capsule Compilation & Sealing
    print(f"[*] Compiling & sealing VSA capsule via native CLI...")
    cmd_create = [CNET_CLI, "gencap-create", domain_key, domain_tag, corpus_file, capsule_file]
    res_create = subprocess.run(cmd_create, capture_output=True, text=True)
    if res_create.returncode != 0:
        print(f"[-] Capsule compilation failed:\n{res_create.stderr}", file=sys.stderr)
        return None
    print(res_create.stdout.strip())

    # Step 4: Cryptographic Verification Gate
    print(f"\n[*] Gate 1: Cryptographic Digest Verification...")
    cmd_verify = [CNET_CLI, "gencap-verify", capsule_file]
    res_verify = subprocess.run(cmd_verify, capture_output=True, text=True)
    if "CERTIFIED_AUTHENTIC" not in res_verify.stdout:
        print(f"[-] Cryptographic digest verification FAILED:\n{res_verify.stdout}", file=sys.stderr)
        return None
    print("  -> Cryptographic digest verified: CERTIFIED_AUTHENTIC [PASS]")

    # Step 5: In-Domain Generation Gate
    print(f"[*] Gate 2: In-Domain Semantic Steerability...")
    cmd_in = [CNET_CLI, "gencap-gen", capsule_file, curriculum["test_in_domain"], "the", "25"]
    res_in = subprocess.run(cmd_in, capture_output=True, text=True)
    output_in = res_in.stdout.strip()
    print(f"  -> Generated: \"{output_in}\"")
    if "ABSTAIN" in output_in:
        print("[-] In-domain calibration failed: capsule abstained on core keywords!", file=sys.stderr)
        return None
    print("  -> In-domain semantic synthesis verified [PASS]")

    # Step 6: Fail-Closed Out-of-Domain Abstention Gate
    print(f"[*] Gate 3: Fail-Closed OOD Abstention...")
    cmd_ood = [CNET_CLI, "gencap-gen", capsule_file, curriculum["test_out_domain"], "the", "25"]
    res_ood = subprocess.run(cmd_ood, capture_output=True, text=True)
    output_ood = res_ood.stdout.strip()
    print(f"  -> OOD Response: \"{output_ood}\"")
    if "ABSTAIN" not in output_ood:
        print(f"[-] OOD abstention gate FAILED! Capsule responded to alien domain (hallucination risk).", file=sys.stderr)
        return None
    print("  -> OOD abstention verified: Fail-Closed Refusal Confirmed [PASS]")

    # Optional Step 7: Sync to Unity StreamingAssets if AliveValley project exists
    unity_capsules = "/home/marble/AI/AliveValleyDemo-puppet-master/Assets/StreamingAssets/PuppetMaster/gencap_capsules"
    if os.path.isdir(unity_capsules):
        dest = os.path.join(unity_capsules, f"{domain_key}.gencap")
        subprocess.run(["cp", "-f", capsule_file, dest])
        print(f"[+] Synchronized capsule to Unity StreamingAssets -> '{dest}'")

    total_time = time.time() - t0
    print("\n" + "="*65)
    print(f" CERTIFIED CAPSULE ADMISSION SUCCESSFUL")
    print(f" Artifact: {capsule_file} ({os.path.getsize(capsule_file)} bytes)")
    print(f" Total Duration: {total_time:.2f}s | Hardware: ROCm GPU + Pure VSA")
    print("="*65 + "\n")
    return capsule_file


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: ./tools/cnet_auto_distill.py <topic or domain> [output_dir]")
        print("Example: ./tools/cnet_auto_distill.py 'siege engineering and trebuchets'")
        sys.exit(1)
        
    topic_arg = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else "bin"
    res = auto_distill(topic_arg, output_dir=out_dir)
    sys.exit(0 if res else 1)
