#!/usr/bin/env python3
"""
test_discord_cognitive_suite.py — End-to-End Integration Suite for CNET Cognitive Discord Capabilities
Tests:
  1. Live Interactive Capsule Learning (VSA Store, instant zero-forgetting recall).
  2. Real-Time VSA Grounding & Anti-Hallucination Audit on warm Neural Mouth (:8084).
  3. Sandboxed Tool Execution (!eval, !rocm, !cnet-status, !vsa-sim, security refusal).
  4. Gateway Message Routing (Certified skills vs tools vs capsules vs chat).
"""
import json
import sys
import time
import urllib.request
from pathlib import Path

# Add paths
sys.path.insert(0, "/home/marble/AI/CNET/tools")
sys.path.insert(0, "/home/marble/.local/share/cnet-learning-ingress-20260908-6GXSak/release")

from cnet_vsa_live_capsules import CAPSULE_STORE
import cnet_discord_tools as tools
import gateway


def run_suite():
    print("=" * 65)
    print(" CNET Cognitive Discord Integration & Verification Suite")
    print("=" * 65)
    passed = 0
    total = 0

    def check(name: str, cond: bool, detail: str = ""):
        nonlocal passed, total
        total += 1
        if cond:
            passed += 1
            print(f"  [PASS] {name} {detail}")
        else:
            print(f"  [FAIL] {name} {detail}")
            raise AssertionError(f"Check failed: {name}")

    # -------------------------------------------------------------
    # Test 1: Live Interactive Capsule Learning
    # -------------------------------------------------------------
    print("\n[1/4] Testing Live Interactive Capsule Learning...")
    CAPSULE_STORE.clear()

    res = CAPSULE_STORE.learn("The orbital velocity of the station is 7.66 km/s", author="flight_dir", channel_id="ch_1")
    check("Capsule learned", res.get("status") == "ok" and res.get("action") == "learned")
    check("Capsule ID assigned", bool(res.get("capsule_id", "").startswith("cap_")))

    # Verify retrieval
    matches = CAPSULE_STORE.query("what is the orbital velocity of the station?")
    check("Query retrieves capsule", len(matches) > 0)
    check("Query content match", "7.66 km/s" in matches[0]["statement"])

    # Duplicate check
    res_dup = CAPSULE_STORE.learn("The orbital velocity of the station is 7.66 km/s")
    check("Duplicate refusal/idempotence", res_dup.get("action") == "already_known")

    # -------------------------------------------------------------
    # Test 2: Real-Time VSA Grounding & Anti-Hallucination Audit
    # -------------------------------------------------------------
    print("\n[2/4] Testing Real-Time VSA Grounding on Warm Mouth (:8084)...")
    chat_payload = {
        "messages": [
            {"role": "user", "content": "user_pilot: What is the orbital velocity of the station?"}
        ],
        "max_tokens": 100
    }
    req = urllib.request.Request(
        "http://127.0.0.1:8084/chat",
        data=json.dumps(chat_payload).encode(),
        headers={"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        chat_res = json.loads(resp.read().decode())

    reply = chat_res.get("reply", "")
    print(f"  Mouth Reply: {reply[:120]}...")
    check("Grounding active", chat_res.get("grounded") in {"grounded_verified", "overridden_safe"})
    check("Factual fidelity (7.66 km/s present)", "7.66" in reply)
    check("Certification badge attached", "🛡️" in reply)

    # Test Anti-Hallucination Audit Unit
    audit_truth = CAPSULE_STORE.audit_anti_hallucination(
        "orbital velocity of the station",
        "The station travels at approximately 7.66 km/s in low Earth orbit."
    )
    check("Truthful audit passes", audit_truth["grounded"] is True and audit_truth["verdict"] == "CERTIFIED_GROUNDED")

    audit_fake = CAPSULE_STORE.audit_anti_hallucination(
        "orbital velocity of the station",
        "The station moves slowly at 120 meters per second."
    )
    check("Hallucination caught", audit_fake["grounded"] is False and audit_fake["verdict"] == "GROUNDING_GAP_OR_CONTRADICTION")

    # -------------------------------------------------------------
    # Test 3: Certified Tool Execution Sandbox
    # -------------------------------------------------------------
    print("\n[3/4] Testing Certified Tool Execution Sandbox...")
    # Safe math evaluation
    e1 = tools.execute_safe_eval("!eval 25 * 4 + 137")
    check("Safe math evaluated correctly", "237" in e1)

    e2 = tools.execute_safe_eval("!eval sqrt(6561) / 9")
    check("Math functions (sqrt) work", "9" in e2)

    # Security: code injection refusal
    e_bad = tools.execute_safe_eval("!eval __import__('os').system('ls')")
    check("Code injection refused safely", "Refused" in e_bad or "Disallowed" in e_bad)

    # ROCm GPU Monitor
    rocm_out = tools.execute_rocm_status()
    check("ROCm monitor returns output", "AMD Radeon" in rocm_out or "Hardware" in rocm_out)

    # CNET Services Status
    cnet_stat = tools.execute_cnet_status()
    check("CNET services active", "cnet-vsa-mouth-8084" in cnet_stat and "RUNNING" in cnet_stat)

    # VSA Cosine Similarity Tool
    vsa_out = tools.execute_vsa_sim("Oliver the fox vs curious fox in the woods")
    check("VSA similarity computed", "Cosine Similarity" in vsa_out)

    # -------------------------------------------------------------
    # Test 4: Gateway Message Routing & Non-Interference
    # -------------------------------------------------------------
    print("\n[4/4] Testing Gateway Routing & Certified Skills Invariant...")
    # Native CNET certified skill
    ans_cert, status_cert = gateway.peer_ask("test_user", "what can you do")
    check("Native certified skill routed cleanly", status_cert == "peer_ok" and "raw LUT bricks" in ans_cert)

    # Conversational reply routing
    conv_reply = gateway.conversational_reply("test_ch", "test_user", "Hello Marble, how are you?")
    check("Conversational reply generated", len(conv_reply) > 10 and not conv_reply.startswith("Not sealed"))

    # Help command
    help_text = tools.execute_help()
    check("Help command documentation present", "Marble — CNET Cognitive Discord Companion" in help_text)

    print("\n" + "=" * 65)
    print(f" ALL {total}/{total} COGNITIVE INTEGRATION CHECKS PASSED.")
    print("=" * 65)


if __name__ == "__main__":
    run_suite()
