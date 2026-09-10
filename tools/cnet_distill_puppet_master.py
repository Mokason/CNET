#!/usr/bin/env python3
"""
tools/cnet_distill_puppet_master.py - Distill Knowledge from 27B Teacher into 10 Unity Puppet Master Capsules
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

PUPPET_MASTER_CURRICULA = {
    "pm_director_pacing": {
        "domain_tag": "PM_DIRECTOR",
        "system_prompt": "You are a master game director AI architect specializing in narrative pacing, dramatic tension curves, and crisis orchestration in systemic games. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain narrative dramatic pacing, tension curves, and three-act structure in autonomous game director AI.",
            "Explain how the director decides when to escalate tension, introduce complications, or resolve conflicts.",
            "Explain narrative beat sequencing and transitioning from silence to rumor, crisis, and resolution.",
            "Explain the balance between player agency and director-driven dramatic pacing in systemic simulations.",
            "Explain how narrative moves (open, advance, escalate, resolve) orchestrate world events.",
            "Explain how the director modulates crisis frequency based on player fatigue and story progression."
        ],
        "test_in_domain": "narrative director dramatic tension pacing arc escalate resolve",
        "test_out_domain": "baking sourdough bread oven flour hydration yeast loaf"
    },
    "pm_steward_economy": {
        "domain_tag": "PM_STEWARD",
        "system_prompt": "You are a chief economic simulation engineer and medieval resource steward. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain seasonal resource management, grain stockpiling, and famine prevention in simulation games.",
            "Explain labor allocation across farming, mining, smithing, and guard duties in settlement simulation.",
            "Explain emergency relief distribution, grain rations, and public welfare during harsh winters or crises.",
            "Explain taxation policies, seasonal levies, and balancing citizen happiness against treasury wealth.",
            "Explain merchant trade networks, import-export surpluses, and commodity pricing in autonomous economies.",
            "Explain economic recovery cycles after bandit raids, monster attacks, or drought in game villages."
        ],
        "test_in_domain": "steward village economy grain stockpile harvest tax levy famine relief",
        "test_out_domain": "astronomy telescope nebulae galaxies redshift light spectrum"
    },
    "pm_cast_lifecycle": {
        "domain_tag": "PM_CAST",
        "system_prompt": "You are a senior NPC behavioral simulation engineer. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain autonomous NPC daily routines, diurnal cycles, work shifts, and home sleep schedules.",
            "Explain NPC state machines transitioning between idle, working, social smalltalk, wary, and guard states.",
            "Explain panic and survival behaviors: when an NPC decides to flee, sound the alarm, or seek shelter.",
            "Explain pairwise NPC interactions, social greetings, and cooperative task execution in living towns.",
            "Explain emotional distress, suspicion levels, and how rumors change NPC behavior toward strangers.",
            "Explain NPC role specialization: how guards, healers, merchants, and farmers prioritize daily goals."
        ],
        "test_in_domain": "npc daily routine work shift idle wary flee guard schedule cast lifecycle",
        "test_out_domain": "marine biology coral reef photosynthesis plankton whale migration"
    },
    "pm_system_isekai": {
        "domain_tag": "PM_SYSTEM",
        "system_prompt": "You are a lead RPG systems designer and progression mechanics architect. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain game system attribute progression, level thresholds, and stat scaling in RPG mechanics.",
            "Explain trait inheritance, perk selection, and blessing or curse effects in systemic game rules.",
            "Explain weighted loot drop tables, item rarity tiers, and economic reward balancing.",
            "Explain progression rites, class promotions, and character advancement triggers.",
            "Explain systemic status effects including fatigue, poison, inspiration, and elemental resistance.",
            "Explain deterministic rule verification and how game ledgers validate character inventory changes."
        ],
        "test_in_domain": "rpg game mechanics traits loot table progression rites stats attributes",
        "test_out_domain": "french pastry chocolate croissant bakery recipe rolling butter dough"
    },
    "pm_wilds_ecology": {
        "domain_tag": "PM_WILDS",
        "system_prompt": "You are a computational ecologist and game wildlife behavior designer. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain ecosystem simulation, predator-prey food chains, and wild fauna population dynamics.",
            "Explain beast aggression pulses, territorial aggression, and migration patterns toward human settlements.",
            "Explain weather and seasonal impacts on wildlife behavior, hibernation, and forage scarcity.",
            "Explain monster hunting mechanics, tracking trails, animal scent cues, and beast warning signs.",
            "Explain pack hunting tactics, alpha predator leadership, and beast encirclement behaviors.",
            "Explain territorial boundaries, animal dens, and environmental hazard zones in wilderness biomes."
        ],
        "test_in_domain": "wilds ecology fauna beast aggression predator migration hunt den",
        "test_out_domain": "renaissance oil painting canvas perspective brushwork glaze varnish"
    },
    "pm_chronicle_memory": {
        "domain_tag": "PM_CHRONICLE",
        "system_prompt": "You are a narrative memory systems architect and world event history researcher. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain episodic memory compaction and transforming raw simulation events into historical chronicle beats.",
            "Explain rumor propagation algorithms: how news travels between NPCs across space and time.",
            "Explain event ring buffers, witness logging, and recording who saw a crime or heroic deed.",
            "Explain how historical deeds and past crimes influence faction reputation and NPC trust years later.",
            "Explain narrative chronicle summaries: converting long tick histories into concise story beats.",
            "Explain forgetting curves, memory decay, and preserving salient pivotal events in game lore."
        ],
        "test_in_domain": "chronicle world memory event ring rumor propagation narrative beat history",
        "test_out_domain": "knitting wool socks needles purl stitch crochet yarn pattern"
    },
    "pm_voice_dialogue": {
        "domain_tag": "PM_VOICE",
        "system_prompt": "You are a dialogue systems architect and NPC conversational flavor designer. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain NPC dialogue selection based on speaker role, emotional state, and recent event rings.",
            "Explain dynamic game barks: contextual one-liners for combat alerts, work complaints, and casual gossip.",
            "Explain accusatory and suspicious dialogue lines when an NPC discovers a crime or theft.",
            "Explain muster calls, battle cries, and rallying shouts used by village guards during an attack.",
            "Explain merchant trade pitches, tavern smalltalk, and intimate comforting lines during grief.",
            "Explain grounded dialogue generation where every spoken line maps to a certified event ID."
        ],
        "test_in_domain": "npc voice dialogue barks smalltalk accuse muster speech acts personality",
        "test_out_domain": "quantum electrodynamics feynman diagrams photon electron vacuum polarization"
    },
    "pm_tactics_combat": {
        "domain_tag": "PM_TACTICS",
        "system_prompt": "You are a senior combat AI engineer and squad tactics designer. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain squad combat coordination, roles (tank, flanker, ranged support), and focus-fire targeting.",
            "Explain dynamic aggro management and threat evaluation based on damage dealt and proximity.",
            "Explain tactical flanking maneuvers, taking cover behind obstacles, and suppressing fire.",
            "Explain defensive shield-wall formations, back-to-back defense, and protecting vulnerable units.",
            "Explain tactical retreat thresholds: when a squad decides to fall back, regroup, or surrender.",
            "Explain ambush execution, choke-point control, and surprise attack coordination in combat AI."
        ],
        "test_in_domain": "squad combat tactics aggro flanking cover defense retreat maneuvers",
        "test_out_domain": "pottery ceramic glaze kiln firing clay wheel sculpting vase"
    },
    "pm_perception_sensors": {
        "domain_tag": "PM_PERCEPTION",
        "system_prompt": "You are a game AI stealth and perception systems programmer. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain sensory perception models in game AI: vision cones, peripheral vision, and line-of-sight raycasts.",
            "Explain acoustic perception: noise radius, footstep decibels, and sound propagation through walls.",
            "Explain dynamic stealth detection: how crouching, shadow light levels, and camouflage affect detection rates.",
            "Explain alertness stage machines: transitioning from unaware to suspicious, searching, and full alert.",
            "Explain search party investigations: how NPCs investigate the last known position of a suspicious noise.",
            "Explain distraction mechanics: how thrown stones, whistling, or body discoveries alter sensory focus."
        ],
        "test_in_domain": "perception vision cone line of sight noise stealth alert stages search",
        "test_out_domain": "ballet choreography pirouette arabesque stage rehearsal tutu shoes"
    },
    "pm_kinetics_ragdoll": {
        "domain_tag": "PM_KINETICS",
        "system_prompt": "You are a physics-driven animation and puppet ragdoll mechanics engineer in Unity. Output clean, factual, declarative sentences. Avoid conversational filler, numbered lists, or markdown styling. One clear technical statement per line.",
        "probes": [
            "Explain physics-driven ragdoll animation blending and configurable joint muscle spring dampers.",
            "Explain center-of-mass balance preservation, inverted pendulum models, and dynamic foot placement.",
            "Explain impact impulse distribution: how heavy blows push joints into ragdoll compliance before recovery.",
            "Explain stumble recovery algorithms: taking compensatory steps to prevent full ragdoll collapse.",
            "Explain get-up state transitions: detecting grounded orientation (supine vs prone) and selecting stand-up animations.",
            "Explain joint torque limits, angular momentum, and preventing unnatural joint hyper-extension in puppets."
        ],
        "test_in_domain": "puppet ragdoll physics muscle stiffness balance stumble recovery joint torque",
        "test_out_domain": "gourmet wine tasting tannins oak barrel fermentation vineyard vintage"
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
    if domain_key not in PUPPET_MASTER_CURRICULA:
        print(f"[-] Unknown domain '{domain_key}'", file=sys.stderr)
        return False

    spec = PUPPET_MASTER_CURRICULA[domain_key]
    domain_tag = spec["domain_tag"]
    system_prompt = spec["system_prompt"]
    probes = spec["probes"]

    print(f"\n=================================================================")
    print(f" Distilling Puppet Master Domain: {domain_key} (Tag: {domain_tag})")
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
        print(f"Starting parallel distillation across all {len(PUPPET_MASTER_CURRICULA)} Puppet Master domains...")
        for k in PUPPET_MASTER_CURRICULA.keys():
            distill_domain(k, workers=4)
        total_time = time.time() - t0
        print(f"=================================================================")
        print(f" ALL {len(PUPPET_MASTER_CURRICULA)} PUPPET MASTER DOMAINS DISTILLED IN {total_time:.1f}s")
        print(f"=================================================================")
    else:
        distill_domain(target, workers=4)
