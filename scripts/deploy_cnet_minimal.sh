#!/usr/bin/env bash
# Explicit offline artifact handoff. No live services, data merge or link switch.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Python is a deployment-host validator dependency, not a package runtime.
exec /usr/bin/python3 -I -B - "$ROOT" "$@" <<'PY'
import argparse
import ctypes
import hashlib
import os
import re
import secrets
import selectors
import signal
import stat
import subprocess
import sys
import tarfile
import time
import zlib

MAX_ARCHIVE = 32 * 1024 * 1024
MAX_EXPANDED = 128 * 1024 * 1024
MAX_FILE = 32 * 1024 * 1024
MAX_MEMBERS = 4096
MAX_OUTPUT = 256 * 1024
SMOKE_SECONDS = 60
COMMIT_SIGNALS = {signal.SIGALRM, signal.SIGINT, signal.SIGTERM}
COMMITTED_DESTINATION = None
DIRECTORY = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
REGULAR = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC
BINARIES = {"roe_daily_packs_seed", "roe_front_door", "roe_domain_route", "roe_chain_think",
            "roe_evolve_tick", "roe_gold_put", "stream_ix_e2e_bench"}
BIN = BINARIES | {"cnet-ask"}
CONFIG = {"domain_routes.tsv", "promote_blocklist.txt", "probe_shortcircuit.txt",
          "query_aliases.tsv", "utterance_phrases.tsv", "coverage_gold.tsv"}
SCRIPTS = {"cnet_runtime_common.sh", "cnet_runtime_smoke.sh", "cnet_runtime_soak_gate.sh"}
TOP_FILES = {"INSTALL.md", "harvest.log", "smoke.log", "VERSION", "BUILD_STATUS", "MANIFEST.sha256"}
TOP_DIRS = {"bin", "config", "scripts", "data"}


class Refused(Exception):
    pass


def require(condition, reason):
    if not condition:
        raise Refused(reason)


def component(name, *, hidden=True):
    return (0 < len(name) <= 96 and name not in (".", "..") and
            (hidden or not name.startswith(".")) and re.fullmatch(r"[A-Za-z0-9_.-]+", name))


def absolute(path):
    require(isinstance(path, str) and 1 < len(path) <= 4095 and path.startswith("/"), "absolute_path_required")
    require(all(component(p) for p in path[1:].split("/")), "unsafe_path_component")
    return path


def open_directory(path, *, private=False):
    absolute(path)
    fd = os.open("/", DIRECTORY)
    try:
        for part in path[1:].split("/"):
            if private:
                refuse_workspace(fd)
            parent = os.fstat(fd)
            require(parent.st_uid in (0, os.geteuid()), "path_ancestor_owner_mismatch")
            require(not (parent.st_mode & 0o022) or parent.st_mode & stat.S_ISVTX,
                    "shared_writable_path_ancestor")
            child = os.open(part, DIRECTORY, dir_fd=fd)
            os.close(fd)
            fd = child
        info = os.fstat(fd)
        if private:
            refuse_workspace(fd)
        require(info.st_uid == os.geteuid(), "directory_owner_mismatch")
        require(not private or not (info.st_mode & 0o077), "private_destination_parent_required")
        return fd
    except BaseException:
        os.close(fd)
        raise


def refuse_workspace(directory):
    # Ancestor metadata only; never follow or traverse the checkout marker.
    try:
        os.stat(".git", dir_fd=directory, follow_symlinks=False)
    except FileNotFoundError:
        return
    raise Refused("workspace_ancestor_refused")


def stable(a, b):
    return (a.st_dev, a.st_ino, a.st_mode, a.st_uid, a.st_nlink, a.st_size, a.st_mtime_ns, a.st_ctime_ns) == (
        b.st_dev, b.st_ino, b.st_mode, b.st_uid, b.st_nlink, b.st_size, b.st_mtime_ns, b.st_ctime_ns)


