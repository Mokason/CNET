"""Frozen synthetic proposal scoring. No answers, learning, or live requests."""
from collections import Counter
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

MAX_BYTES = 131072
STATUSES = {"ready", "clarify", "abstain"}
ATOM = re.compile(r"[a-zA-Z0-9_-]{1,48}\Z")
FREEZE_SHA256 = "1bf769fa77a416237a8a1e2534fbe949346a8e22fba630f1c0a4551e0c7be0fe"
FOLLOWUP_FREEZE_SHA256 = "69ab525fef0e91a6d64398a0d5829f3b314b04cd7ced2a7ea4e3c6a605f31a49"
ROUND3_FREEZE_SHA256 = "e78224298ce97e8fc8cd69088c64a54463934b2d2731030de8146d1c9b65e6b9"
ROUND4_FREEZE_SHA256 = "903f5fddd1788e52e779b67ab0c128930c0d914bb0fc679f1456b7e0c7b00a75"


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
        if not isinstance(case["text"], str) or len(case["text"]) > 2048 or not isinstance(case["status"], str) or case["status"] not in STATUSES:
            raise ValueError("case_text_status")
        if case["status"] == "ready":
            if not isinstance(case["operation"], str) or case["operation"] not in {"upper", "lower"} or type(case["key"]) is not int or not 0 <= case["key"] <= 255:
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
    if not isinstance(value["Status"], str) or value["Status"] not in STATUSES or not isinstance(value["Code"], str) or not ATOM.fullmatch(value["Code"]):
        raise ValueError("proposal_status")
    if value["Status"] == "ready":
        if not isinstance(value["Dataset"], str) or value["Dataset"] not in {"unicode17_upper_latin1", "unicode17_lower_latin1"} or type(value["Key"]) is not int or not 0 <= value["Key"] <= 255 or value["Prompt"] is not None:
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


def read_bounded(path, limit=MAX_BYTES):
    with open(path, "rb") as stream:
        raw = stream.read(limit + 1)
    if len(raw) > limit:
        raise ValueError("file_bound")
    return raw


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def encode_requests(texts):
    # Private probe wire: arrays of original UTF-16 units. JSON string decoders
    # may reject lone surrogates before the actual parser can abstain on them.
    units = []
    for text in texts:
        raw = text.encode("utf-16-le", errors="surrogatepass")
        units.append([raw[i] | raw[i + 1] << 8 for i in range(0, len(raw), 2)])
    return json.dumps(units, separators=(",", ":")).encode("ascii")


def suite_identity(suite, collection):
    if suite == "original" and collection in {"qualification", "confirmation"}:
        return "task_paraphrases_20260909", FREEZE_SHA256
    if suite == "followup" and collection == "confirmation":
        return "task_paraphrases_followup_20260909", FOLLOWUP_FREEZE_SHA256
    if suite == "round3" and collection == "confirmation" and ROUND3_FREEZE_SHA256 is not None:
        return "task_paraphrases_round3_20260909", ROUND3_FREEZE_SHA256
    if suite == "round4" and collection == "confirmation":
        return "task_paraphrases_round4_20260909", ROUND4_FREEZE_SHA256
    raise ValueError("suite_collection")


