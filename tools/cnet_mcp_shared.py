#!/usr/bin/env python3
"""Singleton CNET MCP host + thin stdio bridge.

Problem: Hermes spawns one stdio MCP child per host (TUI / gateway /
dashboard). CnetMcpServer embeds a full SoulHost + soul pack, so N hosts
meant N × multi‑GB brains.

Fix: one long‑lived daemon owns the real CnetMcpServer. Every Hermes
`launch.sh` only starts a tiny bridge that speaks stdio to Hermes and
JSON‑RPC over a Unix socket to the shared daemon.

  Hermes ──stdio──► bridge (tiny) ──unix──► daemon ──stdio──► CnetMcpServer (one)

Usage:
  cnet_mcp_shared.py daemon     # foreground (systemd)
  cnet_mcp_shared.py ensure     # start daemon if needed, then exit 0
  cnet_mcp_shared.py client     # stdio bridge (what launch.sh execs)
  cnet_mcp_shared.py status
  cnet_mcp_shared.py stop
"""
from __future__ import annotations

import argparse
import asyncio
import fcntl
import json
import os
import signal
import socket
import sys
import time
from pathlib import Path
from typing import Any, Dict, Optional, Tuple

SCRIPT_DIR = Path(os.environ.get("CNET_MCP_WORKDIR", str(Path(__file__).resolve().parent)))
SERVER_BIN = Path(os.environ.get("CNET_MCP_SERVER_BIN", str(SCRIPT_DIR / "CnetMcpServer")))
# Native evidence has its own tighter bound. Full legacy inventory metadata can
# exceed asyncio's 64 KiB default; never leave a dead reader behind on refusal.
MAX_RESPONSE_FRAME = 2 * 1024 * 1024

RUNTIME_DIR = Path(os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}") / "cnet"
SOCK_PATH = Path(os.environ.get("CNET_MCP_SHARED_SOCK", str(RUNTIME_DIR / "mcp-shared.sock")))
LOCK_PATH = Path(os.environ.get("CNET_MCP_SHARED_LOCK", str(RUNTIME_DIR / "mcp-shared.lock")))
PID_PATH = Path(os.environ.get("CNET_MCP_SHARED_PID", str(RUNTIME_DIR / "mcp-shared.pid")))
LOG_PATH = Path(
    os.environ.get(
        "CNET_MCP_SHARED_LOG",
        str(Path.home() / ".local" / "share" / "cnet-mcp" / "shared.log"),
    )
)

READY_TIMEOUT_S = float(os.environ.get("CNET_MCP_SHARED_READY_TIMEOUT", "120"))
ENSURE_WAIT_S = float(os.environ.get("CNET_MCP_SHARED_ENSURE_WAIT", "90"))


def _log(msg: str) -> None:
    line = f"[cnet-mcp-shared {time.strftime('%H:%M:%S')}] {msg}"
    try:
        LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
        with open(LOG_PATH, "a", encoding="utf-8") as fh:
            fh.write(line + "\n")
    except OSError:
        pass
    print(line, file=sys.stderr, flush=True)


def _runtime_prep() -> None:
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)


def _server_env() -> Dict[str, str]:
    env = os.environ.copy()
    env.setdefault("DOTNET_ROOT", str(Path.home() / "dotnet"))
    env.setdefault("DOTNET_ROOT_X64", env["DOTNET_ROOT"])
    ld = env.get("LD_LIBRARY_PATH", "")
    prefix = f"{SCRIPT_DIR}:/home/marble/AI/CNET"
    env["LD_LIBRARY_PATH"] = f"{prefix}:{ld}" if ld else prefix
    env.setdefault("CNET_MODEL_PATH", "/home/marble/AI/CNET/soul_gemma4v2_final.cnb")
    env.setdefault("CNET_BASE_PATH", env["CNET_MODEL_PATH"])
    env.setdefault("CNET_GAP_INBOX", "/home/marble/AI/CNET/soul_gemma4v2_final.cnb.inbox")
    env.setdefault("CNET_HEALTH_TICK_SECONDS", "60")
    env.setdefault("CNET_ORACLE_INT8", "1")
    env.setdefault("CNET_SOUL_RELOAD_MIN_SEC", "3600")
    env.setdefault("CNET_SOUL_RELOAD_SAME_SIZE", "0")
    return env


