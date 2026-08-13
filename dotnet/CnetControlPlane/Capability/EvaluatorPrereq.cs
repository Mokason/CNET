using System.Diagnostics;
using System.Text.Json;

namespace CnetControlPlane.Capability;

public static class EvaluatorPrereq
{
    public static readonly HashSet<string> AllowedPrepare = new(StringComparer.Ordinal) { "make", "dotnet" };
    public static readonly HashSet<string> SelfBuilding = new(StringComparer.Ordinal) { "make" };
    public const int PrepareTimeoutSeconds = 600;

    private const string IsolatedConfig = """
        <?xml version="1.0" encoding="utf-8"?>
        <configuration>
          <packageSources>
            <clear />
            <add key="cnet-empty-local" value="{empty}" />
          </packageSources>
          <disabledPackageSources>
            <clear />
          </disabledPackageSources>
          <fallbackPackageFolders>
            <clear />
          </fallbackPackageFolders>
          <auditSources>
            <clear />
          </auditSources>
        </configuration>
        """;

    public static string RepoPath(string root, string value)
    {
        var candidate = Path.GetFullPath(Path.Combine(root, value));
        var rootFull = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        if (!candidate.Equals(rootFull, StringComparison.OrdinalIgnoreCase)
            && !candidate.StartsWith(rootFull + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)
            && !candidate.StartsWith(rootFull + Path.AltDirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
        {
            throw new ArgumentException($"path escapes the repository: {value}");
        }
        return candidate;
    }

    public static List<string> DeclarationProblems(JsonElement manifest)
    {
        var problems = new List<string>();
        var hasPrepare = manifest.TryGetProperty("evaluator_prepare", out var prepare)
            && prepare.ValueKind != JsonValueKind.Null;
        var hasBinary = manifest.TryGetProperty("evaluator_binary", out var binary)
            && binary.ValueKind == JsonValueKind.String
            && !string.IsNullOrEmpty(binary.GetString());

        if (hasPrepare)
        {
            if (prepare.ValueKind != JsonValueKind.Array
                || prepare.GetArrayLength() == 0
                || prepare.EnumerateArray().Any(a =>
                    a.ValueKind != JsonValueKind.String
                    || string.IsNullOrEmpty(a.GetString())
                    || a.GetString()!.Contains('\0')))
            {
                problems.Add("evaluator_prepare must be a nonempty argv of strings");
            }
            else if (!AllowedPrepare.Contains(prepare[0].GetString()!))
            {
                problems.Add(
                    $"evaluator_prepare command '{prepare[0].GetString()}' is not allowlisted "
                    + $"([{string.Join(", ", AllowedPrepare.OrderBy(x => x).Select(x => $"'{x}'"))}])");
            }
            if (!hasBinary)
            {
                problems.Add(
                    "evaluator_prepare requires evaluator_binary: a build step whose "
                    + "output is not declared cannot be checked for having produced it");
            }
        }

        if (manifest.TryGetProperty("evaluator", out var evaluator)
            && evaluator.ValueKind == JsonValueKind.Array
            && evaluator.GetArrayLength() > 0)
        {
            var cmd = evaluator[0].GetString() ?? "";
            if (!SelfBuilding.Contains(cmd) && !hasPrepare)
            {
                problems.Add(
                    $"evaluator '{cmd}' does not build its own binary, so the "
                    + "manifest must declare evaluator_prepare; without it a fresh "
                    + "checkout runs whatever stale or missing output it finds");
            }
        }
        return problems;
    }

    public static List<string> NugetIsolationArgv(IReadOnlyList<string> prepare, string isolated)
    {
        if (prepare.Count == 0 || prepare[0] != "dotnet")
            return [];
        var empty = Path.Combine(isolated, "empty-source");
        Directory.CreateDirectory(empty);
        var config = Path.Combine(isolated, "NuGet.Config");
        File.WriteAllText(config, IsolatedConfig.Replace("{empty}", empty));
        return
        [
            $"-p:RestoreConfigFile={config}",
            $"-p:RestoreSources={empty}",
            "-p:NuGetAudit=false",
        ];
    }

    public static List<string> FreshnessProblems(string root, JsonElement manifest)
    {
        var problems = new List<string>();
        if (!manifest.TryGetProperty("evaluator_binary", out var binary)
            || binary.ValueKind != JsonValueKind.String
            || string.IsNullOrEmpty(binary.GetString()))
        {
            return ["evaluator_binary is not declared, so readiness cannot be checked"];
        }
        var binaryPath = RepoPath(root, binary.GetString()!);
        if (!File.Exists(binaryPath))
            return [$"declared evaluator binary is absent after prepare: {binary.GetString()}"];
        var builtAt = new FileInfo(binaryPath).LastWriteTimeUtc.Ticks;
        if (manifest.TryGetProperty("evaluator_sources", out var sources)
            && sources.ValueKind == JsonValueKind.Array)
        {
            foreach (var value in sources.EnumerateArray().Select(s => s.GetString()!).OrderBy(x => x))
            {
                var sourcePath = RepoPath(root, value);
                if (!File.Exists(sourcePath))
                {
                    problems.Add($"declared evaluator source is missing: {value}");
                    continue;
                }
                if (new FileInfo(sourcePath).LastWriteTimeUtc.Ticks > builtAt)
                {
                    problems.Add(
                        $"declared evaluator binary {binary.GetString()} is older than its declared "
                        + $"source {value}: the run would use stale output");
                }
            }
        }
        return problems;
    }

    public static (int? Code, string Output) RunPrepare(string root, JsonElement manifest)
    {
        if (!manifest.TryGetProperty("evaluator_prepare", out var prepare)
            || prepare.ValueKind != JsonValueKind.Array)
        {
            return (null, "");
        }
        var argv = prepare.EnumerateArray().Select(a => a.GetString()!).ToList();
        var env = new Dictionary<string, string?>();
        foreach (System.Collections.DictionaryEntry e in Environment.GetEnvironmentVariables())
            env[e.Key.ToString()!] = e.Value?.ToString();
        env.Remove("MAKEFLAGS");
        env.Remove("MFLAGS");

        var isolated = Directory.CreateTempSubdirectory("cnet-prepare-nuget-");
        try
        {
            argv.AddRange(NugetIsolationArgv(argv, isolated.FullName));
            var psi = new ProcessStartInfo
            {
                FileName = argv[0],
                WorkingDirectory = root,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
            };
            foreach (var arg in argv.Skip(1))
                psi.ArgumentList.Add(arg);
            foreach (var kv in env)
            {
                if (kv.Value is not null)
                    psi.Environment[kv.Key] = kv.Value;
            }
            using var process = Process.Start(psi)
                ?? throw new IOException($"could not start {argv[0]}");
            if (!process.WaitForExit(PrepareTimeoutSeconds * 1000))
            {
                try { process.Kill(entireProcessTree: true); } catch { /* ignore */ }
                throw new TimeoutException(
                    "evaluator_prepare timed out after "
                    + $"{PrepareTimeoutSeconds}s: " + string.Join(' ', argv.Take(prepare.GetArrayLength())));
            }
            var output = process.StandardOutput.ReadToEnd() + process.StandardError.ReadToEnd();
            return (process.ExitCode, output);
        }
        finally
        {
            try { isolated.Delete(true); } catch { /* ignore */ }
        }
    }

    public static List<string> EnsureReady(string root, JsonElement manifest)
    {
        var problems = DeclarationProblems(manifest);
        if (problems.Count > 0)
            return problems;
        if (!manifest.TryGetProperty("evaluator_prepare", out _)
            || manifest.GetProperty("evaluator_prepare").ValueKind == JsonValueKind.Null)
        {
            return [];
        }
        try
        {
            var (code, output) = RunPrepare(root, manifest);
            if (code != 0)
            {
                var lines = output.Trim().Split('\n');
                var tail = string.Join('\n', lines.TakeLast(12));
                var prepare = manifest.GetProperty("evaluator_prepare")
                    .EnumerateArray().Select(a => a.GetString());
                return
                [
                    $"evaluator_prepare failed (rc={code}): {string.Join(' ', prepare)}\n{tail}",
                ];
            }
            return FreshnessProblems(root, manifest);
        }
        catch (TimeoutException ex)
        {
            return [ex.Message];
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return [$"evaluator_prepare could not be executed: {ex.Message}"];
        }
    }

    public static int RunCli(string[] args, string? rootOverride = null)
    {
        var root = rootOverride ?? RepoPaths.FindRoot();
        var manifestDir = Path.Combine(root, "config", "capability_manifests");
        var wanted = new HashSet<string>(args, StringComparer.Ordinal);
        var manifests = Directory.Exists(manifestDir)
            ? Directory.GetFiles(manifestDir, "*.json").OrderBy(x => x).ToList()
            : [];
        if (manifests.Count == 0)
        {
            Console.WriteLine("CAPABILITY_EVALUATOR_PREREQ_FAIL reason=no_manifests");
            return 1;
        }
        var failures = 0;
        var checkedCount = 0;
        var prepared = 0;
        var knownIds = new HashSet<string>(StringComparer.Ordinal);
        foreach (var manifestPath in manifests)
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(manifestPath));
            var capabilityId = doc.RootElement.TryGetProperty("capability_id", out var id)
                ? id.GetString()! : Path.GetFileNameWithoutExtension(manifestPath);
            knownIds.Add(capabilityId);
            if (wanted.Count > 0 && !wanted.Contains(capabilityId))
                continue;
            checkedCount++;
            var declares = doc.RootElement.TryGetProperty("evaluator_prepare", out var prep)
                && prep.ValueKind == JsonValueKind.Array;
            var problems = EnsureReady(root, doc.RootElement);
            foreach (var problem in problems)
                Console.WriteLine($"CAPABILITY_PREREQ id={capabilityId} problem={problem}");
            if (problems.Count > 0)
            {
                failures++;
                continue;
            }
            if (declares)
                prepared++;
            Console.WriteLine(
                $"CAPABILITY_PREREQ id={capabilityId} "
                + $"prepare={(declares ? "declared" : "self_building")} "
                + $"binary={(declares ? "fresh" : "built_by_evaluator")}");
        }
        foreach (var capabilityId in wanted.Except(knownIds).OrderBy(x => x))
        {
            Console.WriteLine($"CAPABILITY_PREREQ id={capabilityId} problem=no such manifest");
            failures++;
        }
        if (checkedCount == 0)
        {
            Console.WriteLine("CAPABILITY_EVALUATOR_PREREQ_FAIL reason=nothing_checked");
            return 1;
        }
        if (failures > 0)
        {
            Console.WriteLine($"CAPABILITY_EVALUATOR_PREREQ_FAIL failures={failures}");
            return 1;
        }
        Console.WriteLine(
            "CAPABILITY_EVALUATOR_PREREQ_PASS "
            + $"capabilities={checkedCount} prepared={prepared} "
            + $"scope={(wanted.Count > 0 ? "selected" : "all")}");
        return 0;
    }
}
