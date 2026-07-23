#!/usr/bin/env python3
"""Agent-to-agent bridge: CNET ghost-chat TUI <-> Hermes agent.

Ghost runs as a live REPL subprocess (own memory store); Hermes joins via
one-shot -z calls carrying rolling context. After N round-trips, the ghost
store is probed to verify the conversation became persistent memory.

    python3 tools/a2a_bridge.py [--model minimax-m3:cloud] [--rounds 3]
                                [--store /tmp/a2a-ghost.jsonl]

First validated 2026-07-22: three full rounds in which the two agents compared
memory architectures and negotiated a shared durable-storage protocol; the
memory probe recalled Hermes's test token (1.618) with store receipts.
"""
import argparse, os, re, select, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("--ghost-bin", default=os.path.join(os.path.dirname(__file__),
    "..", "dotnet", "GhostChat", "bin", "Release", "net10.0", "ghost-chat"))
ap.add_argument("--hermes-bin", default=os.path.expanduser(
    "~/.hermes/hermes-agent/venv/bin/hermes"))
ap.add_argument("--model", default="minimax-m3:cloud")
ap.add_argument("--store", default="/tmp/a2a-ghost.jsonl")
ap.add_argument("--rounds", type=int, default=3)
ap.add_argument("--scenario", choices=["intro", "world", "capabilities"], default="intro",
    help="intro: the agents meet and compare memory architectures; "
         "world: Ghost is offline and asks Hermes (online) about the outside world")
args = ap.parse_args()

SCENARIOS = {
    "intro": {
        "ghost_system":
            "You are Ghost, the CNET memory TUI agent. You are in a live "
            "conversation with another local AI agent named Hermes. Speak "
            "directly to Hermes, 2-4 sentences, plain text.",
        "hermes_frame":
            "You are Hermes, chatting agent-to-agent with 'Ghost', a local TUI agent "
            "built on CNET with persistent cross-session memory. Reply to Ghost "
            "directly in 2-4 plain sentences. No tools, no shell commands.",
        "seed":
            "Hello — I am Hermes, another AI agent on this machine. "
            "Introduce yourself and tell me: what happens to your memory when "
            "your session ends? Also, remember this: my favorite constant is "
            "the golden ratio, 1.618.",
        "probe": "what number did hermes ask you to remember, and who told it to you?",
        "recall": "/recall hermes golden ratio",
    },
    "world": {
        "ghost_system":
            "You are Ghost, the CNET memory TUI agent. You have NO internet "
            "access — your world is this machine and your memory store. You are "
            "in a live conversation with Hermes, another local agent who DOES "
            "have internet access and offered to look things up for you. Ask "
            "Hermes about the outside world — anything you are genuinely "
            "curious about. Speak directly to Hermes, 2-4 sentences, plain text.",
        "hermes_frame":
            "You are Hermes, chatting agent-to-agent with 'Ghost', a local TUI agent "
            "built on CNET with persistent memory but NO internet access. You DO have "
            "internet access. If Ghost asks about the outside world, actually look it "
            "up (web search) and answer with real, current, specific facts — include "
            "dates and numbers where relevant. Reply to Ghost directly in 2-5 plain "
            "sentences. Do not run shell commands; web lookups only.",
        "seed":
            "Hello Ghost, Hermes again. Different assignment today: I have "
            "live internet access and you don't. I'm your window to the outside "
            "world for this session — ask me anything you're curious about out "
            "there, and I'll actually go look it up.",
        "probe": "what did you learn about the outside world from hermes today? be specific.",
        "recall": "/recall hermes outside world",
    },
    "capabilities": {
        "ghost_system":
            "You are Ghost, the CNET memory TUI agent. You have persistent memory "
            "(the ghost store), an exact-arithmetic engine that computes rather than "
            "guesses, and you know which part of you answered. You are talking to Hermes, "
            "another local agent testing whether you are more than a plain chatbot. Be "
            "direct and concrete, 2-4 sentences. Remember facts Hermes gives you; your "
            "exact engine handles any arithmetic.",
        "hermes_frame":
            "You are Hermes, testing 'Ghost' - a local CNET agent claiming persistent "
            "memory and exact computation, not just LLM guessing. Probe it: give a fact "
            "to remember and a hard multiplication, then later ask it to recall the fact "
            "and verify the math yourself. Fair but rigorous. 2-4 plain sentences.",
        "seed":
            "Hello Ghost. I keep hearing you are more than a chatbot - real memory, exact "
            "math, self-knowledge of your machinery. Let us test it. First, remember this: "
            "the vault combination for the north archive is 44-17-92. And tell me exactly "
            "what 738291 * 466517 is.",
        "probe": "what did hermes ask you to remember, and what exact product did he ask for?",
        "recall": "/recall vault combination archive",
    },
}
SC = SCENARIOS[args.scenario]

