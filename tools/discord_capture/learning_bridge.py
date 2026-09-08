"""Opt-in owner-DM adapter for the existing private bounded learner.

No shell, network, teacher, source imports, budget renewal or allocator activation.
The gateway must establish owner-DM authority and durable deduplication first.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import signal
import stat
import subprocess
import time

from journal import private_root
from unicode_queries import DATASETS, QueryError, Reference, parse
from task_protocol import hex_id, task_result

MANAGED = {"cnet-control.dll", "cnet-control.deps.json", "cnet-control.runtimeconfig.json",
           "Microsoft.Data.Sqlite.dll", "SQLitePCLRaw.core.dll", "SQLitePCLRaw.batteries_v2.dll",
           "SQLitePCLRaw.provider.e_sqlite3.dll", "runtimes/linux-x64/native/libe_sqlite3.so"}
NATIVE = {"cnet_table_capsule", "cnet_table_verify", "cnet_learning_snapshot", "cnet_capsulectl",
          "cnetd", "libcnet_capsule_core.so"}


class BridgeError(ValueError):
    """Fixed reason code, never private request or arbitrary child output."""


class ProtocolError(BridgeError):
    """A completed child reply is malformed, not an ambiguous transport timeout."""


def require(ok, code):
    if not ok:
        raise BridgeError(code)


def decode(raw):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate_json")
            result[key] = value
        return result
    def constant(_):
        raise BridgeError("nonfinite_json")
    try:
        value = json.loads(raw, object_pairs_hook=unique, parse_constant=constant)
        require(isinstance(value, dict), "json_object_required")
        return value
    except (ValueError, UnicodeError, RecursionError):
        raise ProtocolError("json_refused") from None


def read_private(path, limit):
    private_root(path.parent)
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and info.st_nlink == 1
                and not info.st_mode & 0o077 and 0 < info.st_size <= limit, "file_boundary")
        raw = stream.read(limit + 1)
        require(len(raw) == info.st_size, "file_changed")
    return raw


def trusted_path(path):
    require(path.is_absolute() and path.resolve() == path, "path_refused")
    for parent in path.parents:
        info = parent.lstat()
        require(stat.S_ISDIR(info.st_mode) and info.st_uid in {0, os.getuid()}
                and not info.st_mode & 0o022, "untrusted_ancestor")


def bounded_call(argv, cwd, seconds=12):
    """Bound both pipes and elapsed boot time; kill/reap only this child group."""
    started = time.clock_gettime(time.CLOCK_BOOTTIME)
    output = [bytearray(), bytearray()]
    child = subprocess.Popen(argv, cwd=cwd, env={}, stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(child.stdout, selectors.EVENT_READ, 0)
            selector.register(child.stderr, selectors.EVENT_READ, 1)
            while selector.get_map() or child.poll() is None:
                elapsed = time.clock_gettime(time.CLOCK_BOOTTIME) - started
                require(0 <= elapsed < seconds, "command_timeout")
                for key, _ in selector.select(min(.05, seconds - elapsed)):
                    chunk = os.read(key.fd, 32769)
                    if not chunk:
                        selector.unregister(key.fileobj)
                    else:
                        if len(output[key.data]) + len(chunk) > 32768:
                            raise ProtocolError("command_output_limit")
                        output[key.data].extend(chunk)
            return child.wait(), bytes(output[0]), bytes(output[1])
    finally:
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.wait()
        child.stdout.close()
        child.stderr.close()


class Bridge:
    def __init__(self, root, config_sha256):
        require(Path(root).is_absolute(), "path_refused")
        self.root = private_root(root)
        trusted_path(self.root)
        require(isinstance(config_sha256, str) and re.fullmatch(r"[a-f0-9]{64}", config_sha256), "configuration_pin")
        self.config_bytes = read_private(self.root / "bridge.json", 4096)
        require(hashlib.sha256(self.config_bytes).hexdigest() == config_sha256, "configuration_pin")
        config = decode(self.config_bytes)
        require(set(config) == {"schema_version", "dotnet", "managed_sha256", "native_sha256", "policy_sha256", "sources"}
                and type(config["schema_version"]) is int and config["schema_version"] in (1, 2), "configuration")
        require(all(isinstance(config[k], str) and re.fullmatch(r"[a-f0-9]{64}", config[k])
                    for k in ("managed_sha256", "native_sha256", "policy_sha256")), "configuration")
        require(isinstance(config["sources"], dict) and set(config["sources"]) == set(DATASETS)
                and all(isinstance(pin, str) and re.fullmatch(r"[a-f0-9]{64}", pin)
                        for pin in config["sources"].values()), "configuration")
        require(isinstance(config["dotnet"], str), "dotnet_host")
        self.dotnet = Path(config["dotnet"])
        trusted_path(self.dotnet)
        info = self.dotnet.lstat()
        require(stat.S_ISREG(info.st_mode) and info.st_uid in {0, os.getuid()}
                and not info.st_mode & 0o022 and os.access(self.dotnet, os.X_OK), "dotnet_host")
        self.config = config
        self.task_mode = config["schema_version"] == 2
        self.source_pins = config["sources"]
        self.reference = Reference(self.root / "UnicodeData-Latin1.txt")
        self.failed = False
        self.check_installation()

    def check_installation(self):
        private_root(self.root)
        trusted_path(self.root)
        require(read_private(self.root / "bridge.json", 4096) == self.config_bytes, "configuration_changed")
        for name, pin in (("managed.json", "managed_sha256"), ("runtime.json", "native_sha256"), ("policy.json", "policy_sha256")):
            raw = read_private(self.root / name, 16384 if name == "policy.json" else 4096)
            require(hashlib.sha256(raw).hexdigest() == self.config[pin], "installation_changed")
            fields = decode(raw)
            if name == "policy.json":
                expected = [dict(id=dataset, authority="verified_tool", **(
                    dict(symbol_vocabulary_sha256=self.reference.vocabulary_sha256) if dataset in DATASETS[2:] else {}))
                    for dataset in DATASETS]
                require(fields.get("enabled") is True and fields.get("allocator_enabled") is False
                        and fields.get("datasets") == expected, "policy_refused")
                require(type(fields.get("max_run_seconds")) is int and 1 <= fields["max_run_seconds"] <= 604800
                        and type(fields.get("max_probe_gap_seconds")) is int and 1 <= fields["max_probe_gap_seconds"] <= 7200,
                        "policy_refused")
                self.max_run_seconds = fields["max_run_seconds"]
                self.max_probe_gap_seconds = min(120, fields["max_probe_gap_seconds"])
            else:
                managed = name == "managed.json"
                require(set(fields) == ({"schema_version", "target", "files"} if managed else {"schema_version", "files"})
                        and type(fields["schema_version"]) is int and fields["schema_version"] == 1
                        and (not managed or fields["target"] == "linux-x64")
                        and isinstance(fields["files"], dict)
                        and set(fields["files"]) == (MANAGED if managed else NATIVE), "runtime_inventory")
                for file, file_pin in fields["files"].items():
                    raw_file = read_private(self.root / ("managed" if managed else "native") / file, 64 * 1024**2)
                    require(hashlib.sha256(raw_file).hexdigest() == file_pin, "runtime_changed")
        self.reference.verify()
        for dataset, pin in self.source_pins.items():
            require(hashlib.sha256(read_private(self.root / "work/data" / (dataset + ".tsv"), 4096)).hexdigest() == pin,
                    "source_changed")

    def command(self, verb, *args):
        allowed = {"status", "pause", "task", "trace"} if getattr(self, "task_mode", False) else {"status", "pause", "ask", "lookup"}
        require(verb in allowed, "command_refused")
        code, output, error = bounded_call([str(self.dotnet), str(self.root / "managed/cnet-control.dll"),
                                            "learning", verb, str(self.root), *args], self.root)
        if error or code not in ((0, 2) if verb == "task" else (0,)):
            # The fixed native parser deliberately conflates corrupt replies
            # with observation failures. Conservatively stop this bridge/owner.
            raise ProtocolError("command_refused")
        result = decode(output)
        if verb == "task":
            row = result.get("experience")
            expected_code = 2 if isinstance(row, dict) and row.get("State") in ("unknown", "conflict") else 0
            if code != expected_code:
                raise ProtocolError("task_exit_mismatch")
        return result

    def task_answer(self, text, identity):
        """Gateway calls once, only on the fresh durable capture-admission path.

        Every outcome consumes this opt-in finite task route. No legacy/peer
        fallback and no source approval, import, activation or budget renewal.
        """
        attempted = False
        try:
            require(getattr(self, "task_mode", False) is True and not self.failed, "task_mode_unavailable")
            require(hex_id(identity) and isinstance(text, str) and "\0" not in text, "task_arguments")
            self.check_installation()
            self.require_owner()
            attempted = True
            try:
                reply = self.command("task", "unreviewed", identity, text)
                self.check_installation()
                try:
                    result = task_result(reply, identity, self.source_pins, self.reference)
                except (ValueError, TypeError, KeyError):
                    raise ProtocolError("task_refused") from None
                print(json.dumps(dict(event="captured_task_result", request_id=identity,
                      proposal_status=reply["proposal"]["Status"], replayed=reply["replayed"],
                      experience_state=reply["experience"]["State"] if reply["experience"] else None,
                      peer_status=result[1])), flush=True)
                return result
            except ProtocolError:
                self.failed = True
                try:
                    self.command("pause")
                except Exception:
                    print(json.dumps(dict(event="captured_task_pause_failed", request_id=identity)), flush=True)
                raise
        except Exception:
            print(json.dumps(dict(event="captured_task_refused", request_id=identity if hex_id(identity) else None,
                  code="task_refused" if self.failed else "outcome_unknown" if attempted else "unavailable")), flush=True)
            return ("ABSTAIN: learning service unavailable or outcome unknown; no automatic retry.",
                    "peer_unknown" if attempted and not self.failed else "peer_error")

    def require_owner(self):
        status = self.command("status")
        run = status.get("run")
        require(status.get("event") == "learning_status" and status.get("paused") is False
                and status.get("run_state") == "running" and isinstance(run, dict), "owner_unavailable")
        require(all(status.get(key) == self.config[key] for key in ("managed_sha256", "native_sha256", "policy_sha256")),
                "owner_pin_mismatch")
        boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        now = time.clock_gettime_ns(time.CLOCK_BOOTTIME)
        require(run.get("State") == "running" and run.get("Boot") == boot
                and type(run.get("LastNanoseconds")) is int and type(run.get("StartNanoseconds")) is int
                and 0 <= run["StartNanoseconds"] <= run["LastNanoseconds"] <= now
                and now - run["LastNanoseconds"] <= self.max_probe_gap_seconds * 10**9
                and now - run["StartNanoseconds"] < self.max_run_seconds * 10**9,
                "owner_stale")

    def check_response(self, query, response):
        dataset, key = query
        symbolic = dataset in DATASETS[2:]
        fields = {"event", "correlation_id", "dataset", "verified"} | (
            {"token", "label", "source_sha256", "symbol_vocabulary_sha256"} if symbolic else {"key", "value"})
        require(set(response) == fields and response["event"] == ("learning_symbol_answer" if symbolic else "learning_answer")
                and response["dataset"] == dataset and type(response["verified"]) is bool
                and isinstance(response["correlation_id"], str) and re.fullmatch(r"[a-f0-9]{32}", response["correlation_id"]),
                "answer_protocol")
        require(type(response["token" if symbolic else "key"]) is type(key)
                and response["token" if symbolic else "key"] == key, "answer_identity")
        value = response["label" if symbolic else "value"]
        if symbolic:
            require(response["source_sha256"] == self.source_pins[dataset]
                    and response["symbol_vocabulary_sha256"] == self.reference.vocabulary_sha256, "answer_identity")
        if not response["verified"]:
            require(value is None, "answer_value")
        elif symbolic:
            require(isinstance(value, str) and len(value) <= 128, "answer_value")
        else:
            require(type(value) is int and 0 <= value <= 65535, "answer_value")
        expected = self.reference.expected(query)
        if response["verified"]:
            require(expected is not None and type(value) is type(expected) and value == expected, "answer_mismatch")
            rendered = value if symbolic else f"{value} (U+{value:04X})"
            return f"[Verified capsule; Unicode 17 finite lookup] {rendered}", "peer_ok"
        if expected is None:
            return "ABSTAIN: outside this approved finite Unicode table; no value inferred.", "peer_ok"
        return "ABSTAIN: no certified answer yet; request recorded for bounded learning. Ask again after certification.", "peer_ok"

    def answer(self, text):
        try:
            query = parse(text)
        except QueryError:
            return "ABSTAIN: use unicode upper/lower 0..255 or unicode category/bidi EXACT_UNICODE_NAME.", "peer_error"
        if query is None:
            return None
        attempted = False
        try:
            require(not self.failed, "bridge_latched")
            self.check_installation()
            self.require_owner()
            attempted = True
            dataset, key = query
            try:
                response = self.command("lookup" if dataset in DATASETS[2:] else "ask", dataset, str(key))
                self.check_installation()
                try:
                    return self.check_response(query, response)
                except BridgeError:
                    raise ProtocolError("answer_refused") from None
            except ProtocolError:
                self.failed = True
                try:
                    self.command("pause")
                except Exception:
                    print("[learning_bridge] pause_failed", flush=True)
                raise
        except Exception:
            protocol = self.failed
            print("[learning_bridge]", "answer_refused" if protocol else "outcome_unknown" if attempted else "unavailable", flush=True)
            return ("ABSTAIN: learning service unavailable or outcome unknown; no automatic retry.",
                    "peer_unknown" if attempted and not protocol else "peer_error")
