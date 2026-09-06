# Separate control-plane SQLite provider closure — 2026-09-06

Result: the separate `net8.0` control plane loads native SQLite **3.53.4**;
the observed vulnerable **3.41.2** runtime is removed from its application and
test dependency graphs. All **80 tests passed, zero failed or skipped**.
Both transitive NuGet audits returned no advisories from nuget.org at execution.
This is bounded provider and workflow compatibility evidence, not a claim that
all SQLite versions above a single historical floor are vulnerability-free.

Source baseline: `588d3cc21703749532a6546acbc835ea5c127203`, on
`feature/product-closure-20260906`, plus the provider/test/lockfile changes in the
commit containing this record. Environment: Linux x86_64, .NET SDK 10.0.203,
test target .NET 8.0, runtime 8.0.30. Databases were in-memory or created in
private temporary directories by the tests. No live database, service, registry,
OS SQLite installation or GPU was used or changed.

## Migration decision and primary sources

The former `Microsoft.Data.Sqlite` 8.0.11 metapackage resolved
`SQLitePCLRaw.lib.e_sqlite3` 2.1.6. The pre-change audit reported High severity
[GHSA-2m69-gcr7-jv3q / CVE-2025-6965](https://github.com/advisories/GHSA-2m69-gcr7-jv3q).
[SQLite's vulnerability record](https://www.sqlite.org/cves.html) identifies
3.50.2 as the fix version for that vulnerability.

The application now references `Microsoft.Data.Sqlite.Core` 8.0.11 and
`SQLitePCLRaw.bundle_e_sqlite3` 3.0.5. This preserves the existing managed adapter
version while explicitly choosing its maintained native bundle. Tests inherit
the application dependencies through their project reference; the test-only
metapackage reference was removed. There is no custom initialization or runtime
loading shim: [Microsoft documents Core plus a selected bundle and automatic
bundle initialization](https://learn.microsoft.com/en-us/dotnet/standard/data/sqlite/custom-versions).

The [maintainer's v3 migration notes](https://github.com/ericsink/SQLitePCL.raw/blob/main/v3.md)
describe the managed-core compatibility and separation of native package updates
from provider updates. More recent [v3.0.5 release notes](https://github.com/ericsink/SQLitePCL.raw/releases/tag/v3.0.5)
explicitly change the native dependency to package ID `SQLite`, version 3.53.4.
The [bundle's official NuGet metadata](https://www.nuget.org/packages/SQLitePCLRaw.bundle_e_sqlite3/3.0.5)
and [native package metadata](https://www.nuget.org/packages/SQLite/3.53.4) agree.
[SQLite 3.53.4 release notes](https://www.sqlite.org/releaselog/3_53_4.html)
identify its release as 2026-07-24. No new native APIs, schema migration or new
SQL features were introduced into the control plane by this change.

Both generated `packages.lock.json` files retain package content hashes and
resolve the following graph:

| Package | Application | Tests |
|---|---|---|
| Microsoft.Data.Sqlite.Core | 8.0.11 direct | 8.0.11 transitive |
| SQLitePCLRaw.bundle_e_sqlite3 | 3.0.5 direct | 3.0.5 transitive |
| SQLitePCLRaw.config.e_sqlite3 | 3.0.5 | 3.0.5 |
| SQLitePCLRaw.provider.e_sqlite3 | 3.0.5 | 3.0.5 |
| SQLitePCLRaw.core | 3.0.5 | 3.0.5 |
| SQLite | 3.53.4 | 3.53.4 |

`SQLitePCLRaw.lib.e_sqlite3` is absent from both resolved assets graphs.
The older `2.1.6` requirement inside the managed-core package metadata is a
minimum requirement; the actual resolved core version is 3.0.5. Restore lock
generation is enabled for these two projects only. Use `--locked-mode` to refuse
dependency drift in reproducible verification. No audit suppression was added.

## RED before dependency changes

Added `Loaded_native_library_meets_security_floor` and ran:

```sh
dotnet test dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj --filter 'FullyQualifiedName~SqliteProviderTests|FullyQualifiedName~Failed_followup_insert_rolls_back_the_entire_ingestion' --logger 'console;verbosity=normal'
```

Exit **1**: **1 failed, 2 passed**. Actual assertion and test output:

```text
SQLITE_NATIVE_SECURITY_RED: loaded 3.41.2; CVE-2025-6965 requires >= 3.50.2
SQLITE_NATIVE_VERSION=3.41.2
```

The native version comes from executing `SELECT sqlite_version()` on the
loaded provider, not an assembly or package version. The JSON and injected
rollback tests passed on the old runtime before replacement. The pre-change
test-project transitive audit independently identified the vulnerable native
package 2.1.6 with High severity.

## GREEN and reproducible checks

All commands below exited **0**:

```sh
dotnet restore dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj --force-evaluate -p:NuGetAuditMode=all
dotnet test dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj --no-restore --filter 'FullyQualifiedName~SqliteProviderTests|FullyQualifiedName~IngestTests|FullyQualifiedName~ActivateTests' --logger 'console;verbosity=detailed'
dotnet test dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj --no-restore --logger 'console;verbosity=normal'
dotnet list dotnet/CnetControlPlane/CnetControlPlane.csproj package --vulnerable --include-transitive --format json
dotnet list dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj package --vulnerable --include-transitive --format json
dotnet restore dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj --locked-mode -p:NuGetAuditMode=all
git diff --check
```

- Focused suite: **14 passed**, with `SQLITE_NATIVE_VERSION=3.53.4`.
- Full separate control-plane suite: **80 passed**, **0 failed**, **0 skipped**.
- JSON test checks native `json_extract`, `json_set`, and JSON booleans.
- Ingestion tests check persisted inserts, metadata, deduplication, and
  trigger-induced failure of the second record. The injected failure raises an
  exception and leaves **zero rows**, then a retry commits **two rows**.
- Activation tests cover actionable selection, dry-run purity, evidence-bound
  verification, policy refusal, candidate identity mismatch, claimed state
  surviving interruption, and resumed completion using fresh connections.
- Both audits return the selected project with no vulnerable package entries
  and `https://api.nuget.org/v3/index.json` as the audit source.
- Locked restore and inspection of both `obj/project.assets.json` files confirm
  the same package versions as the committed locks and no old native package.

Independent parent review accepted the Core/bundle boundary subject to actual
native-version, graph, transaction, JSON and resume verification. Those checks
executed as recorded above. The parent then reviewed the project diff, native
and JSON tests, transaction rollback test and application lockfile and reported
no required code changes for this bounded migration.
The git workflow, incremental implementation, debugging and code review skills
kept this slice confined to the provider boundary, RED evidence and regression
coverage. A referenced incremental-skill Definition of Done file was unavailable;
the explicit repository and product-plan gates supplied the completion criteria.

Not measured here: Windows/macOS/mobile execution, publish/single-file native
loading, upgrades of pre-existing live database files, process-kill or power-loss
database recovery, production restart, unrelated managed projects, or overall
product/capability closure. Live rollout remains a separate authorized action.
