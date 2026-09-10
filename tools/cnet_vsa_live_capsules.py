#!/usr/bin/env python3
"""
cnet_vsa_live_capsules.py — CNET Vector Symbolic Architecture Live Capsule Store
Provides:
  1. Zero-retraining instant knowledge capsule accumulation from conversation.
  2. Positional N-Gram VSA hypervector encoding and cosine similarity search.
  3. Real-time fact grounding and anti-hallucination verification audit.
  4. Safe persistence with atomic temp-file replacement.
"""
from __future__ import annotations

import hashlib
import json
import math
import os
import re
import time
from pathlib import Path
from typing import Any

STORE_PATH = Path(__file__).resolve().parent.parent / "data" / "vsa_live_capsules.json"
VSA_DIM = 1024


def token_vector(token: str, dim: int = VSA_DIM) -> list[float]:
    """Generates a deterministic pseudo-random bipolar vector {-1.0, +1.0} for a token."""
    h = hashlib.sha256(token.lower().strip().encode("utf-8")).digest()
    seed = int.from_bytes(h[:8], "little")
    state = seed & 0xFFFFFFFFFFFFFFFF
    vec = []
    for _ in range(dim):
        state = (state * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        vec.append(1.0 if (state >> 32) & 1 else -1.0)
    return vec


def permute(vec: list[float], shift: int) -> list[float]:
    """Circular permutation (binding position k in VSA)."""
    if not vec:
        return vec
    shift = shift % len(vec)
    return vec[shift:] + vec[:shift]


def encode_vsa(text: str, dim: int = VSA_DIM) -> list[float]:
    """Encodes arbitrary text into a normalized positional VSA hypervector."""
    tokens = re.findall(r"[A-Za-z0-9_]+", text.lower())
    if not tokens:
        return [0.0] * dim
    acc = [0.0] * dim
    for pos, tok in enumerate(tokens):
        v = token_vector(tok, dim)
        pv = permute(v, pos)
        for i in range(dim):
            acc[i] += pv[i]
    norm = math.sqrt(sum(x * x for x in acc))
    if norm > 1e-9:
        acc = [x / norm for x in acc]
    return acc


def vsa_cosine(v1: list[float], v2: list[float]) -> float:
    """Computes cosine similarity between two unit VSA hypervectors."""
    if not v1 or not v2 or len(v1) != len(v2):
        return 0.0
    return sum(a * b for a, b in zip(v1, v2))


def extract_triplet(statement: str) -> tuple[str, str, str]:
    """Extracts approximate (subject, predicate, object) from statement."""
    clean = re.sub(r"^(fact:\s*|rule:\s*|note:\s*)", "", statement, flags=re.IGNORECASE).strip()
    # Check for common relational verbs
    m = re.search(r"\b(is named|is called|is located in|is the capital of|is a|is an|is|has|loves|likes|prefers|equals|=)\b", clean, re.IGNORECASE)
    if m:
        pred = m.group(1).strip()
        subj = clean[:m.start()].strip()
        obj = clean[m.end():].strip()
        return subj, pred, obj
    return clean, "is", clean


class LiveCapsuleStore:
    def __init__(self, store_path: Path = STORE_PATH):
        self.path = store_path
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._last_mtime = 0.0
        self._load()

    def _check_reload(self):
        try:
            if self.path.is_file():
                mtime = self.path.stat().st_mtime
                if mtime > self._last_mtime:
                    self._load()
        except OSError:
            pass

    def _load(self):
        if self.path.is_file():
            try:
                self._last_mtime = self.path.stat().st_mtime
                data = json.loads(self.path.read_text(encoding="utf-8"))
                self.capsules: list[dict[str, Any]] = data.get("capsules", [])
                return
            except Exception:
                pass
        self.capsules = []

    def _save(self):
        tmp_path = self.path.with_suffix(".tmp")
        payload = {"schema": 1, "updated_at": time.time(), "capsules": self.capsules}
        tmp_path.write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")
        tmp_path.replace(self.path)
        try:
            self._last_mtime = self.path.stat().st_mtime
        except OSError:
            pass

    def learn(self, text: str, author: str = "user", channel_id: str = "") -> dict[str, Any]:
        """Binds and saves a new fact statement into a certified VSA knowledge capsule."""
        self._check_reload()
        clean = text.strip()
        for prefix in ["!learn", "!remember", "remember that", "learn that", "remember:", "learn:"]:
            if clean.lower().startswith(prefix):
                clean = clean[len(prefix):].strip()
                break

        if not clean or len(clean) < 3:
            return {"status": "error", "error": "Fact is too short."}
        if len(clean) > 800:
            return {"status": "error", "error": "Fact exceeds maximum length (800 chars)."}

        subj, pred, obj = extract_triplet(clean)
        capsule_id = "cap_" + hashlib.sha256(clean.encode("utf-8")).hexdigest()[:12]

        # Avoid redundant duplicates
        for c in self.capsules:
            if c.get("id") == capsule_id or c.get("statement", "").lower() == clean.lower():
                return {
                    "status": "ok",
                    "action": "already_known",
                    "capsule_id": c["id"],
                    "statement": c["statement"],
                    "message": f"Capsule `{c['id']}` already active in CNET knowledge base."
                }

        vsa_vec = encode_vsa(clean)
        entry = {
            "id": capsule_id,
            "statement": clean,
            "subject": subj,
            "predicate": pred,
            "object": obj,
            "author": author,
            "channel_id": channel_id,
            "created_at": time.time(),
            "certified": True,
            "vsa_vector": vsa_vec
        }
        self.capsules.append(entry)
        self._save()

        return {
            "status": "ok",
            "action": "learned",
            "capsule_id": capsule_id,
            "subject": subj,
            "statement": clean,
            "message": f"✅ Learned & sealed capsule `{capsule_id}` into VSA memory:\n> **{clean}**"
        }

    def query(self, query_text: str, top_k: int = 3, min_sim: float = 0.20) -> list[dict[str, Any]]:
        """Queries stored capsules using VSA cosine similarity + symbolic token overlap."""
        self._check_reload()
        if not self.capsules:
            return []

        q_vec = encode_vsa(query_text)
        q_tokens = set(re.findall(r"[A-Za-z0-9_]+", query_text.lower()))
        stop_words = {"what", "is", "the", "a", "an", "does", "do", "of", "in", "to", "for", "and", "or", "who", "which", "where", "how", "why", "me", "my", "tell"}
        q_meaningful = q_tokens - stop_words

        scored = []
        for c in self.capsules:
            sim = vsa_cosine(q_vec, c.get("vsa_vector", []))
            s_tokens = set(re.findall(r"[A-Za-z0-9_]+", (c.get("statement", "") + " " + c.get("subject", "")).lower()))
            overlap = len(q_meaningful & s_tokens)
            score = sim + (0.35 * overlap)

            if score >= min_sim or overlap >= 1:
                scored.append((score, c))

        scored.sort(key=lambda x: x[0], reverse=True)
        results = []
        for s, c in scored[:top_k]:
            item = dict(c)
            item["match_score"] = round(s, 4)
            # Remove raw vector from returned summary to save bandwidth
            item.pop("vsa_vector", None)
            results.append(item)
        return results

    def list_all(self) -> list[dict[str, Any]]:
        """Lists all capsules without raw vectors."""
        self._check_reload()
        res = []
        for c in self.capsules:
            item = dict(c)
            item.pop("vsa_vector", None)
            res.append(item)
        return res

    def forget(self, identifier: str) -> bool:
        """Removes a capsule by ID, subject, or exact statement."""
        ident = identifier.strip().lower()
        initial_len = len(self.capsules)
        self.capsules = [
            c for c in self.capsules
            if c.get("id", "").lower() != ident
            and c.get("subject", "").lower() != ident
            and c.get("statement", "").lower() != ident
        ]
        if len(self.capsules) < initial_len:
            self._save()
            return True
        return False

    def clear(self) -> int:
        """Clears all capsules."""
        count = len(self.capsules)
        self.capsules = []
        self._save()
        return count

    def audit_anti_hallucination(self, query: str, answer: str) -> dict[str, Any]:
        """
        Audits a generated answer against known certified capsules.
        Detects if an answer aligns with or violates verified ground facts.
        """
        matched = self.query(query, top_k=2, min_sim=0.18)
        if not matched:
            return {"verdict": "UNCONSTRAINED_GENERAL", "grounded": False, "certified": True}

        top = matched[0]
        statement = top.get("statement", "")
        obj = top.get("object", "").lower()

        # Check if the answer mentions the certified object / value
        ans_lower = answer.lower()
        obj_tokens = [t for t in re.findall(r"\w+", obj) if len(t) > 2]

        if any(t in ans_lower for t in obj_tokens) or (obj and obj in ans_lower):
            return {
                "verdict": "CERTIFIED_GROUNDED",
                "grounded": True,
                "certified": True,
                "capsule_id": top["id"],
                "ground_fact": statement
            }

        # Check for potential hallucination / contradiction
        return {
            "verdict": "GROUNDING_GAP_OR_CONTRADICTION",
            "grounded": False,
            "certified": False,
            "capsule_id": top["id"],
            "expected_ground_truth": statement
        }


# Global singleton instance
CAPSULE_STORE = LiveCapsuleStore()
