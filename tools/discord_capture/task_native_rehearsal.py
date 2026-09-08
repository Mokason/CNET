"""Test driver: synthetic capture + actual pinned local managed/native bridge.

Invoked only by LearningCapturedTaskTests in a fresh disposable installation.
No Discord, HTTP, websocket, teacher, production journal or origin review.
"""
import json
import contextlib
from pathlib import Path
import sys

from capture_task_identity import task_identity
from journal import Journal
from learning_bridge import Bridge
from task_inbox import inspect_page

OWNER, CHANNEL = "111111111111111111", "222222222222222222"


def run(root, pin, stage):
    assert stage in ("before", "after", "terminal")
    bridge = Bridge(root, pin)
    capture = root / "synthetic-capture"
    if stage == "before":
        capture.mkdir(mode=0o700)
    if stage == "terminal":
        before = (capture / "capture.sqlite").read_bytes()
        ledger_before = (root / "work/ledger.sqlite").read_bytes()
        page = inspect_page(capture, bridge)
        assert len(page["captures"]) == 7 and not page["training_eligible"] and not page["origin_attested"]
        assert before == (capture / "capture.sqlite").read_bytes()
        assert ledger_before == (root / "work/ledger.sqlite").read_bytes()
        return dict(event="capture_task_rehearsal", stage=stage, provenance="synthetic_fixture",
                    captures=7, observations=5, training_eligible=False, origin_attested=False, read_only=True)
    journal = Journal(capture, OWNER, CHANNEL)
    try:
        ids = {}
        def dispatch(number, text, wanted):
            message_id = str(333333333333333330 + number)
            data = dict(id=message_id, type=0, channel_id=CHANNEL, author=dict(id=OWNER, bot=False),
                        content=text, timestamp="2026-09-09T00:00:00Z")
            assert journal.begin(data, text)
            identity = task_identity(OWNER, CHANNEL, message_id, text)
            # Keep the rehearsal's final stdout a single receipt. Production
            # bridge diagnostic events are retained on the test's stderr.
            with contextlib.redirect_stdout(sys.stderr):
                answer, status = bridge.task_answer(text, identity)
            assert status == "peer_ok" and wanted in answer, (stage, number, answer, status)
            journal.finish(message_id, status, "reply_sent")  # Simulated delivery only.
            assert not journal.begin(data, text)  # No second bridge invocation.
            ids[str(number)] = identity
        if stage == "before":
            dispatch(1, "What's the uppercase of µ?", "owner approval")
            dispatch(2, "convert 'A' to lowercase", "owner approval")
            dispatch(3, "uppercase 65", "CLARIFY")
            dispatch(4, "What is tomorrow's weather?", "ABSTAIN")
        else:
            dispatch(5, "Please convert 'µ' to uppercase.", "924 (U+039C)")
            dispatch(6, "What is the lowercase of A?", "97 (U+0061)")
            dispatch(7, "uppercase ß", "outside this approved finite Unicode table")
        page = inspect_page(capture, bridge)
        assert not page["training_eligible"] and not page["origin_attested"]
        assert len(page["captures"]) == (4 if stage == "before" else 7)
        assert sum(row["link_state"] == "no_observation" for row in page["captures"]) == 2
        return dict(event="capture_task_rehearsal", stage=stage, provenance="synthetic_fixture",
                    requests=ids, training_eligible=False, origin_attested=False)
    finally:
        journal.close()


if __name__ == "__main__":
    print(json.dumps(run(Path(sys.argv[1]), sys.argv[2], sys.argv[3]), sort_keys=True))
