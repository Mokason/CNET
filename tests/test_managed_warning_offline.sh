#!/bin/sh
# The offline claim, measured at the syscall boundary rather than argued.
#
# WHY THIS EXISTS. A review refused the offline claim the managed warning gate
# made on `--source <empty dir>` alone, and it was right to. NuGet's
# `auditSources` is a separate configuration section that `--source` does not
# cover, and NuGetAudit reaches it during restore. Measured at cb240b5, with a
# user NuGet.Config declaring an audit source on a local HTTP endpoint:
#
#   error NU1900: Error occurred while getting package vulnerability data:
#   Unable to load the service index for source http://127.0.0.1:28081/v3/index.json
#   strace -f -e trace=network:  6 connect() to AF_INET6 ::ffff:127.0.0.1:28081
#
# `tests/test_managed_warning_prereq.sh` proves the FIX is applied -- that every
# restore is handed an isolated config which clears packageSources and
# auditSources and names only an empty local directory. This file proves what
# that fix BUYS, by running the real gate against real projects under a hostile
# user configuration and reading the syscalls:
#
#   * the gate exits 0 and all seven projects restore and build from the
#     machine's existing package cache;
#   * zero connect() to AF_INET or AF_INET6;
#   * zero send/sendto/sendmsg carrying an AF_INET or AF_INET6 address;
#   * zero further network syscalls on ANY descriptor that was created as an
#     AF_INET/AF_INET6 socket -- so a socket that is merely opened and closed
#     cannot be mistaken for traffic, and traffic cannot hide behind one.
#
# AF_UNIX is local IPC (MSBuild worker pipes, the dotnet host) and is counted
# separately and deliberately ignored: mistaking it for feed access would make
# this test fail for a reason that has nothing to do with the network.
#
# This is an EVIDENCE lane, not a ci_core gate: it needs strace, and the
# property it confirms is already enforced in ci_core by the prereq harness.
# It fails closed rather than skipping when its tools are missing.
#
# Usage: sh tests/test_managed_warning_offline.sh
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
STRACE=${CNET_STRACE:-/usr/bin/strace}
TIMEOUT=${CNET_OFFLINE_PROOF_TIMEOUT:-900}
PROJECTS=7

if [ ! -x "$STRACE" ]; then
    echo "MANAGED_WARNING_OFFLINE_PROOF_BLOCKED reason=no_strace path=$STRACE"
    exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/cnet-offline-proof-XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

# A user config that WOULD reach the network if anything inherited it: a package
# feed and an audit source, both on local HTTP endpoints nothing is listening on,
# so any contact shows up as a connect() and as a NuGet error rather than as a
# silent success.
mkdir -p "$work/home/.nuget/NuGet"
cat > "$work/home/.nuget/NuGet/NuGet.Config" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources>
    <add key="hostile-feed" value="http://127.0.0.1:28080/v3/index.json" protocolVersion="3" />
  </packageSources>
  <auditSources>
    <add key="hostile-audit" value="http://127.0.0.1:28081/v3/index.json" protocolVersion="3" />
  </auditSources>
</configuration>
XML

echo "OFFLINE_PROOF hostile_config=$work/home/.nuget/NuGet/NuGet.Config"

# NUGET_PACKAGES keeps the real package cache reachable while HOME is hostile:
# the point is an isolated CONFIGURATION, not an empty machine. It is resolved
# HERE, not in the assignment list below -- POSIX performs those assignments in
# order and lets later ones see earlier ones, so a `~` expanded there would have
# resolved against the hostile HOME and quietly measured a restore that found
# nothing, which is not the experiment.
real_packages=${CNET_NUGET_PACKAGES:-$HOME/.nuget/packages}
if [ ! -d "$real_packages" ]; then
    echo "MANAGED_WARNING_OFFLINE_PROOF_BLOCKED reason=no_package_cache path=$real_packages"
    exit 1
fi
echo "OFFLINE_PROOF package_cache=$real_packages entries=$(ls "$real_packages" | wc -l)"

# Node reuse and the MSBuild server are disabled so no worker outlives the trace.
HOME="$work/home" \
NUGET_PACKAGES="$real_packages" \
DOTNET_CLI_TELEMETRY_OPTOUT=1 \
DOTNET_NOLOGO=1 \
DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 \
MSBUILDDISABLENODEREUSE=1 \
DOTNET_CLI_USE_MSBUILD_SERVER=0 \
    timeout "$TIMEOUT" "$STRACE" -f -qq -e trace=network -o "$work/strace.txt" \
    make -C "$ROOT" managed_warning_gate > "$work/gate.log" 2>&1
