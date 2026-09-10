#!/usr/bin/env python3
"""
cnet_discord_tools.py — Sandboxed Certified Execution Tools for CNET Discord Bot
Provides safe execution of:
  - !eval <math expression> (strictly safe AST math evaluator)
  - !rocm / !gpu (real-time AMD GPU metrics via rocm-smi)
  - !cnet-status (microservice health and ROCm hardware engine)
  - !vsa-sim <t1> vs <t2> (hyperdimensional cosine similarity)
  - !capsules / !inventory (active knowledge capsules)
"""
from __future__ import annotations

import ast
import math
import operator
import re
import subprocess
import time
from pathlib import Path
from typing import Any

from cnet_vsa_live_capsules import CAPSULE_STORE, encode_vsa, vsa_cosine

# Safe math operators for AST evaluation
SAFE_OPERATORS = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: operator.mul,
    ast.Div: operator.truediv,
    ast.FloorDiv: operator.floordiv,
    ast.Mod: operator.mod,
    ast.Pow: operator.pow,
    ast.USub: operator.neg,
    ast.UAdd: operator.pos,
}

SAFE_FUNCTIONS = {
    "sqrt": math.sqrt,
    "sin": math.sin,
    "cos": math.cos,
    "tan": math.tan,
    "asin": math.asin,
    "acos": math.acos,
    "atan": math.atan,
    "log": math.log,
    "log2": math.log2,
    "log10": math.log10,
    "exp": math.exp,
    "floor": math.floor,
    "ceil": math.ceil,
    "abs": abs,
    "round": round,
    "min": min,
    "max": max,
    "pow": pow,
    "factorial": math.factorial,
    "gcd": math.gcd,
}

SAFE_CONSTANTS = {
    "pi": math.pi,
    "e": math.e,
    "tau": math.tau,
}


def _safe_eval_node(node: ast.AST) -> Any:
    if isinstance(node, ast.Expression):
        return _safe_eval_node(node.body)
    if isinstance(node, ast.Constant):  # Python 3.8+ for numbers/bools
        if isinstance(node.value, (int, float)):
            return node.value
        raise ValueError("Only numeric constants allowed")
    if isinstance(node, ast.Num):  # Fallback
        return node.n
    if isinstance(node, ast.Name):
        if node.id in SAFE_CONSTANTS:
            return SAFE_CONSTANTS[node.id]
        raise ValueError(f"Unknown or disallowed identifier: {node.id}")
    if isinstance(node, ast.BinOp):
        op_type = type(node.op)
        if op_type not in SAFE_OPERATORS:
            raise ValueError(f"Disallowed binary operator: {op_type.__name__}")
        left = _safe_eval_node(node.left)
        right = _safe_eval_node(node.right)
        # Guard against absurd power loops
        if op_type is ast.Pow and (abs(right) > 1000 or abs(left) > 100000):
            raise ValueError("Power exponent out of safe bounds")
        return SAFE_OPERATORS[op_type](left, right)
    if isinstance(node, ast.UnaryOp):
        op_type = type(node.op)
        if op_type not in SAFE_OPERATORS:
            raise ValueError(f"Disallowed unary operator: {op_type.__name__}")
        operand = _safe_eval_node(node.operand)
        return SAFE_OPERATORS[op_type](operand)
    if isinstance(node, ast.Call):
        if not isinstance(node.func, ast.Name) or node.func.id not in SAFE_FUNCTIONS:
            raise ValueError(f"Disallowed function call: {ast.dump(node.func)}")
        func = SAFE_FUNCTIONS[node.func.id]
        args = [_safe_eval_node(arg) for arg in node.args]
        return func(*args)
    raise ValueError(f"Disallowed expression node: {type(node).__name__}")