def read_artifact(path):
    absolute(path)
    parent, leaf = path.rsplit("/", 1)
    directory = open_directory(parent)
    try:
        fd = os.open(leaf, REGULAR, dir_fd=directory)
    finally:
        os.close(directory)
    try:
        before = os.fstat(fd)
        require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1 and before.st_uid == os.geteuid(),
                "artifact_must_be_owner_regular_single_link")
        require(0 < before.st_size <= MAX_ARCHIVE, "archive_size_limit")
        chunks, remaining = [], before.st_size
        while remaining:
            chunk = os.read(fd, min(remaining, 1024 * 1024))
            require(bool(chunk), "archive_truncated")
            chunks.append(chunk)
            remaining -= len(chunk)
        require(not os.read(fd, 1) and stable(before, os.fstat(fd)), "archive_changed_during_read")
        return b"".join(chunks)
    finally:
        os.close(fd)


def unpack_checked(compressed):
    decoder = zlib.decompressobj(31)
    raw = decoder.decompress(compressed, MAX_EXPANDED + 1)
    require(len(raw) <= MAX_EXPANDED and decoder.eof and not decoder.unconsumed_tail and not decoder.unused_data,
            "gzip_expansion_truncation_or_trailing_stream")
    require(len(raw) % 512 == 0, "tar_block_alignment")
    entries, offset, root, longname = {}, 0, None, None
    # Parse physical headers, not extractall: PAX/sparse/link metadata is never
    # silently interpreted. Only a bounded GNU long-name header is supported.
    while offset + 512 <= len(raw) and raw[offset:offset + 512] != bytes(512):
        require(len(entries) < MAX_MEMBERS, "archive_member_limit")
        item = tarfile.TarInfo.frombuf(raw[offset:offset + 512], "ascii", "strict")
        require(0 <= item.size <= MAX_FILE and not item.linkname and not (item.mode & 0o7000),
                "archive_member_metadata")
        begin = offset + 512
        offset = begin + ((item.size + 511) // 512) * 512
        require(offset <= len(raw), "archive_member_truncated")
        contents = memoryview(raw)[begin:begin + item.size]
        if item.type == tarfile.GNUTYPE_LONGNAME:
            require(longname is None and 1 < item.size <= 2048, "gnu_long_name_bounds")
            text = bytes(contents)
            require(text[-1:] == b"\0" and b"\0" not in text[:-1], "gnu_long_name_encoding")
            longname = text[:-1].decode("ascii")
            continue
        require(item.type in (tarfile.REGTYPE, tarfile.AREGTYPE, tarfile.DIRTYPE), "archive_member_type_refused")
        name, longname = longname or item.name, None
        directory = item.type == tarfile.DIRTYPE
        if directory and name.endswith("/"):
            name = name[:-1]
        pieces = name.split("/")
        require(len(name) <= 2047 and 1 <= len(pieces) <= 20 and all(component(p) for p in pieces),
                "archive_path_refused")
        require(component(pieces[0], hidden=False), "archive_root_refused")
        if root is None:
            require(directory and len(pieces) == 1, "archive_root_must_be_first_directory")
            root = pieces[0]
        require(pieces[0] == root, "multiple_archive_roots")
        relative = "/".join(pieces[1:])
        require(relative not in entries, "duplicate_archive_path")
        require(not directory or item.size == 0, "directory_payload_refused")
        if relative:
            parent = relative.rpartition("/")[0]
            require(parent in entries and entries[parent][0], "missing_or_non_directory_parent")
        entries[relative] = (directory, contents, item.mode)
    require(root and longname is None and len(raw) - offset >= 1024 and not any(raw[offset:]),
            "tar_end_or_trailing_payload")
    validate_package(entries)
    return entries


def validate_package(entries):
    require({p for p in entries if p and "/" not in p} == TOP_FILES | TOP_DIRS, "unexpected_package_layout")
    for path in TOP_DIRS:
        require(entries[path][0], "package_directory_type")
    for path in TOP_FILES:
        require(not entries[path][0], "package_file_type")
    require({p[4:] for p in entries if p.startswith("bin/")} == BIN, "unexpected_binary_inventory")
    require({p[7:] for p in entries if p.startswith("config/")} == CONFIG, "unexpected_config_inventory")
    require({p[8:] for p in entries if p.startswith("scripts/")} == SCRIPTS, "unexpected_script_inventory")
    for directory, inventory in (("bin", BIN), ("config", CONFIG), ("scripts", SCRIPTS)):
        require(all(not entries[directory + "/" + name][0] for name in inventory), "required_member_not_regular")
    require("data/roe_daily_packs" in entries and entries["data/roe_daily_packs"][0], "missing_pack_root")
    for path, (directory, content, mode) in entries.items():
        if path.startswith("data/"):
            require(path == "data/roe_daily_packs" or path.startswith("data/roe_daily_packs/"), "unexpected_data_root")
        if directory:
            continue
        executable = path.startswith("bin/") or path.startswith("scripts/")
        require(bool(mode & 0o100) == executable, "package_executable_mode")
        if path.startswith("bin/") and path[4:] in BINARIES:
            require(bytes(content[:4]) == b"\x7fELF", "native_elf_required")
    for path in ("data/roe_daily_packs/INDEX.json", "data/roe_daily_packs/ROUTES.jsonl"):
        require(path in entries and not entries[path][0], "missing_required_pack_index")
    require(bytes(entries["BUILD_STATUS"][1]) == b"complete\n", "incomplete_package")
    version = bytes(entries["VERSION"][1]).decode("ascii")
    require(re.fullmatch(r"name=CNET-Minimal\nversion=[A-Za-z0-9][A-Za-z0-9._-]*\ngit=[0-9a-f]{40}\n"
                         r"fixture_only=1\nlive_calls=0\nnever_self_cert=1\n", version), "unsupported_package_version_record")
    manifest = bytes(entries["MANIFEST.sha256"][1])
    require(len(manifest) <= 1024 * 1024 and manifest.endswith(b"\n"), "manifest_bounds")
    hashes = {}
    for line in manifest.splitlines(keepends=True):
        match = re.fullmatch(rb"([0-9a-f]{64})  \./([A-Za-z0-9_./-]+)\n", line)
        require(match is not None, "manifest_record_syntax")
        digest, path = match[1].decode(), match[2].decode()
        require(path not in hashes and path != "MANIFEST.sha256", "duplicate_or_self_manifest_entry")
        require(path in entries and not entries[path][0], "manifest_path_not_regular_member")
        hashes[path] = digest
    require(set(hashes) == {p for p, v in entries.items() if not v[0]} - {"MANIFEST.sha256"},
            "manifest_missing_or_extra_files")
    for path, digest in hashes.items():
        require(hashlib.sha256(entries[path][1]).hexdigest() == digest, "manifest_digest_mismatch")


def descend(root, parts):
    fd = os.dup(root)
    try:
        for part in parts:
            child = os.open(part, DIRECTORY, dir_fd=fd)
            os.close(fd)
            fd = child
        return fd
    except BaseException:
        os.close(fd)
        raise


def write_stage(root, entries):
    for path, (directory, content, mode) in entries.items():
        if not path:
            continue
        pieces = path.split("/")
        parent = descend(root, pieces[:-1])
        try:
            if directory:
                os.mkdir(pieces[-1], 0o700, dir_fd=parent)
            else:
                fd = os.open(pieces[-1], os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
                             0o600, dir_fd=parent)
                try:
                    remaining = content
                    while remaining:
                        written = os.write(fd, remaining)
                        require(written > 0, "stage_write_failed")
                        remaining = remaining[written:]
                    if path.startswith("bin/") or path.startswith("scripts/"):
                        os.fchmod(fd, 0o700)
                    os.fsync(fd)
                finally:
                    os.close(fd)
            os.fsync(parent)
        finally:
            os.close(parent)
    os.fsync(root)


def verify_stage(root, entries):
    root_info = os.fstat(root)
    require(stat.S_ISDIR(root_info.st_mode) and root_info.st_uid == os.geteuid() and
            stat.S_IMODE(root_info.st_mode) == 0o700, "stage_root_no_longer_private")
    seen = set()
    def visit(directory, prefix):
        for name in os.listdir(directory):
            path = prefix + name
            require(path in entries, "stage_unexpected_file")
            seen.add(path)
            info = os.stat(name, dir_fd=directory, follow_symlinks=False)
            require(info.st_uid == os.geteuid() and not (info.st_mode & 0o077), "stage_ownership_or_mode_changed")
            if entries[path][0]:
                require(stat.S_ISDIR(info.st_mode) and stat.S_IMODE(info.st_mode) == 0o700, "stage_directory_changed")
                child = os.open(name, DIRECTORY, dir_fd=directory)
                try:
                    visit(child, path + "/")
                finally:
                    os.close(child)
            else:
                require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and info.st_size == len(entries[path][1]),
                        "stage_regular_file_changed")
                fd = os.open(name, REGULAR, dir_fd=directory)
                try:
                    before = os.fstat(fd)
                    expected_mode = 0o700 if entries[path][2] & 0o100 else 0o600
                    require(stable(info, before) and stat.S_IMODE(before.st_mode) == expected_mode,
                            "stage_descriptor_or_execution_mode_changed")
                    digest = hashlib.sha256()
                    remaining = before.st_size
                    while remaining:
                        chunk = os.read(fd, min(remaining, 1024 * 1024))
                        require(bool(chunk), "stage_read_truncated")
                        remaining -= len(chunk)
                        digest.update(chunk)
                    require(not os.read(fd, 1) and stable(before, os.fstat(fd)), "stage_changed_during_read")
                    require(digest.digest() == hashlib.sha256(entries[path][1]).digest(), "stage_checksum_changed")
                finally:
                    os.close(fd)
    visit(root, "")
    require(seen == set(entries) - {""}, "stage_member_missing")


