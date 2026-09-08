"""Closed, bounded task envelopes. A proposal is not a correctness label."""
import re

from unicode_queries import DATASETS

EXPERIENCE_FIELDS = {"Sequence", "RequestId", "Dataset", "Key", "Origin", "Boot", "StartedNanoseconds",
                     "FinishedBoot", "FinishedNanoseconds", "State", "SourceSha256", "Expected", "Value",
                     "ApprovedSourceSha256", "ApprovedExpected", "ApprovedBoot", "ApprovedNanoseconds", "ReviewConflict"}
STATES = {"pending", "verified", "miss", "awaiting_evidence", "abstain", "unknown", "conflict"}


def check(ok):
    if not ok:
        raise ValueError("task_protocol")


def hex_id(value, size=32):
    return isinstance(value, str) and re.fullmatch(r"[a-f0-9]{%d}" % size, value) is not None


def integer(value, high):
    return type(value) is int and 0 <= value <= high


def boot_id(value):
    return isinstance(value, str) and re.fullmatch(r"[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}", value) is not None


def validate_experience(row, identity):
    """Validate stored shape only; history does not certify a current answer."""
    check(isinstance(row, dict) and set(row) == EXPERIENCE_FIELDS)
    check(hex_id(identity) and row["RequestId"] == identity and integer(row["Sequence"], 2**63-1)
          and row["Sequence"] > 0 and isinstance(row["Dataset"], str)
          and re.fullmatch(r"[a-z][a-z0-9_]{0,63}", row["Dataset"]) is not None
          and integer(row["Key"], 255) and row["Origin"] in ("synthetic", "unreviewed"))
    check(boot_id(row["Boot"]) and integer(row["StartedNanoseconds"], 2**63-1)
          and isinstance(row["State"], str) and row["State"] in STATES and type(row["ReviewConflict"]) is bool)
    for field in ("SourceSha256", "ApprovedSourceSha256"):
        check(row[field] is None or hex_id(row[field], 64))
    for field in ("Expected", "Value", "ApprovedExpected"):
        check(row[field] is None or integer(row[field], 65535))
    check(row["SourceSha256"] is not None or row["Expected"] is None)
    finished = row["State"] != "pending"
    check((boot_id(row["FinishedBoot"]) and integer(row["FinishedNanoseconds"], 2**63-1)) if finished
          else row["FinishedBoot"] is None and row["FinishedNanoseconds"] is None)
    if row["FinishedBoot"] == row["Boot"]:
        check(row["FinishedNanoseconds"] >= row["StartedNanoseconds"])
    approved = row["ApprovedSourceSha256"] is not None
    check((boot_id(row["ApprovedBoot"]) and integer(row["ApprovedNanoseconds"], 2**63-1)
           and row["ApprovedExpected"] is not None) if approved else
          row["ApprovedExpected"] is None and row["ApprovedBoot"] is None and row["ApprovedNanoseconds"] is None)
    if row["State"] == "verified":
        check(row["Expected"] is not None and row["Value"] == row["Expected"])
    elif row["State"] != "conflict":
        check(row["Value"] is None)
    return row


def task_result(reply, identity, pins, reference):
    check(isinstance(reply, dict) and set(reply) == {"event", "correlation_id", "proposal", "replayed", "experience"}
          and reply["event"] == "learning_task" and hex_id(reply["correlation_id"]) and type(reply["replayed"]) is bool)
    proposal = reply["proposal"]
    check(isinstance(proposal, dict) and set(proposal) == {"Status", "Code", "Dataset", "Key", "Prompt"})
    if proposal["Status"] != "ready":
        check(reply["experience"] is None and reply["replayed"] is False
              and proposal["Dataset"] is None and proposal["Key"] is None)
        if proposal["Status"] == "clarify":
            check(proposal["Code"] == "specify_case_input" and isinstance(proposal["Prompt"], str)
                  and len(proposal["Prompt"]) <= 256)
            return 'CLARIFY: specify uppercase or lowercase and one quoted Latin-1 character, or U+00B5.', "peer_ok"
        check(proposal["Status"] == "abstain" and proposal["Code"] in
              ("unsupported_intent", "input_bounds", "input_domain", "dataset_not_authorized") and proposal["Prompt"] is None)
        return "ABSTAIN: this task route supports only bounded Latin-1 uppercase/lowercase requests.", "peer_ok"
    check(proposal["Code"] == "typed_case_change" and proposal["Dataset"] in DATASETS[:2]
          and integer(proposal["Key"], 255) and proposal["Prompt"] is None)
    row = validate_experience(reply["experience"], identity)
    check(row["Dataset"] == proposal["Dataset"] and row["Key"] == proposal["Key"] and row["Origin"] == "unreviewed")
    check(row["State"] != "conflict" and not row["ReviewConflict"])
    if reply["replayed"]:
        return "ABSTAIN: prior observation retained; no new verification or automatic retry.", "peer_unknown"
    check(row["State"] != "pending" and row["ApprovedSourceSha256"] is None)
    expected = reference.expected((row["Dataset"], row["Key"]))
    check(row["SourceSha256"] == pins[row["Dataset"]] and row["Expected"] == expected)
    if row["State"] == "verified":
        check(expected is not None and row["Value"] == expected)
        return f'[Verified capsule; Unicode 17 finite lookup] {row["Value"]} (U+{row["Value"]:04X})', "peer_ok"
    if row["State"] == "unknown":
        return "ABSTAIN: task outcome unknown; no automatic retry or learning approval.", "peer_unknown"
    if row["State"] == "abstain":
        check(expected is None)
        return "ABSTAIN: outside this approved finite Unicode table; no value inferred.", "peer_ok"
    check(row["State"] == "miss" and expected is not None)
    return "ABSTAIN: observation recorded; learning requires separate owner approval of the external source.", "peer_ok"
