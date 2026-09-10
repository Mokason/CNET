#!/usr/bin/env python3
"""
CNET Distillation Pipeline: Feudal Politics & Kinship Domestic Capsules
Distills high-precision domain corpora from 27B Bonsai teacher into portable,
certified CNET VSA Generative Knowledge Capsules (.gencap).
"""

import sys
import os
import json
import re
import time
import urllib.request
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed

TEACHER_URL = "http://127.0.0.1:8081/v1/chat/completions"
CNET_CLI = "./bin/cnet_vsa_cli"

FEUDAL_KINSHIP_CURRICULA = {
    "pm_feudal_politics": {
        "domain_tag": "PM_FEUDAL",
        "system_prompt": "You are a medieval constitutional scholar, feudal law jurist, and realm institutional historian. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain feudal contracts: liege-vassal obligations, auxilium et consilium, and military levy quotas.",
            "Explain serfdom and unfree peasant tenures, corvée labor duties, manorial courts, and death duties.",
            "Explain the causes of peasant unrest, tax default penalties, and how famine triggers agrarian revolt.",
            "Explain royal borough charters, buying town freedom, and the principle of city air makes you free.",
            "Explain fragmented feudal jurisdiction: high justice vs low justice, manorial reeves, and border sanctuary loopholes.",
            "Explain calling the banners: conscription levies, wartime economic drain on villages, and military muster orders."
        ],
        "test_in_domain": "feudal liege vassal serfdom levy tax quota charter revolt jurisdiction justice",
        "test_out_domain": "quantum optics laser interferometer photon entanglement cavity mirror"
    },
    "pm_kinship_domestic": {
        "domain_tag": "PM_KINSHIP",
        "system_prompt": "You are an anthropological historian and medieval peasant household sociologist. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain medieval peasant household dynamics: shared hearth anchors, morning departure, and evening reconvening.",
            "Explain familial division of labor: elder grandparents tending hearths and children while adults work demesne fields.",
            "Explain generational grief ripple effects: how a soldier's death in battle alters spouse and orphan child routines.",
            "Explain workshop tenure inheritance: passing blacksmithing, milling, and farming rights to eldest adult heirs.",
            "Explain domestic emotional barks: affectionate parenting warnings, spousal economic planning, and elder folklore.",
            "Explain protective mobilization: family members defending cottage perimeters and caring for sick or wounded kin."
        ],
        "test_in_domain": "family kinship household hearth spouse child elder inheritance domestic grief",
        "test_out_domain": "petroleum distillation hydrocarbon cracking refinery fraction column octane"
    }
}

def query_teacher(prompt, system_prompt):
    payload = {
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": prompt}
        ],
        "temperature": 0.3,
        "max_tokens": 1024
    }
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(TEACHER_URL, data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=45) as resp:
            res = json.loads(resp.read().decode("utf-8"))
            content = res["choices"][0]["message"]["content"]
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
    if domain_key not in FEUDAL_KINSHIP_CURRICULA:
        print(f"[-] Unknown domain '{domain_key}'", file=sys.stderr)
        return False

    spec = FEUDAL_KINSHIP_CURRICULA[domain_key]
    domain_tag = spec["domain_tag"]
    system_prompt = spec["system_prompt"]
    probes = spec["probes"]

    print(f"\n=================================================================")
    print(f" Distilling Domain: {domain_key} (Tag: {domain_tag})")
    print(f" Teacher: 27B Ternary Bonsai (:8081) [Parallel Slots: {workers}]")
    print(f"=================================================================\n")

    t_start = time.time()
    curated_sentences = []

    def probe_worker(item):
        idx, probe = item
        raw = query_teacher(probe, system_prompt)
        sents = clean_sentences(raw)
        return idx, probe, sents

    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = [executor.submit(probe_worker, (i, p)) for i, p in enumerate(probes, 1)]
        for fut in as_completed(futures):
            idx, p, sents = fut.result()
            curated_sentences.extend(sents)
            print(f"  [Slot Done] Probe {idx}/{len(probes)}: \"{p[:45]}...\" -> {len(sents)} sents")

    print(f"\nTotal curated domain exemplars: {len(curated_sentences)}")

    os.makedirs("var/distill", exist_ok=True)
    corpus_file = f"var/distill/{domain_key}_corpus.txt"
    capsule_file = f"bin/{domain_key}.gencap"

    with open(corpus_file, "w", encoding="utf-8") as f:
        for s in curated_sentences:
            f.write(s + "\n")
        # Add probe sentences as direct anchored exemplars
        for p in probes:
            f.write(p + "\n")
    print(f"Saved domain corpus to '{corpus_file}'.")

    # Step 1: Create and Seal Capsule via native CNET CLI
    print(f"\nCompiling & sealing '{capsule_file}'...")
    cmd_create = [CNET_CLI, "gencap-create", domain_key, domain_tag, corpus_file, capsule_file]
    res_create = subprocess.run(cmd_create, capture_output=True, text=True)
    print(res_create.stdout)
    if res_create.returncode != 0:
        print(f"[-] Failed to create capsule: {res_create.stderr}")
        return False

    # Step 2: Verification
    cmd_inspect = [CNET_CLI, "gencap-inspect", capsule_file]
    res_inspect = subprocess.run(cmd_inspect, capture_output=True, text=True)
    print(res_inspect.stdout)

    t_elapsed = time.time() - t_start
    print(f"[+] Successfully distilled and sealed '{domain_key}' in {t_elapsed:.1f}s.")
    return True

if __name__ == "__main__":
    print("[*] Starting Feudal & Kinship Distillation...")
    for d in FEUDAL_KINSHIP_CURRICULA:
        ok = distill_domain(d, workers=4)
        if not ok:
            sys.exit(1)
    print("\n[+] Feudal & Kinship Distillation Complete!")