def smoke(root):
    # The externally approved digest is the authorization to run packaged code.
    # Manifest checks do not make arbitrary executables trustworthy.
    environment = {"PATH": "/usr/bin:/bin", "LC_ALL": "C", "CNET_MINIMAL_ROOT": root,
                   "ROE_LIVE": "0", "ROE_LLM": "0", "ROE_LOOKUP": "0", "CNET_OPEN_CHAT": "0",
                   "ROE_OPEN_CHAT": "0", "ROE_NO_THOUGHT": "1", "ROE_EVOLVE_TEACHER": "0"}
    process = subprocess.Popen(["/bin/bash", root + "/scripts/cnet_runtime_smoke.sh"], cwd=root,
                               env=environment, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, start_new_session=True, close_fds=True)
    output = bytearray()
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            deadline = time.monotonic() + SMOKE_SECONDS
            while selector.get_map():
                require(time.monotonic() < deadline, "native_smoke_timeout")
                for key, _ in selector.select(min(0.2, max(0, deadline - time.monotonic()))):
                    chunk = os.read(key.fileobj.fileno(), 16384)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    output.extend(chunk)
                    require(len(output) <= MAX_OUTPUT, "native_smoke_output_limit")
        # Observe exit without reaping: the child's PID must not be reusable
        # before our final process-group signal, even when stdout closes first.
        while True:
            require(time.monotonic() < deadline, "native_smoke_timeout")
            exited = os.waitid(os.P_PID, process.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
            if exited is not None:
                require(exited.si_code == os.CLD_EXITED and exited.si_status == 0, "native_smoke_exit_failed")
                break
            time.sleep(0.01)
        text = output.decode("utf-8", "strict")
        require(any(line.startswith("CNET_RUNTIME_SMOKE_PASS fixture_only=1 local=9 ood=4 native_dry_run=1 ")
                    for line in text.splitlines()), "native_smoke_receipt_missing")
        print(text, end="" if text.endswith("\n") else "\n")
    finally:
        # Group kill closes descendants' pipes; wait reaps the direct child.
        try:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        finally:
            try:
                process.wait(timeout=5)
            finally:
                process.stdout.close()


def remove_stage(parent, name, root):
    # Only our exclusively created stage, through pinned descriptors. No glob,
    # rmtree, recursive shell command, or deletion of an existing deployment.
    def empty(directory):
        for leaf in os.listdir(directory):
            info = os.stat(leaf, dir_fd=directory, follow_symlinks=False)
            if stat.S_ISDIR(info.st_mode):
                child = os.open(leaf, DIRECTORY, dir_fd=directory)
                try:
                    empty(child)
                finally:
                    os.close(child)
                os.rmdir(leaf, dir_fd=directory)
            else:
                os.unlink(leaf, dir_fd=directory)
    empty(root)
    os.rmdir(name, dir_fd=parent)


def main():
    global COMMITTED_DESTINATION
    COMMITTED_DESTINATION = None
    workspace = sys.argv.pop(1)
    parser = argparse.ArgumentParser(description="Verify an owner-trusted native artifact into a NEW private directory; never switch live state.")
    parser.add_argument("--artifact", required=True)
    parser.add_argument("--destination", required=True)
    parser.add_argument("--sha256", required=True, help="expected archive digest obtained separately from a trusted source")
    parser.add_argument("--dry-run", action="store_true", help="validate bytes/paths/manifest only: no extraction or code execution")
    options = parser.parse_args()
    require(sys.platform == "linux", "linux_handoff_required")
    require(re.fullmatch(r"[0-9a-fA-F]{64}", options.sha256), "trusted_sha256_required")
    destination = absolute(options.destination)
    require(destination != os.path.expanduser("~") and destination != workspace and not destination.startswith(workspace + "/"),
            "home_or_workspace_destination_refused")
    parent_path, leaf = destination.rsplit("/", 1)
    require(component(leaf, hidden=False), "destination_leaf_refused")
    parent = open_directory(parent_path, private=True)
    try:
        try:
            os.stat(leaf, dir_fd=parent, follow_symlinks=False)
        except FileNotFoundError:
            pass
        else:
            raise Refused("destination_exists")
        compressed = read_artifact(options.artifact)
        digest = hashlib.sha256(compressed).hexdigest()
        require(digest == options.sha256.lower(), "archive_trusted_digest_mismatch")
        entries = unpack_checked(compressed)
        if options.dry_run:
            print(f"CNET_MINIMAL_DEPLOY_DRY_RUN_PASS archive_sha256={digest} members={len(entries)} writes=0 executes=0 live_switch=0")
            return 0
        libc = ctypes.CDLL(None, use_errno=True)
        rename = getattr(libc, "renameat2", None)
        require(rename is not None, "atomic_no_replace_unavailable")
        rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        rename.restype = ctypes.c_int
        stage = ".cnet-deploy-" + secrets.token_hex(16)
        os.mkdir(stage, 0o700, dir_fd=parent)
        stage_fd = os.open(stage, DIRECTORY, dir_fd=parent)
        published = False
        try:
            write_stage(stage_fd, entries)
            verify_stage(stage_fd, entries)
            # Descendants access this helper's pinned descriptor, without
            # resolving a potentially renamed staging directory again.
            smoke(f"/proc/{os.getpid()}/fd/{stage_fd}/.")
            verify_stage(stage_fd, entries)
            os.fsync(stage_fd)
            # Do not let handled signals interrupt rename/commit bookkeeping
            # interval and mistake a completed publication for removable work.
            old_mask = signal.pthread_sigmask(signal.SIG_BLOCK, COMMIT_SIGNALS)
            try:
                require(rename(parent, os.fsencode(stage), parent, os.fsencode(leaf), 1) == 0,
                        "destination_publication_conflict_or_failure")
                published = True
                COMMITTED_DESTINATION = destination
            finally:
                if not published:
                    signal.pthread_sigmask(signal.SIG_SETMASK, old_mask)
            try:
                signal.pthread_sigmask(signal.SIG_SETMASK, old_mask)
                os.fsync(stage_fd)
                os.fsync(parent)
                print(f"CNET_MINIMAL_DEPLOY_PASS destination={destination} archive_sha256={digest} fixture_only=1 live_switch=0")
                return 0
            except (OSError, Refused, KeyboardInterrupt):
                return uncertain(destination)
        finally:
            if not published:
                remove_stage(parent, stage, stage_fd)
            os.close(stage_fd)
    finally:
        os.close(parent)


def deadline(_signum, _frame):
    raise Refused("handoff_deadline")


def interrupted(signum, _frame):
    raise Refused("handoff_interrupted_signal_" + str(signum))


def uncertain(destination):
    print(f"CNET_MINIMAL_DEPLOY_COMMIT_UNCERTAIN destination={destination} retained=1 live_switch=0", file=sys.stderr)
    return 3


signal.signal(signal.SIGALRM, deadline)
signal.signal(signal.SIGINT, interrupted)
signal.signal(signal.SIGTERM, interrupted)
signal.alarm(120)
try:
    sys.exit(main())
except (Refused, OSError, ValueError, UnicodeError, tarfile.HeaderError, zlib.error, subprocess.SubprocessError, KeyboardInterrupt) as error:
    # Includes a handled interrupt during descriptor finalization, after the
    # inner post-publication block has already completed.
    if COMMITTED_DESTINATION is not None:
        sys.exit(uncertain(COMMITTED_DESTINATION))
    print("CNET_MINIMAL_DEPLOY_REFUSED " + str(error), file=sys.stderr)
    sys.exit(1)
PY
