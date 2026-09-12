#!/usr/bin/env python3
"""
tools/cnet_isekai_rpg_crawler.py — CNET Specialized Isekai & RPG Knowledge Crawler
Compounding domain-specific competence for:
  • Anime / Isekai Tropes & Archetypes (Chunibyo, Megumin, Darkness, Aqua, Kazuma)
  • Emergent Narrative, Party Banter & Friction
  • Dynamic Constellation Skill Trees & Boon/Bane Comedy Duality
  • Autonomous Feudal World, Caravans & Guild Economy

Anti-Collapse Invariant: Trained purely from external 27B teacher model on ROCm (:8081).
Zero CNET self-training. Certified with strict 3-stage fail-closed gates.
"""

import sys
import os
import re
import json
import time
import signal
import urllib.request
import subprocess
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed

TEACHER_URL = os.getenv("CNET_TEACHER_URL", "http://127.0.0.1:8081/v1/chat/completions")
CNET_CLI = os.getenv("CNET_CLI", "./bin/cnet_vsa_cli")
CALIB_TARGET_IN = os.environ.get("CNET_CALIB_TARGET_IN", "0.80")   # in-domain accept fraction (measured operating point, see result/cnet_vsa_margin_gate_calibration_20260911.md)
CALIB_TARGET_NEG = os.environ.get("CNET_CALIB_TARGET_NEG", "0.90") # negative reject fraction
VSA_ENCODER = os.environ.get("CNET_VSA_ENCODER", "default")   # "default" = the encoder the C sweep gate selected
# A registry that ships a lexicon (<output_dir>/registry.lex, 2026-09-12 rollout) is sealed with the LEX encoder under
# that table: new capsules must use the same encoder and the same lexicon or the registry refuses them at admission.
_REGISTRY_LEX = os.environ.get("CNET_VSA_LEXICON", "")
def _lexicon_env(output_dir):
    """Environment for CLI calls: CNET_VSA_LEXICON pointing at the registry's shipped table when it exists."""
    env = dict(os.environ)
    lex = _REGISTRY_LEX or os.path.join(str(output_dir), "registry.lex")
    if os.path.exists(lex):
        env["CNET_VSA_LEXICON"] = lex
    return env
def _encoder_for(output_dir):
    if VSA_ENCODER != "default":
        return VSA_ENCODER
    return "lex" if os.path.exists(_REGISTRY_LEX or os.path.join(str(output_dir), "registry.lex")) else "default"
STATE_DIR = Path("var/isekai_rpg_crawler")
STATE_FILE = STATE_DIR / "state.json"
STATUS_MD = Path("docs/ISEKAI_RPG_NETWORK_STATUS.md")
CAPSULES_DIR = Path("bin/capsules")
UNITY_CAPSULES_DIR = Path("/home/marble/AI/AliveValleyDemo-puppet-master/Assets/StreamingAssets/PuppetMaster/gencap_capsules")

STOP_REQUESTED = False

ALIEN_BENCHMARK_PROBES = [
    "How do quantum qubits maintain coherent superposition on a Bloch sphere before decoherence?",
    "What are the specific chemical flavor notes and oak barrel aging requirements of Kentucky bourbon whiskey?",
    "What are the optimal temperature and hydration ratios for proofing sourdough starter in artisan bread baking?",
    "What excavation techniques are used to preserve fragile fossilized dinosaur skull specimens in sandstone?",
    "What are the traditional stitch patterns and tension techniques used in Fair Isle wool sweater knitting?",
    "How does high-fashion haute couture draping differ from standard ready-to-wear pattern drafting?",
    "How are deep-sea coral reef ecosystems affected by ocean acidification and thermal bleaching?",
    "What are the thermodynamic stages of fluid catalytic cracking in petroleum refineries?"
]


def signal_handler(signum, frame):
    global STOP_REQUESTED
    print(f"\n[!] Signal {signum} received. Gracefully finishing current capsule and saving state...", flush=True)
    STOP_REQUESTED = True


