#!/usr/bin/env python3
"""
CNET Distillation Pipeline: Anime Isekai Guilds, Crafting & Civic Order Capsules
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

ISEKAI_GUILD_CURRICULA = {
    "pm_combat_guilds": {
        "domain_tag": "PM_COMBAT_GUILDS",
        "system_prompt": "You are a fantasy guild archivist, martial historian, and tactical guildmaster. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain Adventurers Guild structure: ranks from F copper to S adamantite, quest commission boards, and monster culling bounties.",
            "Explain Hunters Guild operations: wilderness tracking, monster skinning, trap laying, and beast migration monitoring.",
            "Explain Mercenary Guild contracts: caravan protection, city gate garrison defense, and private military company charters.",
            "Explain Assassins Guild underworld bylaws: shadow contracts, anonymous dead drops, poison craft, and bounty fulfillment.",
            "Explain Chivalric Knight Orders: knightly oaths, squires, heraldic honor, royal charters, and defensive vanguard charges."
        ],
        "test_in_domain": "adventurers guild rank bounty quest monster hunter mercenary assassin knight chivalry",
        "test_out_domain": "quantum optics laser interferometer photon entanglement cavity mirror"
    },
    "pm_craft_production": {
        "domain_tag": "PM_CRAFT_PRODUCTION",
        "system_prompt": "You are a master artisan, alchemical scholar, architectural engineer, and fantasy crafting instructor. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain Blacksmiths Guild standards: ore smelting, Damascus pattern welding, tempering, and rune quenching techniques.",
            "Explain Alchemists and Pharmacists Guilds: potion distillation, herb extraction, mana stabilization, and restorative elixirs.",
            "Explain Tailors and Textile Guilds: enchanted thread weaving, insulated winter cloaks, and beast leather tanning.",
            "Explain Cooks Guild and culinary alchemy: monster meat tenderizing, stat-boosting buff recipes, and otherworldly dishes like ramen and mayonnaise.",
            "Explain Carpenters and Architects Guilds: settlement blueprints, timber framing, hot springs, aqueducts, and defensive stone ramparts.",
            "Explain Ink, Paper, and Printing Guilds: pulp pressing, moveable type, grimoire mass production, and modern knowledge dissemination."
        ],
        "test_in_domain": "blacksmith alchemy potion forge blueprint architect cooking recipe printing paper textile weaving",
        "test_out_domain": "petroleum distillation hydrocarbon cracking refinery fraction column octane"
    },
    "pm_civic_magic_order": {
        "domain_tag": "PM_CIVIC_MAGIC_ORDER",
        "system_prompt": "You are a fantasy institutional theorist, arcane academic, ecclesiastical jurist, and underworld historian. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain Merchants Guild and Commerce Associations: trade routes, market stalls, regional price arbitrage, and merchant shop ownership.",
            "Explain Thieves Guild and underworld trade: black markets, fencing stolen goods, lockpicking codes, and dark debt contracts.",
            "Explain Mages Guild and Magic Academies: elemental circles, spell research, mana crystals, and grimoire inscription.",
            "Explain Church and Temple hierarchy: holy blessings, saintly rites, divine purifications, and ecclesiastical heresy inquisitions.",
            "Explain Beast Tamers Guild: monster familiar bonds, empathy resonance, beast saddles, and slime ranching utility.",
            "Explain Appraisers Guild: analytical status appraisal, detecting curses, identifying magical item tiers, and forgery revelation.",
            "Explain Guild of Nobles and Peerage: court etiquette, heraldic lineage, royal monopolies, and political lobbying.",
            "Explain Explorers and Cartographers Guilds: labyrinth charting, fog of war surveying, and ancient ruins archaeology.",
            "Explain Dungeon Management Guilds: dungeon core stability, monster respawn ecology, trap maintenance, and floor hazard calibration.",
            "Explain the Receptionists Faction: cross-guild communication networks, quest applicant screening, rumor brokerages, and receptionist solidarity."
        ],
        "test_in_domain": "merchants thieves mages church beast tamer appraiser nobles cartographer dungeon receptionist guild",
        "test_out_domain": "semiconductor photolithography silicon wafer chemical vapor deposition transistor gate"
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
    if domain_key not in ISEKAI_GUILD_CURRICULA:
        print(f"[-] Unknown domain '{domain_key}'", file=sys.stderr)
        return False

    spec = ISEKAI_GUILD_CURRICULA[domain_key]
    domain_tag = spec["domain_tag"]
    system_prompt = spec["system_prompt"]
    probes = spec["probes"]

    print(f"\n=======================================================")
    print(f"[*] Distilling '{domain_key}' [{domain_tag}] via 27B Bonsai")
    print(f"[*] Total Probes: {len(probes)} | Concurrency: {workers}")
    print(f"=======================================================")

    t0 = time.time()
    corpus_sentences = []

    with ThreadPoolExecutor(max_workers=workers) as executor:
        future_to_probe = {executor.submit(query_teacher, probe, system_prompt): probe for probe in probes}
        for future in as_completed(future_to_probe):
            probe = future_to_probe[future]
            try:
                res = future.result()
                sents = clean_sentences(res)
                corpus_sentences.extend(sents)
                print(f"  [+] Probe '{probe[:45]}...' -> {len(sents)} sentences")
            except Exception as exc:
                print(f"  [-] Probe failed: {exc}", file=sys.stderr)

    # Deduplicate while preserving order
    unique_sentences = []
    seen = set()
    for s in corpus_sentences:
        s_norm = s.lower().strip()
        if s_norm not in seen and len(s) >= 25:
            seen.add(s_norm)
            unique_sentences.append(s)

    print(f"[*] Distilled {len(unique_sentences)} unique sentences in {time.time() - t0:.2f}s")
    if len(unique_sentences) < 15:
        print(f"[-] Corpus too sparse ({len(unique_sentences)} lines). Aborting.", file=sys.stderr)
        return False

    os.makedirs("var/distill", exist_ok=True)
    os.makedirs("bin", exist_ok=True)
    corpus_path = f"var/distill/{domain_key}_corpus.txt"
    with open(corpus_path, "w", encoding="utf-8") as f:
        for s in unique_sentences:
            f.write(s + "\n")

    capsule_path = f"bin/{domain_key}.gencap"
    print(f"[*] Compiling capsule via CNET VSA CLI -> {capsule_path}")

    cmd = [
        CNET_CLI, "capsule-compile",
        corpus_path,
        capsule_path,
        domain_tag
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[-] Compilation error:\n{res.stderr}", file=sys.stderr)
        return False
    print(res.stdout.strip())

    # Verification probe test
    print(f"[*] Running Verification & Separation Gates on {capsule_path}")
    verify_cmd = [
        CNET_CLI, "capsule-verify",
        capsule_path,
        spec["test_in_domain"],
        spec["test_out_domain"]
    ]
    v_res = subprocess.run(verify_cmd, capture_output=True, text=True)
    if v_res.returncode != 0:
        print(f"[-] Verification failed:\n{v_res.stderr}\n{v_res.stdout}", file=sys.stderr)
        return False
    print(v_res.stdout.strip())
    print(f"[✓] Successfully Certified & Sealed: {capsule_path} ({time.time() - t0:.2f}s total)")
    return True

if __name__ == "__main__":
    domains = list(ISEKAI_GUILD_CURRICULA.keys())
    if len(sys.argv) > 1:
        domains = sys.argv[1:]

    success = True
    for d in domains:
        if not distill_domain(d):
            success = False
            break

    if success:
        print("\n[✓] All Isekai Guild Knowledge Capsules compiled & certified successfully!")
        sys.exit(0)
    else:
        print("\n[-] Pipeline failure.", file=sys.stderr)
        sys.exit(1)
