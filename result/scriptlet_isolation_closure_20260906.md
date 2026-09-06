# Scriptlet isolation closure — 2026-09-06

Status: Linux isolation implemented; focused verification 49/49 PASS and
framework-dependent Debug publish PASS. Independent review findings repaired
and re-reviewed. No cross-platform or live-deployment success claimed.

## Measured feasibility

- Linux 6.17.0-20-generic x86-64; .NET SDK 10.0.203, runtime 10.0.7.
- Bubblewrap 0.9.0 refuses a private `/usr/bin/true` sandbox:
  `loopback: Failed RTM_NEWADDR: Operation not permitted`; omitting the network
  namespace also refuses with `setting up uid map: Permission denied`.
- A private native probe reports Landlock ABI 7 and successful seccomp TSYNC.
  No host configuration, permissions, packages, or services were changed.

## Reviewed implementation contract

The supported target is an unprivileged Linux x86-64/glibc process, standard
framework-dependent .NET 10 layout, Landlock ABI >= 7, and seccomp TSYNC. Missing
isolation, unsupported OS/architecture, root/set-ID/capability-bearing identity,
or missing runtime dependencies refuse before source compilation or execution.
The native launcher checks real/effective/saved UID/GID, supplementary groups,
and both words of permitted/effective/inheritable Linux capabilities; the seal
checks identity again. Neither `no_new_privs` nor CLR reference filtering is
treated as privilege removal.

The trusted native launcher applies Landlock before .NET starts, so every CLR
thread inherits the filesystem restriction. Read grants cover only the private
worker payload, exact .NET runtime and host resolver directories, dotnet
executable, ten enumerated system libraries, loader cache, entropy device, and
the current process's pinned `/proc/self/maps` inode. No broad home, temporary,
system-library or proc directory is granted. No filesystem writes are granted.
All inherited descriptors >= 3 are closed; environment is cleared and replaced
with fixed runtime controls, including `DOTNET_EnableDiagnostics=0`.

Before reading any request, the trusted worker preloads an explicit bounded
framework/Roslyn dependency list, warms crypto/console and two pool threads,
then synchronizes seccomp onto every CLR thread. This permits lazy runtime
operations without exposing path metadata syscalls to untrusted source. The
filter checks native x86-64 ABI and denies x32/other ABIs. It denies sockets,
fork/exec and all new threads, `O_PATH`, path-stat/access/readlink/directory
enumeration, external signals and process memory, ioctl, pidfds, namespace and
privilege changes, io_uring, and new anonymous backing files. `madvise`, fcntl,
prlimit, membarrier and tgkill arguments are restricted; harmful memory advice
is not allowed. Existing CLR JIT backing memory and private pipes remain
available under resource limits. The worker reads/parses no source and compiles
or loads no source-generated assembly before the seal. The host guard performs
only bounded syntax-policy parsing, not compilation or execution.

The host sends bounded source and input only to that sealed worker and accepts
one bounded response followed by EOF and a zero exit. Failures, malformed
responses and timeouts kill and reap the worker, observing both pipe tasks.
Reap failure is loud, never reported as a successful refusal. The method-body
guard remains an additional host policy check; its syntax denylist and Roslyn
references are not isolation boundaries.

The independently built worker does not reference CCE, server configuration,
tool registries or capsule data. Seven explicit artifact digests are embedded
in the trusted CCE assembly. Deployment artifacts are opened nofollow and
nonblocking, checked as regular files using the same descriptor, read with a
fixed cap, hash-verified and copied into a private 0700 directory. Copied files
are 0400 (launcher 0500). Request data cannot select executable paths, mounts,
environment or flags. Integrity is relative to the trusted host/build/runtime;
it is not authentication against a compromised same-UID owner or kernel.

## Enforced resource contract

| Resource | Bound |
|---|---|
| Concurrent invocations | 2; excess refuses immediately |
| Source / input / output | 4000 / 8192 / 8192 UTF-16 characters |
| Request frame / diagnostic output | 65536 / 1024 bytes |
| Execution | Caller 1..10000 ms, default 200 ms, after READY |
| Startup and isolated compilation | Separate 5000 ms deadline |
| Worker address space / GC heap | 2 GiB / 128 MiB per job |
| CPU / descriptors / locked memory | 10 CPU seconds / 256 / zero |
| CLR anonymous JIT backing file | 64 MiB; new memfd denied after seal |
| Artifact read | 32 MiB per file, exactly seven files |

The memory claim is bounded address space, managed heap, descriptor count and
anonymous backing; it is not an ineffective `RLIMIT_RSS` claim or a cgroup RSS
measurement. Two hostile jobs can use a substantial part of these ceilings;
kernel bookkeeping and the trusted host process are additional memory. No
micro-VM, side-channel freedom, filesystem-existence secrecy, general .NET
sandbox, self-contained/AOT packaging or other Linux ABI is claimed. Kernel
and trusted filesystem availability remain operating assumptions.

## API migration and cost