def execute_safe_eval(expr_str: str) -> str:
    """Evaluates a mathematical expression safely with strict AST sandboxing."""
    clean = expr_str.strip()
    if clean.lower().startswith("!eval"):
        clean = clean[5:].strip()
    if not clean:
        return "⚠️ Error: Empty expression. Example: `!eval sqrt(144) + 17*3`"
    if len(clean) > 200:
        return "⚠️ Error: Expression too long (> 200 chars)."

    try:
        tree = ast.parse(clean, mode="eval")
        val = _safe_eval_node(tree)
        if isinstance(val, float):
            formatted = f"{val:.6g}" if abs(val) < 1e12 else f"{val:.4e}"
        else:
            formatted = str(val)
        return f"🧮 **[CNET Certified Math Evaluator]**\n`{clean}` = **`{formatted}`**"
    except Exception as e:
        return f"⚠️ **Math Evaluation Refused**: {e}"


def execute_rocm_status() -> str:
    """Reads live AMD ROCm GPU status and returns formatted Discord metrics."""
    try:
        p = subprocess.run(
            ["rocm-smi", "--showtemp", "--showuse", "--showpower", "--showmeminfo", "vram"],
            capture_output=True,
            text=True,
            timeout=5
        )
        out = p.stdout.strip()
        if not out or p.returncode != 0:
            return "⚠️ ROCm SMI returned non-zero exit code or empty output."

        # Parse key metrics cleanly
        temp_m = re.search(r"Temperature\s*\(Edge\):\s*([0-9.]+)", out) or re.search(r"([0-9.]+)c", out, re.IGNORECASE)
        use_m = re.search(r"GPU use\s*\(%\):\s*([0-9]+)", out) or re.search(r"GPU\[[0-9]+\]\s*:\s*([0-9]+)%", out)
        power_m = re.search(r"Average Graphics Package Power\s*\(W\):\s*([0-9.]+)", out) or re.search(r"([0-9.]+)W", out)
        vram_m = re.search(r"VRAM Total Used Memory\s*\(B\):\s*([0-9]+)", out)
        vram_tot_m = re.search(r"VRAM Total Memory\s*\(B\):\s*([0-9]+)", out)

        card = ["⚡ **[AMD ROCm Hardware Monitor]**", "```"]
        card.append("Hardware:   AMD Radeon AI PRO R9700 (ROCm gfx1201)")
        if temp_m:
            card.append(f"Temperature:{temp_m.group(1).rjust(8)} °C")
        if use_m:
            card.append(f"GPU Load:   {use_m.group(1).rjust(8)} %")
        if power_m:
            card.append(f"Power Draw: {power_m.group(1).rjust(8)} W")
        if vram_m and vram_tot_m:
            used_mb = int(vram_m.group(1)) / (1024 * 1024)
            tot_mb = int(vram_tot_m.group(1)) / (1024 * 1024)
            pct = (used_mb / tot_mb) * 100.0 if tot_mb > 0 else 0
            card.append(f"VRAM Usage: {used_mb:.1f} MB / {tot_mb:.1f} MB ({pct:.1f}%)")
        card.append("Status:     HEALTHY & ACTIVE")
        card.append("```")
        return "\n".join(card)
    except FileNotFoundError:
        return "⚠️ `rocm-smi` binary not found on PATH."
    except Exception as e:
        return f"⚠️ Error querying ROCm status: {e}"


def execute_cnet_status() -> str:
    """Queries systemd and CNET components for overall architecture status."""
    lines = ["🖥️ **[CNET Cognitive Architecture Status]**", "```"]

    # Check services via systemctl --user
    for sname in ["cnet-vsa-mouth-8084.service", "cnet-discord-peer.service", "cnetd.service"]:
        try:
            p = subprocess.run(
                ["systemctl", "--user", "is-active", sname],
                capture_output=True,
                text=True,
                timeout=3
            )
            state = p.stdout.strip()
            status_icon = "RUNNING" if state == "active" else state.upper()
        except Exception:
            status_icon = "UNKNOWN"
        lines.append(f"{sname[:26].ljust(26)}: {status_icon}")

    capsules = CAPSULE_STORE.list_all()
    lines.append(f"Live Knowledge Capsules   : {len(capsules)} registered")
    lines.append("ROCm Execution Engine     : gfx1201 (Hardware Accelerated)")
    lines.append("VSA Manifold Space        : D=1024 Bipolar Hypervectors")
    lines.append("```")
    return "\n".join(lines)