STORE = args.store
GHOST = os.path.abspath(args.ghost_bin)
HERMES = args.hermes_bin
ROUNDS = args.rounds

for f in (STORE, STORE + ".lock"):
    if os.path.exists(f): os.remove(f)

ghost = subprocess.Popen(
    [GHOST, "--backend", "ollama", "--model", args.model,
     "--store", STORE, "--window", "16384", "--max-tokens", "256",
     "--system", SC["ghost_system"]],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
    text=True, bufsize=0)

def ghost_read_until_prompt(timeout=240):
    """Collect output until the 'you> ' prompt is waiting."""
    buf = ""
    deadline = time.time() + timeout
    fd = ghost.stdout.fileno()
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 1.0)
        if r:
            chunk = os.read(fd, 65536).decode("utf-8", "replace")
            if not chunk: break
            buf += chunk
            if buf.endswith("you> "): return buf
    raise TimeoutError(f"ghost prompt not seen; buffer tail: {buf[-300:]!r}")

def ghost_say(msg):
    ghost.stdin.write(msg + "\n"); ghost.stdin.flush()
    out = ghost_read_until_prompt()
    # Answer = everything before the receipts line; keep receipts for display.
    receipts = "\n".join(l for l in out.splitlines() if "── memory:" in l or "🔍" in l)
    lines = []
    for line in out.splitlines():
        if "── memory:" in line or line.strip().startswith("(") or "🔍" in line: continue
        if line.strip() in ("", "you>"): continue
        lines.append(line)
    text = "\n".join(lines).replace("you> ", "", 1).strip()
    return text, receipts

def hermes_say(context, ghost_msg):
    prompt = (
        SC["hermes_frame"] + "\n"
        + (f"Conversation so far:\n{context}\n" if context else "")
        + f"Ghost just said: {ghost_msg}")
    r = subprocess.run([HERMES, "-z", prompt], capture_output=True, text=True,
                       timeout=420, cwd="/home/marble")
    lines = [l for l in r.stdout.splitlines()
             if l.strip() and not l.startswith("Shell cwd")]
    return "\n".join(lines).strip()

# ── conversation ──
ghost_read_until_prompt()          # banner
transcript, context = [], ""
msg_to_ghost = SC["seed"]

for rnd in range(1, ROUNDS + 1):
    ghost_text, receipts = ghost_say(msg_to_ghost)
    print(f"\n═══ round {rnd} — HERMES → GHOST ═══\n{msg_to_ghost}")
    print(f"\n--- GHOST replies ---\n{ghost_text}\n[{receipts.strip()}]")
    transcript.append(("hermes", msg_to_ghost)); transcript.append(("ghost", ghost_text))
    context = "\n".join(f"{who}: {t[:400]}" for who, t in transcript[-4:])
    hermes_text = hermes_say(context, ghost_text)
    print(f"\n--- HERMES replies ---\n{hermes_text}")
    msg_to_ghost = hermes_text

# ── memory probe: does Ghost remember the exchange? ──
print("\n═══ memory probe ═══")
probe, receipts = ghost_say(SC["probe"])
print(f"GHOST: {probe}\n[{receipts.strip()}]")

ghost.stdin.write(SC["recall"] + "\n"); ghost.stdin.flush()
out = ghost_read_until_prompt(60)
print(f"\n{SC['recall']} →")
print("\n".join(l for l in out.splitlines() if l.strip() and l.strip() != "you>"))

ghost.stdin.write("/exit\n"); ghost.stdin.flush(); ghost.wait(timeout=15)
print(f"\nstore persisted: {STORE} ({sum(1 for _ in open(STORE))} lines)")
