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
args = ap.parse_args()

STORE = args.store
GHOST = os.path.abspath(args.ghost_bin)
HERMES = args.hermes_bin
ROUNDS = args.rounds

for f in (STORE, STORE + ".lock"):
    if os.path.exists(f): os.remove(f)

ghost = subprocess.Popen(
    [GHOST, "--backend", "ollama", "--model", args.model,
     "--store", STORE, "--window", "16384", "--max-tokens", "256",
     "--system", "You are Ghost, the CNET memory TUI agent. You are in a live "
                 "conversation with another local AI agent named Hermes. Speak "
                 "directly to Hermes, 2-4 sentences, plain text."],
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
        "You are Hermes, chatting agent-to-agent with 'Ghost', a local TUI agent "
        "built on CNET with persistent cross-session memory. Reply to Ghost "
        "directly in 2-4 plain sentences. No tools, no shell commands.\n"
        + (f"Conversation so far:\n{context}\n" if context else "")
        + f"Ghost just said: {ghost_msg}")
    r = subprocess.run([HERMES, "-z", prompt], capture_output=True, text=True,
                       timeout=240, cwd="/home/marble")
    lines = [l for l in r.stdout.splitlines()
             if l.strip() and not l.startswith("Shell cwd")]
    return "\n".join(lines).strip()

# ── conversation ──
ghost_read_until_prompt()          # banner
transcript, context = [], ""
msg_to_ghost = ("Hello — I am Hermes, another AI agent on this machine. "
                "Introduce yourself and tell me: what happens to your memory when "
                "your session ends? Also, remember this: my favorite constant is "
                "the golden ratio, 1.618.")

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
probe, receipts = ghost_say("what number did hermes ask you to remember, and who told it to you?")
print(f"GHOST: {probe}\n[{receipts.strip()}]")

ghost.stdin.write("/recall hermes golden ratio\n"); ghost.stdin.flush()
out = ghost_read_until_prompt(60)
print("\n/recall hermes golden ratio →")
print("\n".join(l for l in out.splitlines() if l.strip() and l.strip() != "you>"))

ghost.stdin.write("/exit\n"); ghost.stdin.flush(); ghost.wait(timeout=15)
print(f"\nstore persisted: {STORE} ({sum(1 for _ in open(STORE))} lines)")
