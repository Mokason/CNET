"""Link identity only: no dispatch, origin attestation or correctness authority."""
import hashlib
import json

from journal import CaptureError, MAX_TEXT, snowflake


def task_identity(owner, channel, message, delivered):
    if not all(snowflake(value) for value in (owner, channel, message)):
        raise CaptureError("task_scope")
    if not isinstance(delivered, str) or not delivered or len(delivered.encode("utf-8")) > MAX_TEXT:
        raise CaptureError("task_text")
    text_sha256 = hashlib.sha256(delivered.encode("utf-8")).hexdigest()
    # Fixed namespace/scope; no mutable timestamps, segment, completion or replies.
    body = json.dumps(["cnet_discord_task_v1", owner, channel, message, text_sha256],
                      ensure_ascii=True, separators=(",", ":")).encode("ascii")
    return hashlib.sha256(body).hexdigest()[:32]