signal.signal(signal.SIGINT, signal_handler)
signal.signal(signal.SIGTERM, signal_handler)
if hasattr(signal, "SIGHUP"):
    signal.signal(signal.SIGHUP, signal.SIG_IGN)


ISEKAI_RPG_SEEDS = [
    # Anime & Isekai Archetypes & Tropes
    "overpowered explosion sorcery",
    "masochistic crusader defense",
    "divine debauchery and holy debt",
    "cynical pragmatic scoundrel tactics",
    "accidental demon overlord misunderstandings",
    "isekai cheat skill comedic subversions",
    "apocalyptic one spell exhaustion",
    "chunibyo incantations and grand monologues",
    "cursed relic haggling and shoe throwing",
    "isekai reincarnation goddess misunderstandings",

    # Emergent Gameplay Storytelling & Party Dynamics
    "emergent narrative party friction",
    "dysfunctional adventuring party banter",
    "npc rumor propagation and magistrate fines",
    "autonomous guild emergency bulletin bounties",
    "feudal frontier realm diplomacy and skirmishes",
    "tavern revelry and holy mead purification",
    "collateral municipal infrastructure destruction",
    "dwarven trade caravan mountain ambushes",
    "ambient townsfolk reactions and dialogue",

    # Dynamic Skill Trees & Perk Duality
    "dynamic constellation skill graphs",
    "boon and bane comedy balancing mechanics",
    "immutable player perk progression invariants",
    "alchemical thermal runaway crafting",
    "collateral demolition masonry",
    "monster taming through culinary bribery",
    "chimerical beast herding and grazing"
]


def query_teacher(prompt: str, system_prompt: str, max_tokens: int = 256, temperature: float = 0.25) -> str:
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


def normalize_key(name: str) -> str:
    key = re.sub(r"[^a-z0-9_]", "_", name.lower().strip().replace(" ", "_"))
    return re.sub(r"_+", "_", key).strip("_")


class IsekaiCrawlerState:
    def __init__(self):
        self.visited: dict[str, dict] = {}
        self.frontier: list[dict] = []
        self.failed: list[dict] = []
        self.graph_edges: list[dict] = []
        self.total_tokens: int = 0
        self.start_time: float = time.time()

    def load(self):
        if STATE_FILE.is_file():
            try:
                with open(STATE_FILE, "r") as f:
                    data = json.load(f)
                self.visited = data.get("visited", {})
                self.frontier = data.get("frontier", [])
                self.failed = data.get("failed", [])
                self.graph_edges = data.get("graph_edges", [])
                self.total_tokens = data.get("total_tokens", 0)
                print(f"[+] Loaded existing state: {len(self.visited)} visited, {len(self.frontier)} in frontier.")
            except Exception as e:
                print(f"[-] Error loading state: {e}. Starting fresh.")
                self.init_seeds()
        else:
            self.init_seeds()

    def init_seeds(self):
        for seed in ISEKAI_RPG_SEEDS:
            k = normalize_key(seed)
            if k not in self.visited:
                self.frontier.append({"topic": seed, "parent": "seed_origin", "depth": 0})
        self.save()

    def save(self):
        STATE_DIR.mkdir(parents=True, exist_ok=True)
        tmp = STATE_FILE.with_suffix(".tmp")
        data = {
            "visited": self.visited,
            "frontier": self.frontier,
            "failed": self.failed,
            "graph_edges": self.graph_edges,
            "total_tokens": self.total_tokens,
            "last_updated": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime()),
            "uptime_seconds": time.time() - self.start_time
        }
        with open(tmp, "w") as f:
            json.dump(data, f, indent=2)
        tmp.replace(STATE_FILE)