`ScriptletCompiler.TryCompile` validates source and returns an invocation
closure bound to source, never an assembly loaded in the host. Arbitrary
delegates passed to `ScriptletSandbox.TryRun` refuse without invocation;
source-bound closures preserve the existing pure transform/registry call shape.
Contract examples execute in the same sandbox as later invocations. Only the
tested Linux contract may be claimed.

New callers should pass source directly:

```csharp
bool accepted = ScriptletSandbox.TryRunSource(
    "return input.ToUpperInvariant();", "hello", out string output);
```

There is no arbitrary-delegate serialization and no in-process compatibility
fallback. Applications needing custom privileged functions must keep them
outside the untrusted-scriptlet API. Stored scriptlet source and contracts keep
their existing format and are re-certified; no knowledge package format changes.

This is not unchanged performance. A measured cold isolated timeout call took
1688 ms, including new process/runtime/compilation cost before its 150 ms
execution budget. The former <1500 ms total-time assertion therefore changed
to the documented startup budget plus execution/reap allowance. A separate
test measures the post-READY 150 ms timeout and requires completion/reaping in
100..1000 ms; execution and certification floors were not weakened.

## Verification evidence

The initial production RED executed two tests and failed both: public compiler
guard bypass and arbitrary delegate invocation with host authority. Both now
pass. A later independent review produced a real FIFO installation RED
(305 ms blocked open, safely unblocked and joined by the test), now refusing
in 4 ms. Privileged-state checks use synthetic credential/capability inputs;
no privileges were acquired and no destructive memory-advice probe was run.

Adversarial cases deliberately bypass the public guard and actually compile
inside the worker. They verify host read/write denial, own-maps-only proc
access, no environment credentials or inheritable file descriptors, native
socket/fork/clone/exec/ioctl/ptrace/process_vm/pidfd/kill/fcntl/memfd refusal,
path-metadata/O_PATH refusal, managed process/thread denial, address-space and
heap bounds, allocation/output bombs, a non-obvious infinite loop, malformed
framing/nonzero exits, capacity refusal, and exited/reaped worker PIDs. Direct
unsealed worker launch refuses before consuming a source-side-effect request.
Trusted native artifact tampering also refuses. Existing registry persistence,
forge, declarative-tool composition, exceptions, LINQ, regex (including compiled
regex) and arithmetic remain covered.

Command from the isolated worktree:

```sh
dotnet test dotnet/Cce.Llm.Tests/CNET.Cce.Llm.Tests.csproj --no-restore \
  --filter 'FullyQualifiedName~Scriptlet' \
  --logger 'console;verbosity=normal' -v quiet
```

Final complete run: 49/49 passed in 82.05 seconds. A framework-dependent Debug
publish of `dotnet/Cce.Llm/CNET.Cce.Llm.csproj` succeeded and contained exactly
the seven listed worker artifacts. This verifies standard build/publish copy
wiring, not self-contained/RID/AOT/NuGet packaging or live service deployment.
The sole build warning is pre-existing `GhostOrchestrationTests.cs:244`
xUnit2013. Scoped `git diff --check` passed.

Independent read-only review identified the FIFO installation and inherited
privilege issues above. Both corrections were re-reviewed against source; the
reviewer reported no additional blocking issue within the documented contract.
That reviewer did not independently rerun the recorded tests. Broader product
regression and deployment remain the parent integration task.

## Primary references

- [Bubblewrap security model](https://github.com/containers/bubblewrap): caller
  flags define sandbox policy; installing the executable alone proves nothing.
- [Microsoft AssemblyLoadContext documentation](https://learn.microsoft.com/en-us/dotnet/core/dependency-loading/understanding-assemblyloadcontext): assembly
  loading isolation does not supply a security boundary.
- [Linux Landlock documentation](https://docs.kernel.org/userspace-api/landlock.html):
  restrictions inherit to future threads/children; ABI 7 lacks thread-wide TSYNC,
  so enforcement must precede CLR startup.
- [Linux seccomp documentation](https://www.kernel.org/doc/html/latest/userspace-api/seccomp_filter.html):
  filter architecture and syscall arguments; synchronize the worker filter.
- [Linux resource limits](https://man7.org/linux/man-pages/man2/setrlimit.2.html):
  address-space and CPU limits are enforced; RLIMIT_RSS does not bound modern
  Linux resident memory and will not be presented as such.
- [Linux no_new_privs](https://docs.kernel.org/userspace-api/no_new_privs.html):
  this prevents privilege gains through exec, not retained existing privileges.
- [Linux 6.17 memory advice implementation](https://github.com/torvalds/linux/blob/v6.17/mm/madvise.c):
  privileged memory-failure advice is why the native policy refuses privileged
  credentials and permits only enumerated CLR memory hints.
- [.NET 10 Console implementation](https://github.com/dotnet/runtime/blob/v10.0.7/src/libraries/System.Console/src/System/ConsolePal.Unix.cs):
  console initialization creates native signal support; it is warmed before the
  final no-new-threads filter, without writing protocol bytes.
