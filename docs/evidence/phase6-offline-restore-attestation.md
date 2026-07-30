# Attestation — fresh-checkout `ci_core` with an offline, isolated NuGet restore

This file records a verification run. It is committed **after** the code it
attests to and changes nothing the run exercised, so the tested tree and this
document are separable by construction.

## What was tested

| | |
|---|---|
| Tested code commit | `8055e9f66a13d4915660d6c9840b5f1f32f79be1` |
| Tested code tree | `d0eb099ac5a4eddff68c5454a21888c53144e0a8` |
| Worktree | brand-new disposable detached worktree, created from that commit |
| Worktree dirty files at start | 0 |
| `dotnet` `obj`/`bin` directories at start | 0 |
| NuGet user config in `HOME` | HOSTILE — see below |
| Package cache reachable | `$HOME/.nuget/packages`, 98 entries |
| Durable logs | `/tmp/cnet-integrity-phase6-evidence/` |
| Summary | `/tmp/cnet-integrity-phase6-evidence/summary.txt` sha256 `32b37f85eeed206d89fcb9a866d2a3076a90029d266384935ad4650e232efd61` |

The hostile user configuration, sha256
`1f932e0eef220b1b505081437ab98c69d46ed57818ea29001b8a20398e321735`, declares
both a package feed and an **audit source** on local HTTP endpoints that nothing
is listening on, so any contact appears as a `connect()` and as a NuGet error
rather than as a silent success:

```xml
<packageSources>
  <add key="hostile-feed"  value="http://127.0.0.1:28080/v3/index.json" />
</packageSources>
<auditSources>
  <add key="hostile-audit" value="http://127.0.0.1:28081/v3/index.json" />
</auditSources>
```

## Results

| Target | Exit | Log sha256 |
|---|---|---|
| `make managed_warning_prereq` | 0 | `a72abd9a649146e8b92ca75cfe61db5806211fbfa16cfb5d5530f9469fe70614` |
| `make managed_warning_gate` | 0 | `a5e6b10c2070c9ccba1e88444bd128480756d186cc423261b6edbffac7281345` |
| `make managed_warning_offline_proof` | 0 | `b986b859626e1f8b068be5e68a1e7281f70329bc221c2f547322266423ee4c71` |
| `make release_warning_gate` | 0 | `ed33a6d5bb363faccc06b85dbac308ca3ba31fae6780444f4b36e1e56074c0d8` |
| `make ci_core` | **0** | `ec550cde2cb8b6032a727a3ca3684f3aa937a99ef541017a151e5d856d2787b5` |

```
CNET_CI_CORE_PASS                       (marker count 1)
MANAGED_WARNING_GATE_PASS               (marker count 1)
MANAGED_WARNING_OFFLINE_PROOF_PASS      (marker count 1)
CAPABILITY_CERT_PASS certified=6/6 commit=8055e9f66a13d4915660d6c9840b5f1f32f79be1
```

Counts taken from the worktree's own gate logs after the run:

```
restores = 7        builds = 7        NU1900/NU1905 audit errors = 0
```

## Network isolation, at the syscall boundary

`make managed_warning_offline_proof` runs the real gate over the real seven
projects under `strace -f -qq -e trace=network`, with the hostile config in
`HOME` and the real package cache reachable through `NUGET_PACKAGES`.

```
OFFLINE_PROOF gate_rc=0 restores=7 builds=7 audit_errors=0
OFFLINE_PROOF inet_connect=0 inet_send=0 inet_sockets_opened=24
              inet_fd_further_use=0 unix_connect_ignored=493
MANAGED_WARNING_OFFLINE_PROOF_PASS projects=7 restores=7 builds=7
```

Trace: `/tmp/cnet-integrity-phase6-evidence/offline_strace.txt` sha256
`27b4746930af0e90cdafd4b5f74bb993305cb02b9248ca22895ff1494b649875`
(3 822 170 bytes).

* **0** `connect()` to `AF_INET` or `AF_INET6`.
* **0** `send`/`sendto`/`sendmsg` carrying an inet address.
* **24** `AF_INET`/`AF_INET6` sockets are opened, and **0** further network
  syscalls occur on any descriptor those calls returned — the check walks each
  one. They are .NET IPv4/IPv6 capability probes, which send nothing. This is
  diagnosed rather than excused: an opened-and-closed socket is not traffic, and
  traffic cannot hide behind one.
* **493** `AF_UNIX` connects are MSBuild worker pipes and the dotnet host. Local
  IPC is counted separately and deliberately not treated as feed access.

## What the gate proves, and where

`managed_warning_prereq` is the enforcing gate and runs inside `ci_core`. It
drives the real recipe with a fake `dotnet` that records every invocation and
copies the file named by `--configfile` at the moment of the call:

```
MANAGED_WARNING_PREREQ_PASS checks=30 projects=7 order=restore_before_build
  source=empty_local_only config=isolated audit=cleared_and_disabled
  refusals=failed,absent,timeout
MANAGED_WARNING_PREREQ_RED_CONFIRMED rev=7396275 property=restore precedes build
MANAGED_WARNING_PREREQ_RED_CONFIRMED rev=cb240b5 property=isolated config with --configfile
```

`managed_warning_offline_proof` is an EVIDENCE lane, not a `ci_core`
prerequisite: it needs `strace`, and the property it confirms is already
enforced by `managed_warning_prereq`. It fails closed when `strace` or the
package cache is missing.

## Reproducing

```
git worktree add --detach <tmp> 8055e9f66a13d4915660d6c9840b5f1f32f79be1
cd <tmp>
HOME=<hostile>  NUGET_PACKAGES=$HOME/.nuget/packages \
DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 \
  make managed_warning_prereq managed_warning_gate \
       managed_warning_offline_proof release_warning_gate ci_core
```

## Scope

This attests one verification run on one host. It is not a claim about any
scientific result, and nothing in `plans/cnet_integrity_remediation_20260730.md`
marked FAILED, WITHHELD or BLOCKED is changed by it.