gate_rc=$?

# `grep -c` prints 0 AND exits 1 when nothing matches, so a `|| echo 0` fallback
# emits the count twice and every comparison against it is false.
count() { n=$(grep -cE "$1" "$2" 2>/dev/null) || n=0; echo "$n"; }
restores=$(count '^restoring' "$ROOT/logs/managed_warning_restore.log")
builds=$(count 'Build succeeded' "$ROOT/logs/managed_warning_gate.log")
audit_errors=$(count 'NU1900|NU1905' "$ROOT/logs/managed_warning_restore.log")

inet_connect=$(grep -cE 'connect\([0-9]+, \{sa_family=AF_INET6?,' "$work/strace.txt")
inet_send=$(grep -cE '(send|sendto|sendmsg)\([0-9]+,.*sa_family=AF_INET6?,' "$work/strace.txt")
unix_connect=$(grep -cE 'connect\([0-9]+, \{sa_family=AF_UNIX' "$work/strace.txt")
inet_sockets=$(grep -cE 'socket\(AF_INET6?,' "$work/strace.txt")

# Every descriptor ever returned by socket(AF_INET*) ... and then every other
# network syscall on one of them. An opened-and-closed probe socket is not
# traffic; a send on one would be.
sed -n 's/.*socket(AF_INET6\?,.*= \([0-9]\+\)$/\1/p' "$work/strace.txt" | sort -u > "$work/inet_fds"
inet_fd_uses=0
while IFS= read -r fd; do
    [ -n "$fd" ] || continue
    n=$(grep -cE "^[0-9]+ +(bind|connect|listen|accept4?|send|sendto|sendmsg|recv|recvfrom|recvmsg|getsockopt|setsockopt|getpeername|getsockname)\($fd," "$work/strace.txt")
    inet_fd_uses=$((inet_fd_uses + n))
done < "$work/inet_fds"

cp "$work/strace.txt" "${CNET_OFFLINE_PROOF_STRACE:-/dev/null}" 2>/dev/null || :

echo "OFFLINE_PROOF gate_rc=$gate_rc restores=$restores builds=$builds audit_errors=$audit_errors"
echo "OFFLINE_PROOF inet_connect=$inet_connect inet_send=$inet_send inet_sockets_opened=$inet_sockets inet_fd_further_use=$inet_fd_uses unix_connect_ignored=$unix_connect"

failures=0
fail() { failures=$((failures + 1)); echo "FAIL: $1"; }

[ "$gate_rc" = "0" ] || fail "the gate must pass under a hostile user config (rc=$gate_rc)
$(tail -20 "$work/gate.log")"
[ "$restores" = "$PROJECTS" ] || fail "all $PROJECTS projects must restore (saw $restores)"
[ "$builds" = "$PROJECTS" ] || fail "all $PROJECTS projects must build (saw $builds)"
[ "$audit_errors" = "0" ] || fail "no audit source may be consulted (saw $audit_errors NU1900/NU1905)"
[ "$inet_connect" = "0" ] || fail "zero AF_INET/AF_INET6 connect() (saw $inet_connect)"
[ "$inet_send" = "0" ] || fail "zero AF_INET/AF_INET6 send (saw $inet_send)"
[ "$inet_fd_uses" = "0" ] || fail "an AF_INET socket was used for something (saw $inet_fd_uses further network syscalls on inet descriptors)"

if [ "$failures" != "0" ]; then
    echo "MANAGED_WARNING_OFFLINE_PROOF_FAIL failures=$failures strace=$work/strace.txt"
    cp "$work/strace.txt" "$ROOT/logs/managed_warning_offline_strace.txt" 2>/dev/null || :
    exit 1
fi
cp "$work/strace.txt" "$ROOT/logs/managed_warning_offline_strace.txt" 2>/dev/null || :
echo "MANAGED_WARNING_OFFLINE_PROOF_PASS projects=$PROJECTS restores=$restores \
builds=$builds inet_connect=0 inet_send=0 inet_fd_further_use=0 \
inet_sockets_opened=$inet_sockets unix_connect_ignored=$unix_connect"
exit 0