def run(args):
    repository = Path(__file__).resolve().parents[2]
    directory, freeze_sha = suite_identity(args.suite, args.collection)
    corpus_root = repository / "benchmarks" / directory
    frozen_bytes = read_bounded(corpus_root / "freeze.json")
    if digest(frozen_bytes) != freeze_sha:
        raise ValueError("freeze_metadata_changed")
    frozen = decode(frozen_bytes)
    # The separately pinned third manifest records corpus identity in a nested
    # object. Select that exact layout, not a permissive fallback or file path.
    identity = frozen.get("corpus") if args.suite == "round3" and isinstance(frozen, dict) else frozen
    field = "sha256" if args.suite == "round3" else args.collection + "_sha256"
    corpus_sha = identity.get(field) if isinstance(identity, dict) else None
    if not isinstance(corpus_sha, str) or not re.fullmatch(r"[a-f0-9]{64}", corpus_sha):
        raise ValueError("freeze_corpus_identity")
    parser = repository / "dotnet/CnetControlPlane/Learning/LearningTaskProposal.cs"
    if digest(read_bounded(parser)) != args.parser_sha256:
        raise ValueError("parser_source_pin_mismatch")
    # No inference that an arbitrary supplied binary was built from the source:
    # both identities are reported; the documented fresh-build step binds them.
    assembly_bytes = read_bounded(args.assembly, 32 * 1024 * 1024)
    if digest(assembly_bytes) != args.assembly_sha256:
        raise ValueError("assembly_pin_mismatch")
    probe = repository / ".artifacts/task-verified/bin/TaskParaphraseProbe/debug/cnet-task-paraphrase-probe.dll"
    probe_sha = digest(read_bounded(probe, 32 * 1024 * 1024))
    corpus_bytes = read_bounded(corpus_root / (args.collection + ".json"))
    cases = load_corpus(corpus_bytes, corpus_sha, args.collection)
    dotnet = shutil.which("dotnet")
    if not dotnet:
        raise ValueError("dotnet_unavailable")
    result = subprocess.run([dotnet, str(probe), str(Path(args.assembly).resolve()), args.assembly_sha256],
                            input=encode_requests([c["text"] for c in cases]),
                            capture_output=True, timeout=30, env={})
    if result.returncode or result.stderr:
        raise ValueError("managed_probe_refused")
    response = decode(result.stdout)
    if not isinstance(response, dict) or set(response) != {"schema", "assembly_sha256", "proposals"} or type(response["schema"]) is not int or response["schema"] != 1 or response["assembly_sha256"] != args.assembly_sha256:
        raise ValueError("managed_probe_identity")
    if digest(read_bounded(parser)) != args.parser_sha256 or digest(read_bounded(probe, 32 * 1024 * 1024)) != probe_sha or digest(read_bounded(corpus_root / (args.collection + ".json"))) != corpus_sha:
        raise ValueError("evaluation_inputs_changed")
    return {"schema": 1, "suite": args.suite, "collection": args.collection, "origin": "synthetic", "training_eligible": False,
            "freeze_sha256": freeze_sha, "corpus_sha256": corpus_sha,
            "parser_source_sha256": args.parser_sha256, "assembly_sha256": args.assembly_sha256,
            "probe_sha256": probe_sha, **score(cases, response["proposals"])}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("collection", choices=("qualification", "confirmation"))
    parser.add_argument("--suite", choices=("original", "followup", "round3", "round4"), default="original",
                        help="explicit frozen population; only original has qualification")
    parser.add_argument("--assembly", required=True)
    parser.add_argument("--assembly-sha256", required=True)
    parser.add_argument("--parser-sha256", required=True)
    parser.add_argument("--output", required=True, help="new report file; existing output is never overwritten")
    args = parser.parse_args(argv)
    try:
        if not Path(args.output).is_absolute() or not Path(args.assembly).is_absolute() or any(not re.fullmatch(r"[a-f0-9]{64}", value) for value in (args.assembly_sha256, args.parser_sha256)):
            raise ValueError("arguments")
        # Reserve the output before opening a corpus or invoking the parser.
        # A failed attempt may retain an empty file, never a successful score.
        fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            result = run(args)
            json.dump(result, output, ensure_ascii=True, sort_keys=True, indent=2)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        print(json.dumps({k: result[k] for k in ("suite", "collection", "passed", "total", "exact", "wrong_ready", "by_status", "gates")}, sort_keys=True))
        return 0 if result["passed"] else 1
    except (ValueError, OSError, subprocess.SubprocessError):
        print("PARAPHRASE_EVALUATION_REFUSED", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