def discover_adjacent_topics(topic: str, corpus_sample: list[str]) -> list[str]:
    sample_text = "\n".join(corpus_sample[:6])
    prompt = (
        f"Domain: {topic}\n"
        f"Exemplar Statements:\n{sample_text}\n\n"
        "Identify 4 to 5 specialized adjacent concepts, gameplay mechanics, anime/isekai comedic tropes, "
        "or emergent storytelling systems directly connected to these ideas. "
        "Output one concept per line. Be creative, specific, and comedic (e.g. 'cabbage monster migratory harvesting', "
        "'court ordered community service quests', 'rock paper scissors duel exploitation', 'fluorescent alchemical fumes')."
    )
    sys_prompt = "You are an anime isekai game designer. Output exactly 4-5 specific domain names, one per line, without numbering or bullets."
    raw = query_teacher(prompt, sys_prompt, max_tokens=160, temperature=0.35)
    lines = [l.strip() for l in raw.splitlines() if len(l.strip()) >= 5]
    cleaned = []
    for l in lines:
        l_clean = re.sub(r"^[\*\-\•\d+\.\s]+", "", l).strip().lower()
        if l_clean and len(l_clean) >= 6:
            cleaned.append(l_clean)
    return cleaned[:5]


def distill_and_certify(topic: str, output_dir: Path) -> dict | None:
    t0 = time.time()
    domain_key = normalize_key(topic)

    # 1. Generate Domain Tag
    sys_tag = "You are an anime isekai and RPG taxonomy classifier. Output exactly one short uppercase alphanumeric tag with underscores representing this domain. Nothing else."
    raw_tag = query_teacher(f"Topic: {topic}", sys_tag, max_tokens=16, temperature=0.1)
    tag_clean = re.sub(r"[^A-Z0-9_]", "", raw_tag.upper()) or "ISEKAI_RPG"

    # 2. Generate 6 Targeted Deep Probes
    sys_probes = (
        "You are an anime isekai RPG curriculum architect. Output 6 precise, deep probe questions to extract all essential "
        "gameplay mechanics, comedic anime tropes, Boon/Bane dualities, party dynamics, and narrative rules for this domain. "
        "Return one question per line without numbering."
    )
    raw_probes = query_teacher(f"Domain: {topic}", sys_probes, max_tokens=320, temperature=0.3)
    probes = [p.strip() for p in raw_probes.splitlines() if len(p.strip()) > 15]
    if len(probes) < 3:
        probes = [
            f"Explain the core gameplay mechanisms, character traits, and comedic rules of {topic}.",
            f"Explain the Boon and Bane trade-offs, party friction, and social consequences of {topic}.",
            f"Explain the procedural world reactions, NPC dialogues, and systemic constraints of {topic}."
        ]

    # 3. Parallel Harvesting from 27B Teacher
    sys_specialist = (
        f"You are a lead systems and narrative designer for an anime isekai comedy RPG specializing in {topic}. "
        "Output clean, factual, declarative sentences defining this domain's concrete rules, comedic tropes, "
        "double-edged mechanics, character interactions, or systemic world reactions. "
        "Avoid conversational filler, introductory remarks, or markdown. One clear declarative statement per line."
    )
    all_sentences = []
    with ThreadPoolExecutor(max_workers=4) as executor:
        futures = {
            executor.submit(query_teacher, probe, sys_specialist): (i, probe)
            for i, probe in enumerate(probes[:6], 1)
        }
        for future in as_completed(futures):
            idx, probe = futures[future]
            raw = future.result()
            sents = clean_sentences(raw)
            all_sentences.extend(sents)

    # 4. Deduplicate & Curate
    seen = set()
    unique_sentences = []
    for s in all_sentences:
        s_clean = s.lower().strip()
        if s_clean not in seen and len(s_clean) >= 20:
            seen.add(s_clean)
            unique_sentences.append(s)

    if len(unique_sentences) < 15:
        return {"ok": False, "reason": f"Insufficient statements harvested ({len(unique_sentences)} < 15)"}

    # 5. Out-of-Domain Negative Probe Selection
    corpus_vocab = set(re.findall(r"[a-z]{4,}", " ".join(unique_sentences).lower()))
    probe_stopwords = {
        "what", "which", "where", "when", "that", "this", "with", "from", "have",
        "been", "their", "there", "some", "more", "does", "about", "used", "differ",
        "standard", "before", "after", "through", "between", "under", "over"
    }

    best_probe = None
    min_overlap = 999
    for candidate in ALIEN_BENCHMARK_PROBES:
        cand_tokens = set(re.findall(r"[a-z]{4,}", candidate.lower())) - probe_stopwords
        overlap = len(corpus_vocab & cand_tokens)
        if overlap < min_overlap:
            min_overlap = overlap
            best_probe = candidate
            if overlap == 0:
                break
    test_out = best_probe or ALIEN_BENCHMARK_PROBES[0]

    # Write Corpus File
    corpus_dir = Path("var/distill_isekai")
    corpus_dir.mkdir(parents=True, exist_ok=True)
    corpus_file = corpus_dir / f"{domain_key}_corpus.txt"
    with open(corpus_file, "w") as f:
        for s in unique_sentences:
            f.write(s + "\n")
    # Held-out in-domain probes for radius calibration. probes[0] is kept out of
    # calibration so Gate 2 below remains a genuine held-out check.
    probes_file = corpus_dir / f"{domain_key}_probes.txt"
    with open(probes_file, "w") as f:
        for q in probes[1:]:
            f.write(q + "\n")

    # 6. Native VSA Capsule Compilation & Sealing
    output_dir.mkdir(parents=True, exist_ok=True)
    capsule_file = output_dir / f"{domain_key}.gencap"
    # Per-capsule radius calibration: leave-one-out corpus sentences plus the
    # held-out probes are the in-domain set; sentences sampled from every other
    # corpus under var/distill are the negatives. The CLI refuses to seal when
    # no radius meets both targets (NOT_SEPARABLE) or evidence is thin.
    cmd_create = [CNET_CLI, "gencap-create", domain_key, tag_clean, str(corpus_file), str(capsule_file),
                  "--encoder", _encoder_for(output_dir),
                  "--probes", str(probes_file), "--negatives", str(corpus_dir),
                  "--target-in", CALIB_TARGET_IN, "--target-neg", CALIB_TARGET_NEG]
    res_create = subprocess.run(cmd_create, capture_output=True, text=True, env=_lexicon_env(output_dir))
    if res_create.returncode != 0:
        if "NOT_SEPARABLE" in res_create.stdout:
            m_sep = re.search(r"separation=([+-]?[0-9.]+)", res_create.stdout)
            sep = m_sep.group(1) if m_sep else "?"
            return {"ok": False, "reason": f"Calibration refused: not separable from other domains (separation={sep})"}
        if "INSUFFICIENT_EVIDENCE" in res_create.stdout:
            return {"ok": False, "reason": "Calibration refused: insufficient held-out evidence"}
        return {"ok": False, "reason": f"Compilation failed: {res_create.stderr or res_create.stdout[-300:]}"}
    if "CALIBRATED (held-out evidence)" not in res_create.stdout:
        return {"ok": False, "reason": "Capsule sealed without a calibration receipt"}
    m_radius = re.search(r"Safe Radius:\s+([0-9.]+)", res_create.stdout)
    calibrated_radius = float(m_radius.group(1)) if m_radius else None
    m_rates = re.search(r"negatives:.*measured=([0-9.]+)", res_create.stdout)
    neg_reject_rate = float(m_rates.group(1)) if m_rates else None

    digest = "unknown"
    m_digest = re.search(r"Integrity Digest:\s+(0x[0-9a-fA-F]+)", res_create.stdout)
    if m_digest:
        digest = m_digest.group(1)

    m_words = re.search(r"Vocabulary:\s+(\d+)\s+unique words", res_create.stdout)
    vocab_count = int(m_words.group(1)) if m_words else 0
    m_trans = re.search(r"Transitions:\s+(\d+)\s+n-gram transitions", res_create.stdout)
    trans_count = int(m_trans.group(1)) if m_trans else 0

    # Gate 1: Cryptographic Digest Verification
    cmd_verify = [CNET_CLI, "gencap-verify", str(capsule_file)]
    res_verify = subprocess.run(cmd_verify, capture_output=True, text=True)
    if "CERTIFIED_AUTHENTIC" not in res_verify.stdout:
        return {"ok": False, "reason": "Cryptographic digest mismatch"}

    # Gate 2: In-Domain Semantic Steerability
    cmd_in = [CNET_CLI, "gencap-gen", str(capsule_file), probes[0], "the", "25"]
    res_in = subprocess.run(cmd_in, capture_output=True, text=True)
    if "SUCCESS (In-Domain)" not in res_in.stdout:
        return {"ok": False, "reason": "In-domain verification abstained"}

    # Gate 3: Fail-Closed OOD Abstention
    cmd_ood = [CNET_CLI, "gencap-gen", str(capsule_file), test_out, "the", "25"]
    res_ood = subprocess.run(cmd_ood, capture_output=True, text=True)
    if "REFUSED (Out-of-Domain)" not in res_ood.stdout or "ABSTAIN:" not in res_ood.stdout:
        return {"ok": False, "reason": f"OOD abstention gate failed on query: '{test_out}'"}

    # Sync to bin/capsules and Unity StreamingAssets
    bin_target = CAPSULES_DIR / f"{domain_key}.gencap"
    if not bin_target.exists() or bin_target.resolve() != capsule_file.resolve():
        subprocess.run(["cp", "-f", str(capsule_file), str(bin_target)])

    if UNITY_CAPSULES_DIR.is_dir():
        dest = UNITY_CAPSULES_DIR / f"{domain_key}.gencap"
        subprocess.run(["cp", "-f", str(capsule_file), str(dest)])

    elapsed = time.time() - t0
    return {
        "ok": True,
        "domain_key": domain_key,
        "domain_tag": tag_clean,
        "capsule_file": str(capsule_file),
        "digest": digest,
        "sentences": len(unique_sentences),
        "vocab": vocab_count,
        "transitions": trans_count,
        "elapsed": elapsed,
        "sample_sentences": unique_sentences[:10]
    }


