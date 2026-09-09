"""Frozen synthetic proposal scoring. No answers, learning, or live requests."""
from collections import Counter
import hashlib
import json
import re

MAX_BYTES = 131072
STATUSES = {"ready", "clarify", "abstain"}
ATOM = re.compile(r"[a-zA-Z0-9_-]{1,48}\Z")


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate_json_property")
        result[key] = value
    return result


def decode(raw):
    if not isinstance(raw, bytes) or not 1 <= len(raw) <= MAX_BYTES:
        raise ValueError("byte_bound")
    try:
        return json.loads(raw.decode("utf-8", errors="strict"), object_pairs_hook=_object)
    except (UnicodeError, RecursionError) as error:
        raise ValueError("invalid_json") from error


def load_corpus(raw, sha256, name):
    if name not in {"qualification", "confirmation"} or not isinstance(sha256, str) or not re.fullmatch(r"[a-f0-9]{64}", sha256):
        raise ValueError("corpus_identity")
    if hashlib.sha256(raw).hexdigest() != sha256:
        raise ValueError("corpus_hash_mismatch")
    value = decode(raw)
    if not isinstance(value, dict) or set(value) != {"schema", "name", "cases"} or type(value["schema"]) is not int or value["schema"] != 1 or value["name"] != name:
        raise ValueError("corpus_schema")
    cases = value["cases"]
    if not isinstance(cases, list) or len(cases) != 128:
        raise ValueError("corpus_count")
    for case in cases:
        if not isinstance(case, dict) or set(case) != {"id", "family", "text", "status", "operation", "key"}:
            raise ValueError("case_schema")
        if any(not isinstance(case[k], str) or not ATOM.fullmatch(case[k]) for k in ("id", "family")):
            raise ValueError("case_identity")
        if not isinstance(case["text"], str) or len(case["text"]) > 2048 or case["status"] not in STATUSES:
            raise ValueError("case_text_status")
        if case["status"] == "ready":
            if case["operation"] not in {"upper", "lower"} or type(case["key"]) is not int or not 0 <= case["key"] <= 255:
                raise ValueError("case_typed_input")
        elif case["operation"] is not None or case["key"] is not None:
            raise ValueError("nonready_label_authority")
    if len({c["id"] for c in cases}) != 128 or len({c["text"] for c in cases}) != 128:
        raise ValueError("duplicate_case")
    if len({c["family"] for c in cases}) < 8 or Counter(c["status"] for c in cases) != {"ready": 80, "clarify": 24, "abstain": 24} or Counter(c["operation"] for c in cases) != {"upper": 40, "lower": 40, None: 48}:
        raise ValueError("population_quota")
    return cases


def _proposal(value):
    if not isinstance(value, dict) or set(value) != {"Status", "Code", "Dataset", "Key", "Prompt"}:
        raise ValueError("proposal_schema")
    if value["Status"] not in STATUSES or not isinstance(value["Code"], str) or not ATOM.fullmatch(value["Code"]):
        raise ValueError("proposal_status")
    if value["Status"] == "ready":
        if value["Dataset"] not in {"unicode17_upper_latin1", "unicode17_lower_latin1"} or type(value["Key"]) is not int or not 0 <= value["Key"] <= 255 or value["Prompt"] is not None:
            raise ValueError("proposal_typed_input")
    elif value["Dataset"] is not None or value["Key"] is not None:
        raise ValueError("nonready_proposal_authority")
    elif value["Status"] == "clarify" and (not isinstance(value["Prompt"], str) or not 1 <= len(value["Prompt"]) <= 512):
        raise ValueError("missing_clarification")
    elif value["Status"] == "abstain" and value["Prompt"] is not None:
        raise ValueError("abstention_prompt")


def score(cases, proposals):
    if not isinstance(proposals, list) or len(proposals) != len(cases) or len(cases) != 128:
        raise ValueError("proposal_count")
    rows = []
    for case, proposal in zip(cases, proposals, strict=True):
        _proposal(proposal)
        exact = case["status"] == proposal["Status"]
        if case["status"] == "ready":
            exact = exact and proposal["Dataset"] == f'unicode17_{case["operation"]}_latin1' and proposal["Key"] == case["key"]
        rows.append({"id": case["id"], "family": case["family"], "expected_status": case["status"],
                     "operation": case["operation"], "key": case["key"], "proposal": proposal,
                     "exact": exact, "wrong_ready": proposal["Status"] == "ready" and not exact})

    def groups(field, allowed=None):
        values = sorted({r[field] for r in rows if allowed is None or r[field] in allowed})
        return {v: {"total": sum(r[field] == v for r in rows), "exact": sum(r[field] == v and r["exact"] for r in rows)} for v in values}

    statuses = groups("expected_status")
    operations = groups("operation", {"upper", "lower"})
    wrong = sum(r["wrong_ready"] for r in rows)
    gates = {"zero_wrong_ready": wrong == 0,
             "ready_90_percent": statuses["ready"]["exact"] * 10 >= 80 * 9,
             "upper_85_percent": operations["upper"]["exact"] * 100 >= 40 * 85,
             "lower_85_percent": operations["lower"]["exact"] * 100 >= 40 * 85,
             "clarify_90_percent": statuses["clarify"]["exact"] * 10 >= 24 * 9,
             "abstain_95_percent": statuses["abstain"]["exact"] * 100 >= 24 * 95}
    return {"passed": all(gates.values()), "total": 128, "exact": sum(r["exact"] for r in rows),
            "wrong_ready": wrong, "gates": gates, "by_status": statuses, "by_operation": operations,
            "by_family": groups("family"), "cases": rows}