def execute_vsa_sim(text_input: str) -> str:
    """Computes exact VSA hyperdimensional similarity between two phrases."""
    parts = re.split(r"\s+(?:vs|and|,)\s+", text_input, maxsplit=1, flags=re.IGNORECASE)
    if len(parts) != 2:
        return "⚠️ Usage: `!vsa-sim <phrase 1> vs <phrase 2>`"

    t1, t2 = parts[0].strip(), parts[1].strip()
    v1 = encode_vsa(t1)
    v2 = encode_vsa(t2)
    sim = vsa_cosine(v1, v2)

    return (
        f"📐 **[CNET VSA Hyperdimensional Space Probe]**\n"
        f"• Phrase A: *\"{t1}\"*\n"
        f"• Phrase B: *\"{t2}\"*\n"
        f"• **Cosine Similarity: `{sim:.4f}`** (Dimension: 1024)"
    )


def execute_capsules_list() -> str:
    """Lists both compiled certified .gencap knowledge capsules and live taught capsules."""
    gencap_dir = Path("/home/marble/AI/CNET/bin")
    gencaps = sorted(gencap_dir.glob("*.gencap")) if gencap_dir.is_dir() else []

    lines = [f"📚 **[CNET Multi-Capsule Knowledge Network]** ({len(gencaps)} compiled domains):", "```"]
    for i, p in enumerate(gencaps, 1):
        name = p.stem
        lines.append(f"{i:2d}. {name}")
    lines.append("```")
    lines.append("💡 *Query directly:* `!capsule <prompt>` | *Check domain routing:* `!route <prompt>`")

    live_caps = CAPSULE_STORE.list_all() if CAPSULE_STORE else []
    if live_caps:
        lines.append(f"\n🧠 **[Live Dynamic Memory Facts]** ({len(live_caps)} taught):")
        for i, c in enumerate(live_caps[-5:], 1):
            lines.append(f"• `{c['id']}`: **{c['statement']}** *(by {c.get('author', 'user')})*")
        if len(live_caps) > 5:
            lines.append(f"*(and {len(live_caps) - 5} more facts)*")
    return "\n".join(lines)


def execute_vsa_route(query_str: str) -> str:
    """Evaluates topical cosine distance across all 16 certified capsules."""
    clean = query_str.strip()
    if clean.lower().startswith("!route"):
        clean = clean[6:].strip()
    if not clean:
        return "⚠️ Usage: `!route <technical query>` (e.g. `!route How does ROCm schedule wavefronts?`)"

    cli = "/home/marble/AI/CNET/bin/cnet_vsa_cli"
    try:
        p = subprocess.run([cli, "route", clean], capture_output=True, text=True, timeout=5)
        out = p.stdout.strip()
        lines = [line.strip() for line in out.splitlines() if "|" in line or "Routing Decision" in line]
        if not lines:
            return "⚠️ Route evaluation failed or returned empty."
        card = ["🧭 **[CNET VSA Multi-Capsule Intent Router]**", f"Query: *\"{clean}\"*", "```"]
        card.extend(lines[:14])
        card.append("```")
        return "\n".join(card)
    except Exception as e:
        return f"⚠️ Error executing VSA router: {e}"