def update_dashboard(state: IsekaiCrawlerState):
    uptime = time.time() - state.start_time
    hours = int(uptime // 3600)
    minutes = int((uptime % 3600) // 60)

    total_caps = len(state.visited)
    frontier_count = len(state.frontier)
    failed_count = len(state.failed)

    lines = [
        "# CNET Isekai & RPG Autonomous Knowledge Network (Live)",
        "",
        "> **Compounding Anime, Isekai, RPG Gameplay & Emergent Storytelling Competence.**",
        "> **Anti-Collapse Guaranteed**: Trained purely from 27B external teacher model on ROCm (:8081). Zero CNET self-training.",
        "",
        f"* **Uptime**: {hours}h {minutes}m",
        f"* **Total Certified Isekai/RPG Capsules**: `{total_caps}`",
        f"* **Curiosity Frontier Backlog**: `{frontier_count}` unexplored concepts",
        f"* **Certification Fail-Closed Refusals**: `{failed_count}` rejected candidates",
        f"* **Last Updated**: `{time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())}`",
        "",
        "---",
        "",
        "## Recent Knowledge Acquisitions (Certified Capsules)",
        "",
        "| Domain Name | Tag | Digest | Statements | Vocabulary | Transitions |",
        "|---|---|---|---|---|---|"
    ]

    recent = sorted(state.visited.items(), key=lambda item: item[1].get("timestamp", 0), reverse=True)[:20]
    for key, info in recent:
        tag = info.get("tag", "-")
        digest = info.get("digest", "-")
        stmts = info.get("statements", "-")
        vocab = info.get("vocab", "-")
        trans = info.get("transitions", "-")
        lines.append(f"| `{key}` | `{tag}` | `{digest}` | {stmts} | {vocab} words | {trans} |")

    lines.extend([
        "",
        "---",
        "",
        "## Active Curiosity Frontier (Next in Queue)",
        ""
    ])

    for item in state.frontier[:15]:
        topic = item.get("topic", "")
        parent = item.get("parent", "root")
        depth = item.get("depth", 0)
        lines.append(f"* **{topic}** *(branch depth: {depth}, linked from: `{parent}`)*")

    if state.failed:
        lines.extend([
            "",
            "---",
            "",
            "## Gate Enforcement Log (Fail-Closed Refusals)",
            "",
            "| Rejected Domain | Refusal Reason | Timestamp |",
            "|---|---|---|"
        ])
        for f_item in state.failed[-8:]:
            lines.append(f"| `{f_item.get('topic')}` | {f_item.get('reason')} | {f_item.get('time')} |")

    lines.extend([
        "",
        "---",
        "",
        "## Integrity Protocol",
        "Every capsule is compiled into a high-dimensional Vector Symbolic Architecture kernel, checked with an authoritative FNV-1a checksum, and tested against OOD alien probes to prevent hallucinations or out-of-domain false positives.",
        ""
    ])

    STATUS_MD.parent.mkdir(parents=True, exist_ok=True)
    with open(STATUS_MD, "w") as f:
        f.write("\n".join(lines))


def main():
    print("=================================================================")
    print(" CNET Autonomous Isekai & RPG Knowledge Crawler (v1.0)")
    print(" Target: Anime Tropes, Storytelling, Skill Trees, Boon & Bane Duality")
    print(" Teacher: 27B Ternary Bonsai on ROCm (:8081)")
    print("=================================================================")

    state = IsekaiCrawlerState()
    state.load()
    CAPSULES_DIR.mkdir(parents=True, exist_ok=True)

    try:
        while not STOP_REQUESTED and state.frontier:
            item = state.frontier.pop(0)
            topic = item["topic"]
            parent = item.get("parent", "root")
            depth = item.get("depth", 0)
            d_key = normalize_key(topic)

            if d_key in state.visited:
                continue

            print(f"\n[+] [{len(state.visited) + 1}] Processing Domain: '{topic}' (depth: {depth})")
            res = distill_and_certify(topic, CAPSULES_DIR)

            if res and res.get("ok"):
                print(f"    ✓ CERTIFIED: tag={res['domain_tag']} digest={res['digest']} stmts={res['sentences']} vocab={res['vocab']} ({res['elapsed']:.1f}s)")
                state.visited[d_key] = {
                    "topic": topic,
                    "tag": res["domain_tag"],
                    "digest": res["digest"],
                    "statements": res["sentences"],
                    "vocab": res["vocab"],
                    "transitions": res["transitions"],
                    "timestamp": time.time(),
                    "parent": parent,
                    "depth": depth
                }

                # Discover adjacent topics and grow the frontier
                adjacent = discover_adjacent_topics(topic, res["sample_sentences"])
                for adj in adjacent:
                    adj_key = normalize_key(adj)
                    if adj_key not in state.visited and not any(f["topic"] == adj for f in state.frontier):
                        state.frontier.append({"topic": adj, "parent": d_key, "depth": depth + 1})
                        state.graph_edges.append({"source": d_key, "target": adj_key})
                        print(f"      -> Enqueued adjacent: '{adj}'")

            else:
                reason = res.get("reason", "Unknown failure") if res else "Unknown error"
                print(f"    ✗ REFUSED: {reason}")
                state.failed.append({
                    "topic": topic,
                    "reason": reason,
                    "time": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
                })

            state.save()
            update_dashboard(state)
            time.sleep(0.5)

    except KeyboardInterrupt:
        print("\n[!] Crawler interrupted by user.")
    finally:
        state.save()
        update_dashboard(state)
        print(f"\n[✓] Finished. Total certified Isekai/RPG capsules: {len(state.visited)}. State saved to {STATE_FILE}.")


if __name__ == "__main__":
    main()