def _pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def _read_pid() -> Optional[int]:
    try:
        return int(PID_PATH.read_text(encoding="utf-8").strip())
    except (OSError, ValueError):
        return None


def _socket_alive() -> bool:
    if not SOCK_PATH.exists():
        return False
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(1.0)
            s.connect(str(SOCK_PATH))
        return True
    except OSError:
        return False


def status_text() -> str:
    pid = _read_pid()
    return (
        f"sock={SOCK_PATH} exists={SOCK_PATH.exists()} connectable={_socket_alive()}\n"
        f"pid_file={pid} alive={_pid_alive(pid) if pid else False}\n"
        f"lock={LOCK_PATH} log={LOG_PATH}\n"
    )


# ── Daemon ──────────────────────────────────────────────────────────────


class SharedDaemon:
    def __init__(self) -> None:
        self._lock_fd: Optional[int] = None
        self._backend: Optional[asyncio.subprocess.Process] = None
        self._backend_lock = asyncio.Lock()
        self._pending: Dict[str, Tuple[asyncio.StreamWriter, Any]] = {}
        self._id_seq = 0
        self._reader_task: Optional[asyncio.Task] = None
        self._stopping = False
        self._backend_ready = asyncio.Event()

    def acquire_singleton(self) -> None:
        _runtime_prep()
        self._lock_fd = os.open(str(LOCK_PATH), os.O_RDWR | os.O_CREAT, 0o600)
        try:
            fcntl.flock(self._lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise SystemExit("another cnet-mcp shared daemon holds the lock") from exc
        if SOCK_PATH.exists():
            try:
                SOCK_PATH.unlink()
            except OSError:
                pass
        PID_PATH.write_text(str(os.getpid()), encoding="utf-8")

    async def start_backend(self) -> None:
        if not SERVER_BIN.exists():
            raise FileNotFoundError(f"missing {SERVER_BIN}")
        _log(f"starting backend {SERVER_BIN}")
        self._backend = await asyncio.create_subprocess_exec(
            str(SERVER_BIN),
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            env=_server_env(),
            cwd=str(SCRIPT_DIR),
            limit=MAX_RESPONSE_FRAME,
        )
        assert self._backend.stdin and self._backend.stdout and self._backend.stderr
        self._reader_task = asyncio.create_task(self._backend_stdout_loop())
        asyncio.create_task(self._backend_stderr_loop())
        asyncio.create_task(self._backend_watch_exit())
        # Probe with initialize — proves native host is up enough to speak MCP.
        await self._probe_ready()

    async def _backend_watch_exit(self) -> None:
        assert self._backend is not None
        rc = await self._backend.wait()
        _log(f"backend exited rc={rc}")
        # Fail the daemon so systemd (or ensure) can restart a clean pair.
        if not self._stopping:
            os.kill(os.getpid(), signal.SIGTERM)

    async def _probe_ready(self) -> None:
        deadline = time.monotonic() + READY_TIMEOUT_S
        last_err = "not started"
        while time.monotonic() < deadline:
            if self._backend and self._backend.returncode is not None:
                raise RuntimeError(f"backend exited early rc={self._backend.returncode}")
            # Soul pack load can take 20–60s; use a long per-attempt timeout so
            # a late response is not dropped as "orphaned" after a short wait.
            attempt_timeout = min(60.0, max(5.0, deadline - time.monotonic()))
            try:
                resp = await self._call_backend(
                    {
                        "jsonrpc": "2.0",
                        "id": "ready-probe",
                        "method": "initialize",
                        "params": {
                            "protocolVersion": "2024-11-05",
                            "capabilities": {},
                            "clientInfo": {"name": "cnet-mcp-shared", "version": "1.0"},
                        },
                    },
                    timeout=attempt_timeout,
                )
                if isinstance(resp, dict) and "result" in resp:
                    self._backend_ready.set()
                    _log("backend ready")
                    return
                last_err = f"unexpected probe response: {resp!r}"[:200]
            except Exception as exc:  # noqa: BLE001
                last_err = str(exc)
            await asyncio.sleep(0.5)
        raise TimeoutError(f"backend not ready within {READY_TIMEOUT_S}s: {last_err}")

    async def _backend_stderr_loop(self) -> None:
        try:
            await self._read_backend_stderr()
        except (ValueError, OSError):
            self._pipe_failed("stderr")

    async def _read_backend_stderr(self) -> None:
        assert self._backend and self._backend.stderr
        while True:
            line = await self._backend.stderr.readline()
            if not line:
                break
            text = line.decode("utf-8", "replace").rstrip()
            if text:
                _log(f"backend: {text}")

    async def _backend_stdout_loop(self) -> None:
        try:
            await self._read_backend_responses()
        except (ValueError, UnicodeError, OSError, RecursionError):
            pass
        # EOF is also terminal: the process may have closed stdout without
        # exiting, so waiting only for its exit would strand every client.
        self._pipe_failed("stdout")

    def _pipe_failed(self, stream: str) -> None:
        if not self._stopping:
            _log(f"backend {stream} unavailable or frame refused; stopping broker")
            os.kill(os.getpid(), signal.SIGTERM)

    async def _read_backend_responses(self) -> None:
        assert self._backend and self._backend.stdout
        while True:
            line = await self._backend.stdout.readline()
            if not line:
                break
            if not line.endswith(b"\n") or len(line) > MAX_RESPONSE_FRAME:
                raise ValueError("backend_frame_boundary")
            text = line.decode("utf-8").strip()
            if not text:
                continue
            msg = json.loads(text)
            if not isinstance(msg, dict):
                raise ValueError("backend_frame_object")
            mid = msg.get("id", None)
            key = self._id_key(mid)
            item = self._pending.pop(key, None)
            if item is None:
                _log(f"orphaned backend response id={mid!r}")
                continue
            writer, orig_id = item
            msg["id"] = orig_id
            try:
                await self._write_json(writer, msg)
            except OSError as exc:
                _log(f"client write failed: {exc}")

    @staticmethod
    def _id_key(mid: Any) -> str:
        return json.dumps(mid, separators=(",", ":"), ensure_ascii=False)

    async def _write_json(self, writer: asyncio.StreamWriter, obj: dict) -> None:
        data = (json.dumps(obj, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
        if len(data) > MAX_RESPONSE_FRAME:
            # Restoring a client's longer ID can grow an otherwise bounded
            # backend response. Return an explicit error, never a clipped JSON.
            error = {"jsonrpc": "2.0", "id": obj.get("id"),
                     "error": {"code": -32000, "message": "response_frame_exceeds_limit"}}
            data = (json.dumps(error, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
            if len(data) > MAX_RESPONSE_FRAME:
                raise ValueError("response_id_exceeds_limit")
        writer.write(data)
        await writer.drain()

    async def _call_backend(self, req: dict, timeout: float = 600.0) -> dict:
        """Send a request that expects a response; used for probe + serial path."""
        loop = asyncio.get_running_loop()
        fut: asyncio.Future = loop.create_future()
        orig_id = req.get("id")
        self._id_seq += 1
        internal_id = f"d{self._id_seq}"
        req = dict(req)
        req["id"] = internal_id
        key = self._id_key(internal_id)

        # Route response into our future via a fake writer shim.
        class _FutWriter:
            def __init__(self, f: asyncio.Future):
                self.f = f

            def write(self, data: bytes) -> None:
                if self.f.done():
                    return
                try:
                    msg = json.loads(data.decode("utf-8").strip())
                except Exception as exc:  # noqa: BLE001
                    self.f.set_exception(exc)
                    return
                self.f.set_result(msg)

            async def drain(self) -> None:
                return None

        shim = _FutWriter(fut)
        self._pending[key] = (shim, orig_id)  # type: ignore[arg-type]
        async with self._backend_lock:
            assert self._backend and self._backend.stdin
            payload = (json.dumps(req, ensure_ascii=False, separators=(",", ":")) + "\n").encode(
                "utf-8"
            )
            self._backend.stdin.write(payload)
            await self._backend.stdin.drain()
        try:
            resp = await asyncio.wait_for(fut, timeout=timeout)
        finally:
            self._pending.pop(key, None)
        if isinstance(resp, dict):
            resp["id"] = orig_id
        return resp  # type: ignore[return-value]

    async def forward_client_request(
        self, req: dict, client_writer: asyncio.StreamWriter
    ) -> None:
        method = req.get("method")
        has_id = "id" in req

        # Notifications: forward without expecting a response.
        if not has_id:
            async with self._backend_lock:
                assert self._backend and self._backend.stdin
                payload = (
                    json.dumps(req, ensure_ascii=False, separators=(",", ":")) + "\n"
                ).encode("utf-8")
                self._backend.stdin.write(payload)
                await self._backend.stdin.drain()
            return

        orig_id = req["id"]
        self._id_seq += 1
        internal_id = f"c{self._id_seq}"
        req = dict(req)
        req["id"] = internal_id
        key = self._id_key(internal_id)
        self._pending[key] = (client_writer, orig_id)

        try:
            async with self._backend_lock:
                assert self._backend and self._backend.stdin
                payload = (
                    json.dumps(req, ensure_ascii=False, separators=(",", ":")) + "\n"
                ).encode("utf-8")
                self._backend.stdin.write(payload)
                await self._backend.stdin.drain()
        except Exception:
            self._pending.pop(key, None)
            raise

        # compress_model can run a long time; no per-request wait here —
        # response is delivered asynchronously by _backend_stdout_loop.
        _ = method

    async def handle_client(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        peer = writer.get_extra_info("peername")
        _log(f"client connected {peer}")
        try:
            await asyncio.wait_for(self._backend_ready.wait(), timeout=READY_TIMEOUT_S)
            while True:
                line = await reader.readline()
                if not line:
                    break
                text = line.decode("utf-8", "replace").strip()
                if not text:
                    continue
                try:
                    req = json.loads(text)
                except json.JSONDecodeError:
                    err = {
                        "jsonrpc": "2.0",
                        "id": None,
                        "error": {"code": -32700, "message": "Parse error"},
                    }
                    await self._write_json(writer, err)
                    continue
                if not isinstance(req, dict):
                    continue
                try:
                    await self.forward_client_request(req, writer)
                except Exception as exc:  # noqa: BLE001
                    _log(f"forward failed: {exc}")
                    if "id" in req:
                        await self._write_json(
                            writer,
                            {
                                "jsonrpc": "2.0",
                                "id": req.get("id"),
                                "error": {
                                    "code": -32603,
                                    "message": f"shared daemon forward error: {exc}",
                                },
                            },
                        )
        finally:
            # Drop in-flight responses aimed at this client so they don't
            # attempt writes on a closed socket.
            dead = [k for k, (w, _) in self._pending.items() if w is writer]
            for k in dead:
                self._pending.pop(k, None)
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass
            _log(f"client disconnected {peer}")

    async def run(self) -> None:
        self.acquire_singleton()
        await self.start_backend()

        server = await asyncio.start_unix_server(self.handle_client, path=str(SOCK_PATH))
        try:
            os.chmod(SOCK_PATH, 0o600)
        except OSError:
            pass
        _log(f"listening on {SOCK_PATH}")

        stop_event = asyncio.Event()

        def _stop(*_args: Any) -> None:
            if not self._stopping:
                self._stopping = True
                _log("shutdown signal")
                stop_event.set()

        loop = asyncio.get_running_loop()
        for sig in (signal.SIGTERM, signal.SIGINT):
            try:
                loop.add_signal_handler(sig, _stop)
            except NotImplementedError:
                signal.signal(sig, lambda *_: _stop())

        await stop_event.wait()
        server.close()
        await server.wait_closed()
        await self.shutdown()

    async def shutdown(self) -> None:
        _log("shutting down")
        if self._backend and self._backend.returncode is None:
            try:
                self._backend.terminate()
                try:
                    await asyncio.wait_for(self._backend.wait(), timeout=10)
                except asyncio.TimeoutError:
                    self._backend.kill()
                    await self._backend.wait()
            except ProcessLookupError:
                pass
        if self._reader_task:
            self._reader_task.cancel()
        try:
            if SOCK_PATH.exists():
                SOCK_PATH.unlink()
        except OSError:
            pass
        try:
            if PID_PATH.exists():
                PID_PATH.unlink()
        except OSError:
            pass
        if self._lock_fd is not None:
            try:
                fcntl.flock(self._lock_fd, fcntl.LOCK_UN)
                os.close(self._lock_fd)
            except OSError:
                pass
        _log("shutdown complete")


# ── Ensure / client ─────────────────────────────────────────────────────


def ensure_daemon() -> None:
    """Start the shared daemon if the socket is not connectable."""
    _runtime_prep()
    if _socket_alive():
        return

    # Double-check after a short pause (another launcher may be racing).
    time.sleep(0.2)
    if _socket_alive():
        return

    _log("shared daemon not up — starting")
    # Prefer the installed user unit (survives Hermes session death).
    if os.path.exists("/usr/bin/systemctl"):
        os.system("systemctl --user reset-failed cnet-mcp-shared.service 2>/dev/null")
        rc = os.system("systemctl --user start cnet-mcp-shared.service 2>/dev/null")
        if rc == 0:
            deadline = time.monotonic() + ENSURE_WAIT_S
            while time.monotonic() < deadline:
                if _socket_alive():
                    _log("shared daemon is up (systemd)")
                    return
                time.sleep(0.25)
            _log("systemd unit started but socket not ready; falling back")

    log_f = open(LOG_PATH, "a", encoding="utf-8")
    # Detach fully so Hermes' MCP watchdog lifecycle does not reap us.
    asyncio.run(_spawn_detached_daemon(log_f))

    deadline = time.monotonic() + ENSURE_WAIT_S
    while time.monotonic() < deadline:
        if _socket_alive():
            _log("shared daemon is up")
            return
        time.sleep(0.25)
    raise SystemExit(
        f"timed out waiting for shared cnet-mcp daemon at {SOCK_PATH} "
        f"(see {LOG_PATH})"
    )


async def _spawn_detached_daemon(log_f) -> int:
    p = await asyncio.create_subprocess_exec(
        sys.executable,
        str(Path(__file__).resolve()),
        "daemon",
        stdout=log_f,
        stderr=log_f,
        start_new_session=True,
        cwd=str(SCRIPT_DIR),
    )
    # Parent does not wait — daemon holds flock.
    return p.pid


async def run_client() -> None:
    ensure_daemon()
    # Connect to shared daemon and bridge stdio ↔ socket (raw line proxy).
    reader, writer = await asyncio.open_unix_connection(str(SOCK_PATH), limit=MAX_RESPONSE_FRAME)

    async def stdin_to_sock() -> None:
        loop = asyncio.get_running_loop()
        # Read stdin in a thread so we don't block the event loop on Hermes.
        while True:
            line = await loop.run_in_executor(None, sys.stdin.buffer.readline)
            if not line:
                break
            writer.write(line)
            await writer.drain()
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass

    async def sock_to_stdout() -> None:
        while True:
            line = await reader.readline()
            if not line:
                break
            if not line.endswith(b"\n") or len(line) > MAX_RESPONSE_FRAME:
                raise ValueError("client_response_frame_boundary")
            sys.stdout.buffer.write(line)
            sys.stdout.buffer.flush()

    t1 = asyncio.create_task(stdin_to_sock())
    t2 = asyncio.create_task(sock_to_stdout())
    done, pending = await asyncio.wait({t1, t2}, return_when=asyncio.FIRST_COMPLETED)
    for t in pending:
        t.cancel()
    for t in done:
        exc = t.exception()
        if exc:
            raise exc


def stop_daemon() -> None:
    pid = _read_pid()
    if pid and _pid_alive(pid):
        os.kill(pid, signal.SIGTERM)
        for _ in range(50):
            if not _pid_alive(pid):
                break
            time.sleep(0.1)
        if _pid_alive(pid):
            os.kill(pid, signal.SIGKILL)
    # Also stop transient systemd unit if present
    os.system("systemctl --user stop cnet-mcp-shared.service 2>/dev/null")
    for p in (SOCK_PATH, PID_PATH):
        try:
            p.unlink()
        except OSError:
            pass
    print("stopped", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description="CNET MCP shared singleton host")
    ap.add_argument(
        "mode",
        choices=("daemon", "ensure", "client", "status", "stop"),
        help="daemon=run shared host; client=stdio bridge; ensure=start if needed",
    )
    args = ap.parse_args()
    if args.mode == "daemon":
        try:
            asyncio.run(SharedDaemon().run())
        except SystemExit:
            raise
        except Exception as exc:  # noqa: BLE001
            _log(f"daemon fatal: {exc}")
            raise SystemExit(1) from exc
    elif args.mode == "ensure":
        ensure_daemon()
    elif args.mode == "client":
        try:
            asyncio.run(run_client())
        except BrokenPipeError:
            raise SystemExit(0)
        except Exception as exc:  # noqa: BLE001
            print(f"[cnet-mcp-shared] client error: {exc}", file=sys.stderr)
            raise SystemExit(1) from exc
    elif args.mode == "status":
        sys.stdout.write(status_text())
    elif args.mode == "stop":
        stop_daemon()


if __name__ == "__main__":
    main()