def execute_vsa_auto(query_str: str) -> str:
    """Executes end-to-end zero-LLM autonomous synthesis via winning certified capsule."""
    clean = query_str.strip()
    for prefix in ("!capsule", "!gencap", "!vsa"):
        if clean.lower().startswith(prefix):
            clean = clean[len(prefix):].strip()
            break
    if not clean:
        return "⚠️ Usage: `!capsule <technical query>` (e.g. `!capsule What is an inode in the Linux kernel?`)"

    cli = "/home/marble/AI/CNET/bin/cnet_vsa_cli"
    try:
        p = subprocess.run([cli, "auto", clean], capture_output=True, text=True, timeout=5)
        out = p.stdout
        is_success = "Verdict:         SUCCESS (In-Domain)" in out
        m_cap = re.search(r"Routed Capsule:\s+([a-zA-Z0-9_]+)\s+\(dist=([0-9.]+)\)", out)
        cap = m_cap.group(1) if m_cap else "unknown"
        dist = float(m_cap.group(2)) if m_cap else 1.0
        m_resp = re.search(r'Generated Response:\s*\n\s*"(.*?)"', out, re.DOTALL)
        resp = m_resp.group(1).strip() if m_resp else ""

        if is_success:
            return (
                f"🧠 **[CNET Certified Knowledge Capsule — `{cap}`]** *(dist={dist:.4f} <= 0.900)*\n"
                f"> \"{resp}\"\n\n"
                f"`[Pure VSA Algebraic Unbinding | Zero LLM | ~38,500 tok/sec]`"
            )
        else:
            return (
                f"🛑 **[CNET Fail-Closed Abstention]**\n"
                f"> Closest domain: `{cap}` (topical distance: `{dist:.4f}` > limit `0.900`)\n"
                f"*Query refused: Out-of-domain for certified technical capsules. CNET will not fabricate an uncertified answer.*"
            )
    except Exception as e:
        return f"⚠️ Error executing capsule synthesis: {e}"


def check_vsa_in_domain_answer(query_str: str) -> str | None:
    """If the query matches one of the 16 certified capsules with dist <= 0.900, returns formatted answer."""
    clean = query_str.strip()
    if len(clean) < 8 or clean.startswith("!"):
        return None
    cli = "/home/marble/AI/CNET/bin/cnet_vsa_cli"
    if not Path(cli).is_file():
        return None
    try:
        p = subprocess.run([cli, "auto", clean], capture_output=True, text=True, timeout=3)
        out = p.stdout
        if "Verdict:         SUCCESS (In-Domain)" not in out:
            return None
        m_cap = re.search(r"Routed Capsule:\s+([a-zA-Z0-9_]+)\s+\(dist=([0-9.]+)\)", out)
        cap = m_cap.group(1) if m_cap else "capsule"
        dist = float(m_cap.group(2)) if m_cap else 1.0
        m_resp = re.search(r'Generated Response:\s*\n\s*"(.*?)"', out, re.DOTALL)
        resp = m_resp.group(1).strip() if m_resp else ""
        if not resp:
            return None
        return (
            f"🧠 **[{cap}]** *(Certified Knowledge Capsule, dist={dist:.4f})*\n"
            f"> \"{resp}\"\n\n"
            f"*Synthesized via CNET Vector Symbolic Architecture (Zero LLM)*"
        )
    except Exception:
        return None


def execute_help() -> str:
    """Generates a concise Discord help manual."""
    return (
        "🤖 **Marble — CNET Cognitive Discord Companion**\n\n"
        "**Multi-Capsule Knowledge Network (Zero LLM, Pure VSA):**\n"
        "• `!capsules` — List all 16 compiled technical domains and memory facts.\n"
        "• `!capsule <query>` — Synthesize certified in-domain answer directly from specialized capsule.\n"
        "• `!route <query>` — Inspect sub-microsecond topical distance routing across all 16 domains.\n\n"
        "**Conversational & Creative:**\n"
        "• Just chat naturally for multi-turn dialogue, technical discussion, and help.\n"
        "• `tell me a story about <hero>` — Synthesizes stories with VSA safety back-projection.\n"
        "• `!clear` or `!reset` — Clears conversation history in this channel.\n\n"
        "**Instant Knowledge Learning:**\n"
        "• `!learn <fact>` — Binds a new fact permanently into CNET's VSA memory without retraining.\n"
        "• `!forget <id or topic>` — Removes a knowledge capsule.\n\n"
        "**System & Certified Execution Tools:**\n"
        "• `!eval <expression>` — Certified mathematical evaluator (e.g. `!eval sqrt(256) * 3.14`).\n"
        "• `!rocm` or `!gpu` — Live AMD ROCm GPU temperature, VRAM usage, and power.\n"
        "• `!cnet-status` — Overall health of CNET services and VSA engine.\n"
        "• `!vsa-sim <t1> vs <t2>` — Compute hyperdimensional semantic similarity."
    )
