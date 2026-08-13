using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace CnetControlPlane.Capability;

public static partial class CapabilityCertRunner
{
    public const int SchemaVersion = 1;
    public static readonly HashSet<string> AllowedEvaluators = new(StringComparer.Ordinal) { "make", "dotnet" };
    public static readonly HashSet<string> MetricSources = new(StringComparer.Ordinal) { "heldout_receipt", "regex" };
    public const long SpecialIndexByteCap = 512L * 1024 * 1024;

    public static readonly string[] TerminalWords =
        ["PASS", "FAIL", "WITHHELD", "BLOCKED", "NO_VERDICT", "AMBIGUOUS"];

    public static readonly HashSet<string> EnvValueAllowlist = new(StringComparer.Ordinal)
    {
        "CNET_HELD_OUT_FIXTURE", "CNET_COVERAGE_ABSTAIN", "CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE",
        "CNET_BASE_PATH", "CNET_PROMOTE_EVAL_DELTA", "CNET_ORACLE_INT8", "CNET_TRAIN_FAST",
        "CNET_GGUF_MMAP", "CNET_REQUIRE_REAL_MODEL", "CCE_CLASSIFICATION_LANE_REQUIRE",
    };

    [GeneratedRegex(@"^[a-z][a-z0-9_]{2,63}$")]
    private static partial Regex CapabilityIdRe();

    [GeneratedRegex(@"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")]
    private static partial Regex CaseIdRe();

    [GeneratedRegex(
        @"^HELDOUT_FIXTURE capability=(\S+) sha256=([0-9a-f]{64}) cases=(\d+) consumed=(\d+)$",
        RegexOptions.Multiline)]
    private static partial Regex ReceiptRe();

    [GeneratedRegex(@"^HELDOUT_CASE id=(\S+) reads=(\d+)$", RegexOptions.Multiline)]
    private static partial Regex CaseRe();

    [GeneratedRegex(
        @"^HELDOUT_METRIC cases_passed=(\d+) cases_declared=(\d+) metric=([0-9.]+) errors=(\d+)$",
        RegexOptions.Multiline)]
    private static partial Regex ReceiptMetricRe();

    public static bool LooksTerminal(string marker) =>
        TerminalWords.Any(word => marker.EndsWith("_" + word, StringComparison.Ordinal));

    public static int MarkerLines(string text, string marker)
    {
        var pattern = new Regex("^" + Regex.Escape(marker) + @"(?:\s|$)", RegexOptions.Multiline);
        return pattern.Matches(text).Count;
    }

    public static (bool Ok, List<string> Problems) TerminalMarkerOk(string output, string marker)
    {
        var problems = new List<string>();
        var count = MarkerLines(output, marker);
        if (count == 0)
            problems.Add($"required marker '{marker}' is not present on its own line");
        else if (count > 1)
            problems.Add($"marker '{marker}' appears {count} times; exactly one is a verdict");

        string? prefix = null;
        foreach (var word in TerminalWords)
        {
            if (marker.EndsWith("_" + word, StringComparison.Ordinal))
            {
                prefix = marker[..^(word.Length + 1)];
                break;
            }
        }
        if (prefix is not null)
        {
            foreach (var word in TerminalWords)
            {
                var other = $"{prefix}_{word}";
                if (other != marker && MarkerLines(output, other) > 0)
                    problems.Add($"output also asserts '{other}'; a run cannot claim two verdicts");
            }
        }
        return (problems.Count == 0, problems);
    }

    public static string Sha256(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }

    public static string RepoPath(string root, string value) =>
        RepoPaths.ResolveInside(root, value);

    public static Dictionary<string, object?> LoadJsonObject(string path)
    {
        using var doc = JsonDocument.Parse(File.ReadAllText(path));
        if (doc.RootElement.ValueKind != JsonValueKind.Object)
            throw new ArgumentException($"{path}: expected a JSON object");
        return JsonUtil.ToDict(doc.RootElement)!;
    }

    private static string Git(string root, params string[] args)
    {
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = "git",
                WorkingDirectory = root,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
            };
            foreach (var a in args)
                psi.ArgumentList.Add(a);
            using var process = Process.Start(psi);
            if (process is null)
                return "";
            var stdout = process.StandardOutput.ReadToEnd();
            process.WaitForExit(60_000);
            return process.ExitCode == 0 ? stdout : "";
        }
        catch
        {
            return "";
        }
    }

    public static byte[] PathIdentity(string path)
    {
        try
        {
            var info = new FileInfo(path);
            if (info.LinkTarget is not null || (info.Attributes & FileAttributes.ReparsePoint) != 0)
            {
                try
                {
                    var target = info.LinkTarget ?? File.ResolveLinkTarget(path, false)?.FullName ?? "";
                    return Encoding.UTF8.GetBytes("symlink:" + target);
                }
                catch
                {
                    return "absent"u8.ToArray();
                }
            }
            if (!File.Exists(path))
            {
                if (Directory.Exists(path))
                    return Encoding.UTF8.GetBytes($"special:{(int)FileAttributes.Directory:o}");
                return "absent"u8.ToArray();
            }
            try
            {
                return Convert.FromHexString(Sha256(path));
            }
            catch
            {
                return "absent"u8.ToArray();
            }
        }
        catch
        {
            return "absent"u8.ToArray();
        }
    }

    public static (string Digest, int Files, long Bytes) SpecialIndexBinding(string root)
    {
        using var digest = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var entries = new List<(string Entry, char Flag)>();
        foreach (var record in Git(root, "ls-files", "-v", "-z").Split('\0'))
        {
            if (record.Length < 3 || record[1] != ' ' || record[0] == 'H')
                continue;
            entries.Add((record[2..], record[0]));
        }
        long total = 0;
        foreach (var (entry, flag) in entries.OrderBy(e => e.Entry, StringComparer.Ordinal))
        {
            var candidate = Path.Combine(root, entry);
            try { total += new FileInfo(candidate).Length; } catch { /* ignore */ }
            digest.AppendData(Encoding.UTF8.GetBytes(new[] { flag }));
            digest.AppendData(Encoding.UTF8.GetBytes(entry));
            digest.AppendData(PathIdentity(candidate));
        }
        return (Convert.ToHexString(digest.GetHashAndReset()).ToLowerInvariant(), entries.Count, total);
    }

    public static Dictionary<string, object?> GitBinding(string root)
    {
        var commit = Git(root, "rev-parse", "HEAD").Trim();
        if (string.IsNullOrEmpty(commit))
            commit = "unknown";
        using var digest = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var dirtyFiles = 0;
        foreach (var line in Git(root, "status", "--porcelain=v1").Split('\n'))
        {
            if (line.Length < 4)
                continue;
            var entry = line[3..].Split(" -> ").Last().Trim().Trim('"');
            digest.AppendData(Encoding.UTF8.GetBytes(line[..3]));
            digest.AppendData(Encoding.UTF8.GetBytes(entry));
            digest.AppendData(PathIdentity(Path.Combine(root, entry)));
            dirtyFiles++;
        }
        var (special, specialFiles, specialBytes) = SpecialIndexBinding(root);
        return new Dictionary<string, object?>
        {
            ["commit"] = commit,
            ["worktree_sha256"] = Convert.ToHexString(digest.GetHashAndReset()).ToLowerInvariant(),
            ["worktree_dirty_files"] = dirtyFiles,
            ["special_index_sha256"] = special,
            ["special_index_files"] = specialFiles,
            ["special_index_bytes"] = specialBytes,
        };
    }

    public static Dictionary<string, object?> EnvironmentBinding(IReadOnlyDictionary<string, string> environment)
    {
        var serialized = string.Join('\n', environment.Keys.OrderBy(k => k, StringComparer.Ordinal)
            .Select(k => $"{k}={environment[k]}"));
        var cnetNames = environment.Keys
            .Where(k => k.StartsWith("CNET_", StringComparison.Ordinal) || k.StartsWith("CCE_", StringComparison.Ordinal))
            .OrderBy(k => k, StringComparer.Ordinal).ToList();
        var allowlisted = cnetNames.Where(EnvValueAllowlist.Contains)
            .ToDictionary(k => k, k => environment[k]);
        var redacted = cnetNames.Where(k => !EnvValueAllowlist.Contains(k))
            .ToDictionary(k => k, k => "sha256:" + JsonUtil.Sha256Utf8(environment[k]));
        return new Dictionary<string, object?>
        {
            ["env_sha256"] = JsonUtil.Sha256Utf8(serialized),
            ["env_variables"] = environment.Count,
            ["env_knob_names"] = cnetNames,
            ["env_allowlisted_knobs"] = allowlisted,
            ["env_redacted_knobs"] = redacted,
        };
    }

    public static string UntrackedBinding(string root)
    {
        using var digest = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var listing = Git(root, "ls-files", "--others", "--exclude-standard", "-z");
        foreach (var entry in listing.Split('\0').Where(x => x.Length > 0).OrderBy(x => x, StringComparer.Ordinal))
        {
            digest.AppendData(Encoding.UTF8.GetBytes(entry));
            digest.AppendData(PathIdentity(Path.Combine(root, entry)));
        }
        return Convert.ToHexString(digest.GetHashAndReset()).ToLowerInvariant();
    }

    public static (string Digest, List<string> Resolved) SourceSetDigest(string root, IEnumerable<string> sources)
    {
        using var digest = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var resolved = new List<string>();
        foreach (var value in sources.OrderBy(x => x, StringComparer.Ordinal))
        {
            var path = RepoPath(root, value);
            if (!File.Exists(path))
                throw new ArgumentException($"declared evaluator source is missing: {value}");
            digest.AppendData(Encoding.UTF8.GetBytes(value));
            digest.AppendData(Convert.FromHexString(Sha256(path)));
            resolved.Add(value);
        }
        return (Convert.ToHexString(digest.GetHashAndReset()).ToLowerInvariant(), resolved);
    }

    private static string DigestFile(string path) => File.Exists(path) ? Sha256(path) : "absent";

    public static Dictionary<string, object?> CaptureState(
        string root,
        IReadOnlyList<string> sources,
        string fixturePath,
        string? binary,
        bool includeBinary)
    {
        var state = GitBinding(root);
        state["untracked_sha256"] = UntrackedBinding(root);
        var (sourceSha, resolved) = SourceSetDigest(root, sources);
        state["evaluator_source_set_sha256"] = sourceSha;
        state["evaluator_sources"] = resolved;
        state["fixture_sha256"] = DigestFile(fixturePath);
        if (includeBinary && binary is not null)
            state["evaluator_binary_sha256"] = DigestFile(RepoPath(root, binary));
        return state;
    }

    public static List<string> CompareStates(Dictionary<string, object?> before, Dictionary<string, object?> after)
    {
        var drift = new List<string>();
        foreach (var key in before.Keys.Union(after.Keys).OrderBy(k => k, StringComparer.Ordinal))
        {
            var b = before.GetValueOrDefault(key);
            var a = after.GetValueOrDefault(key);
            if (!JsonSerializer.Serialize(b).Equals(JsonSerializer.Serialize(a), StringComparison.Ordinal))
                drift.Add(key);
        }
        return drift;
    }

    public static (Dictionary<string, object?> Manifest, string FixturePath, Dictionary<string, object?> Fixture)
        ValidateManifest(string root, string path)
    {
        using var doc = JsonDocument.Parse(File.ReadAllText(path));
        var manifestEl = doc.RootElement;
        var required = new[]
        {
            "schema_version", "capability_id", "held_out_fixture", "evaluator", "evaluator_sources",
            "metric_source", "required_marker", "title", "owner", "evidence_artifact",
            "failure_envelope", "absolute_floor", "baseline_metric", "regression_budget",
        };
        var missing = required.Where(k => !manifestEl.TryGetProperty(k, out _)).OrderBy(x => x).ToList();
        if (missing.Count > 0)
            throw new ArgumentException($"{path}: missing keys: {string.Join(", ", missing)}");
        if (manifestEl.GetProperty("schema_version").GetInt32() != SchemaVersion)
            throw new ArgumentException($"{path}: unsupported schema_version");
        var capabilityId = manifestEl.GetProperty("capability_id").GetString()!;
        if (!CapabilityIdRe().IsMatch(capabilityId))
            throw new ArgumentException($"{path}: invalid capability_id");
        var evaluator = manifestEl.GetProperty("evaluator");
        if (evaluator.ValueKind != JsonValueKind.Array
            || evaluator.GetArrayLength() == 0
            || evaluator.EnumerateArray().Any(a =>
                a.ValueKind != JsonValueKind.String || string.IsNullOrEmpty(a.GetString()) || a.GetString()!.Contains('\0'))
            || !AllowedEvaluators.Contains(evaluator[0].GetString()!))
        {
            throw new ArgumentException($"{path}: evaluator must be a non-shell allowlisted argv");
        }
        var sources = manifestEl.GetProperty("evaluator_sources");
        if (sources.ValueKind != JsonValueKind.Array
            || sources.GetArrayLength() == 0
            || sources.EnumerateArray().Any(v => v.ValueKind != JsonValueKind.String || string.IsNullOrEmpty(v.GetString())))
        {
            throw new ArgumentException($"{path}: evaluator_sources must be a nonempty path list");
        }
        var metricSource = manifestEl.GetProperty("metric_source").GetString()!;
        if (!MetricSources.Contains(metricSource))
            throw new ArgumentException($"{path}: metric_source must be one of [{string.Join(", ", MetricSources.OrderBy(x => x).Select(x => $"'{x}'"))}]");
        if (metricSource == "regex" && (!manifestEl.TryGetProperty("metric_regex", out _)
            || string.IsNullOrEmpty(manifestEl.GetProperty("metric_regex").GetString())))
        {
            throw new ArgumentException($"{path}: metric_source=regex requires metric_regex");
        }
        if (metricSource == "heldout_receipt" && manifestEl.TryGetProperty("metric_regex", out _))
            throw new ArgumentException($"{path}: metric_source=heldout_receipt must not also declare metric_regex; one number, one source");
        if (string.IsNullOrEmpty(manifestEl.GetProperty("required_marker").GetString()))
            throw new ArgumentException($"{path}: required_marker must be nonempty");
        foreach (var field in new[] { "title", "owner", "failure_envelope" })
        {
            if (string.IsNullOrEmpty(manifestEl.GetProperty(field).GetString()))
                throw new ArgumentException($"{path}: {field} must be nonempty");
        }
        var evidenceArtifact = RepoPath(root, manifestEl.GetProperty("evidence_artifact").GetString()!);
        if (!evidenceArtifact.EndsWith(".log", StringComparison.Ordinal))
            throw new ArgumentException($"{path}: evidence_artifact must be a log path");
        foreach (var field in new[] { "absolute_floor", "baseline_metric", "regression_budget" })
        {
            var value = manifestEl.GetProperty(field).GetDouble();
            if (value < 0)
                throw new ArgumentException($"{path}: {field} must be nonnegative");
        }
        if (manifestEl.TryGetProperty("evaluator_binary", out var binary)
            && binary.ValueKind != JsonValueKind.Null
            && (binary.ValueKind != JsonValueKind.String || string.IsNullOrEmpty(binary.GetString())))
        {
            throw new ArgumentException($"{path}: evaluator_binary must be a nonempty path");
        }
        var declaration = EvaluatorPrereq.DeclarationProblems(manifestEl);
        if (declaration.Count > 0)
            throw new ArgumentException($"{path}: " + string.Join("; ", declaration));

        var fixturePath = RepoPath(root, manifestEl.GetProperty("held_out_fixture").GetString()!);
        var fixture = LoadJsonObject(fixturePath);
        if (Convert.ToString(fixture.GetValueOrDefault("capability_id")) != capabilityId)
            throw new ArgumentException($"{fixturePath}: capability_id mismatch");
        if (fixture.GetValueOrDefault("cases") is not List<object?> cases || cases.Count == 0)
            throw new ArgumentException($"{fixturePath}: held-out cases must be nonempty");
        var ids = new List<string>();
        foreach (var caseObj in cases)
        {
            if (caseObj is not Dictionary<string, object?> caseDict)
                throw new ArgumentException($"{fixturePath}: every case must be an object");
            var caseId = Convert.ToString(caseDict.GetValueOrDefault("id"));
            if (string.IsNullOrEmpty(caseId) || !CaseIdRe().IsMatch(caseId))
                throw new ArgumentException($"{fixturePath}: every case needs a stable id");
            ids.Add(caseId);
        }
        if (ids.Distinct().Count() != ids.Count)
            throw new ArgumentException($"{fixturePath}: case ids must be unique");
        if (fixture.GetValueOrDefault("expected_markers") is not List<object?> markers
            || markers.Count == 0
            || markers.Any(m => string.IsNullOrEmpty(Convert.ToString(m))))
        {
            throw new ArgumentException($"{fixturePath}: expected_markers must be nonempty strings");
        }
        return (JsonUtil.ToDict(manifestEl)!, fixturePath, fixture);
    }

    public static (bool Ok, List<string> Problems, Dictionary<string, object?> Detail) CheckReceipt(
        string output, string capabilityId, string fixtureSha, IReadOnlyList<string> caseIds)
    {
        var problems = new List<string>();
        var detail = new Dictionary<string, object?> { ["receipt_present"] = false };
        var receipts = ReceiptRe().Matches(output);
        if (receipts.Count == 0)
        {
            problems.Add("no HELDOUT_FIXTURE receipt in evaluator output");
            return (false, problems, detail);
        }
        if (receipts.Count > 1)
        {
            problems.Add($"{receipts.Count} HELDOUT_FIXTURE receipts in one run; exactly one is a proof");
            detail["receipt_count"] = receipts.Count;
            return (false, problems, detail);
        }
        var receipt = receipts[0];
        detail["receipt_present"] = true;
        detail["receipt_capability"] = receipt.Groups[1].Value;
        detail["receipt_fixture_sha256"] = receipt.Groups[2].Value;
        detail["receipt_cases"] = int.Parse(receipt.Groups[3].Value);
        detail["receipt_consumed"] = int.Parse(receipt.Groups[4].Value);
        if (receipt.Groups[1].Value != capabilityId)
            problems.Add($"receipt names capability {receipt.Groups[1].Value}, expected {capabilityId}");
        if (receipt.Groups[2].Value != fixtureSha)
            problems.Add($"receipt binds fixture {receipt.Groups[2].Value}, supplied {fixtureSha}");
        if (int.Parse(receipt.Groups[3].Value) != caseIds.Count)
            problems.Add($"receipt declares {receipt.Groups[3].Value} cases, fixture has {caseIds.Count}");
        if (int.Parse(receipt.Groups[4].Value) != caseIds.Count)
            problems.Add($"only {receipt.Groups[4].Value}/{caseIds.Count} declared cases were consumed");

        var caseLines = CaseRe().Matches(output)
            .Select(m => (Id: m.Groups[1].Value, Reads: int.Parse(m.Groups[2].Value))).ToList();
        var reads = new Dictionary<string, int>(StringComparer.Ordinal);
        var seenTwice = new List<string>();
        foreach (var (caseId, count) in caseLines)
        {
            if (reads.ContainsKey(caseId))
                seenTwice.Add(caseId);
            reads[caseId] = count;
        }
        detail["case_reads"] = reads;
        detail["case_line_count"] = caseLines.Count;
        foreach (var caseId in seenTwice.Distinct().OrderBy(x => x))
            problems.Add($"case {caseId} is reported more than once");
        foreach (var caseId in reads.Keys.Except(caseIds).OrderBy(x => x))
            problems.Add($"receipt reports case {caseId}, which the fixture does not declare");
        if (caseLines.Count != caseIds.Count)
            problems.Add($"receipt has {caseLines.Count} case lines for {caseIds.Count} declared cases");
        foreach (var caseId in caseIds)
        {
            if (!reads.ContainsKey(caseId))
                problems.Add($"case {caseId} is absent from the receipt");
            else if (reads[caseId] == 0)
                problems.Add($"case {caseId} was declared but never read");
        }

        var metricLines = ReceiptMetricRe().Matches(output);
        if (metricLines.Count > 1)
        {
            problems.Add($"{metricLines.Count} HELDOUT_METRIC lines in one run; exactly one is a proof");
            detail["metric_line_count"] = metricLines.Count;
            return (false, problems, detail);
        }
        if (metricLines.Count == 0)
        {
            problems.Add("no HELDOUT_METRIC line in evaluator output");
        }
        else
        {
            var metricLine = metricLines[0];
            detail["cases_passed"] = int.Parse(metricLine.Groups[1].Value);
            detail["cases_declared"] = int.Parse(metricLine.Groups[2].Value);
            detail["receipt_metric"] = double.Parse(metricLine.Groups[3].Value);
            detail["receipt_errors"] = int.Parse(metricLine.Groups[4].Value);
            if (Convert.ToInt32(detail["receipt_errors"]) != 0)
                problems.Add($"evaluator reported {detail["receipt_errors"]} fixture read errors");
            if (Convert.ToInt32(detail["cases_declared"]) != caseIds.Count)
                problems.Add($"receipt metric covers {detail["cases_declared"]} cases, fixture has {caseIds.Count}");
            if (Convert.ToInt32(detail["cases_passed"]) != caseIds.Count)
                problems.Add($"{detail["cases_passed"]}/{caseIds.Count} declared cases passed");
        }
        return (problems.Count == 0, problems, detail);
    }

    public static double ExtractMetric(
        Dictionary<string, object?> manifest,
        string output,
        Dictionary<string, object?> receipt)
    {
        if (Convert.ToString(manifest["metric_source"]) == "heldout_receipt")
        {
            if (!receipt.ContainsKey("receipt_metric"))
                throw new ArgumentException("metric_source=heldout_receipt but no receipt metric");
            return Convert.ToDouble(receipt["receipt_metric"]);
        }
        var pattern = Convert.ToString(manifest["metric_regex"]);
        if (string.IsNullOrEmpty(pattern) || pattern.Length > 256)
            throw new ArgumentException("metric_regex must be a bounded string");
        var compiled = new Regex(pattern);
        var matches = compiled.Matches(output);
        if (matches.Count == 0)
            throw new ArgumentException("metric_regex did not match the evaluator output");
        if (matches.Count > 1)
            throw new ArgumentException($"metric_regex matched {matches.Count} times; exactly one is a measurement");
        if (matches[0].Groups.Count != 2)
            throw new ArgumentException("metric_regex did not produce exactly one capture");
        return double.Parse(matches[0].Groups[1].Value);
    }

    public static Dictionary<string, object?> RunManifest(string root, string manifestPath, Dictionary<string, object?> run)
    {
        using var manifestDoc = JsonDocument.Parse(File.ReadAllText(manifestPath));
        var (manifest, fixturePath, fixture) = ValidateManifest(root, manifestPath);
        var capabilityId = Convert.ToString(manifest["capability_id"])!;
        var binary = manifest.TryGetValue("evaluator_binary", out var b) ? Convert.ToString(b) : null;
        var sources = ((List<object?>)manifest["evaluator_sources"]!).Select(x => Convert.ToString(x)!).ToList();
        var environment = Environment.GetEnvironmentVariables()
            .Cast<System.Collections.DictionaryEntry>()
            .ToDictionary(e => e.Key.ToString()!, e => e.Value?.ToString() ?? "");
        environment["CNET_HELD_OUT_FIXTURE"] = fixturePath;
        var timeoutSeconds = manifest.TryGetValue("timeout_seconds", out var ts) ? Convert.ToInt32(ts) : 300;
        if (timeoutSeconds is < 1 or > 1800)
            throw new ArgumentException($"{manifestPath}: timeout_seconds outside 1..1800");

        var readyProblems = EvaluatorPrereq.EnsureReady(root, manifestDoc.RootElement);
        if (readyProblems.Count > 0)
        {
            return new Dictionary<string, object?>
            {
                ["capability_id"] = capabilityId,
                ["status"] = "failed",
                ["return_code"] = null,
                ["metric"] = 0.0,
                ["metric_source"] = manifest["metric_source"],
                ["receipt_ok"] = false,
                ["receipt_problems"] = readyProblems.Select(p => $"evaluator is not ready: {p}").ToList(),
                ["markers_ok"] = false,
                ["missing_markers"] = new List<string>(),
                ["evaluator_argv"] = ((List<object?>)manifest["evaluator"]!).Select(x => Convert.ToString(x)!).ToList(),
                ["evaluator_prepare"] = manifest.GetValueOrDefault("evaluator_prepare"),
                ["evaluator_binary"] = binary,
                ["run_id"] = run["run_id"],
            };
        }

        var pre = CaptureState(root, sources, fixturePath, binary, includeBinary: false);
        var evaluatorArgv = ((List<object?>)manifest["evaluator"]!).Select(x => Convert.ToString(x)!).ToList();
        var psi = new ProcessStartInfo
        {
            FileName = evaluatorArgv[0],
            WorkingDirectory = root,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        foreach (var arg in evaluatorArgv.Skip(1))
            psi.ArgumentList.Add(arg);
        foreach (var kv in environment)
            psi.Environment[kv.Key] = kv.Value;
        var started = DateTime.UtcNow;
        using var process = Process.Start(psi) ?? throw new IOException("failed to start evaluator");
        if (!process.WaitForExit(timeoutSeconds * 1000))
        {
            try { process.Kill(entireProcessTree: true); } catch { /* ignore */ }
            throw new TimeoutException("evaluator timed out");
        }
        var stdout = process.StandardOutput.ReadToEnd() + process.StandardError.ReadToEnd();
        var duration = (DateTime.UtcNow - started).TotalSeconds;
        var postRun = CaptureState(root, sources, fixturePath, binary, includeBinary: true);

        var evidencePath = RepoPath(root, Convert.ToString(manifest["evidence_artifact"])!);
        Directory.CreateDirectory(Path.GetDirectoryName(evidencePath)!);
        File.WriteAllText(evidencePath, stdout);

        var fixtureSha = Sha256(fixturePath);
        var caseIds = ((List<object?>)fixture["cases"]!)
            .OfType<Dictionary<string, object?>>()
            .Select(c => Convert.ToString(c["id"])!)
            .ToList();
        var (receiptOk, receiptProblems, receipt) = CheckReceipt(stdout, capabilityId, fixtureSha, caseIds);

        var missingMarkers = new List<string>();
        var markerProblems = new List<string>();
        var (found, issues) = TerminalMarkerOk(stdout, Convert.ToString(manifest["required_marker"])!);
        if (!found)
        {
            missingMarkers.Add(Convert.ToString(manifest["required_marker"])!);
            markerProblems.AddRange(issues);
        }
        foreach (var markerObj in (List<object?>)fixture["expected_markers"]!)
        {
            var marker = Convert.ToString(markerObj)!;
            if (LooksTerminal(marker))
            {
                var (ok, markerIssues) = TerminalMarkerOk(stdout, marker);
                if (!ok)
                {
                    missingMarkers.Add(marker);
                    markerProblems.AddRange(markerIssues);
                }
            }
            else if (!stdout.Contains(marker, StringComparison.Ordinal))
            {
                missingMarkers.Add(marker);
            }
        }
        var markersOk = missingMarkers.Count == 0;
        receiptProblems.AddRange(markerProblems);

        double metric = 0;
        string? metricError = null;
        if (receiptOk && markersOk)
        {
            try { metric = ExtractMetric(manifest, stdout, receipt); }
            catch (ArgumentException ex) { metricError = ex.Message; }
        }
        var floor = Convert.ToDouble(manifest["absolute_floor"]);
        var regressionFloor = Convert.ToDouble(manifest["baseline_metric"]) - Convert.ToDouble(manifest["regression_budget"]);
        var passed = process.ExitCode == 0 && receiptOk && markersOk && metricError is null
            && metric >= floor && metric >= regressionFloor;

        // Fail closed: missing receipts already set receiptOk=false; never lower floors.
        var binarySha = postRun.GetValueOrDefault("evaluator_binary_sha256");
        if (binary is not null && (binarySha is null || Convert.ToString(binarySha) == "absent"))
        {
            passed = false;
            receiptProblems.Add($"declared evaluator binary is absent: {binary}");
        }

        var postBind = CaptureState(root, sources, fixturePath, binary, includeBinary: true);
        var preDrift = CompareStates(pre, postRun).Where(k => k != "evaluator_binary_sha256").ToList();
        var postDrift = CompareStates(postRun, postBind);
        var bindingDrift = preDrift.Union(postDrift).OrderBy(x => x).ToList();
        if (bindingDrift.Count > 0)
        {
            passed = false;
            receiptProblems.Add("bound state changed during the run: " + string.Join(", ", bindingDrift));
        }

        var result = new Dictionary<string, object?>
        {
            ["capability_id"] = capabilityId,
            ["status"] = passed ? "certified" : "failed",
            ["return_code"] = process.ExitCode,
            ["metric"] = metric,
            ["metric_source"] = manifest["metric_source"],
            ["metric_error"] = metricError,
            ["absolute_floor"] = floor,
            ["regression_floor"] = regressionFloor,
            ["fixture_sha256"] = fixtureSha,
            ["manifest_sha256"] = Sha256(manifestPath),
            ["evidence_sha256"] = Sha256(evidencePath),
            ["evidence_log"] = Path.GetRelativePath(root, evidencePath).Replace('\\', '/'),
            ["markers_ok"] = markersOk,
            ["missing_markers"] = missingMarkers,
            ["receipt_ok"] = receiptOk,
            ["receipt_problems"] = receiptProblems,
            ["receipt"] = receipt,
            ["declared_case_ids"] = caseIds,
            ["evaluator_argv"] = evaluatorArgv,
            ["evaluator_prepare"] = manifest.GetValueOrDefault("evaluator_prepare"),
            ["evaluator_sources"] = sources,
            ["evaluator_source_set_sha256"] = postRun["evaluator_source_set_sha256"],
            ["evaluator_binary"] = binary,
            ["evaluator_binary_sha256"] = binarySha,
            ["binding_pre"] = pre,
            ["binding_post_run"] = postRun,
            ["binding_post_bind"] = postBind,
            ["binding_stable"] = bindingDrift.Count == 0,
            ["binding_drift"] = bindingDrift,
            ["duration_seconds"] = Math.Round(duration, 3),
            ["run_id"] = run["run_id"],
        };
        foreach (var kv in EnvironmentBinding(environment))
            result[kv.Key] = kv.Value;
        return result;
    }

    public static int RunCli(string[] args, string? rootOverride = null)
    {
        var manifestDirRel = "config/capability_manifests";
        var outputRel = "logs/capability_cert.json";
        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--manifest-dir": manifestDirRel = args[++i]; break;
                case "--output": outputRel = args[++i]; break;
                default: throw new ArgumentException($"unknown argument: {args[i]}");
            }
        }
        var root = rootOverride ?? RepoPaths.FindRoot();
        var manifestDir = RepoPath(root, manifestDirRel);
        var outputPath = RepoPath(root, outputRel);
        var manifests = Directory.Exists(manifestDir)
            ? Directory.GetFiles(manifestDir, "*.json").OrderBy(x => x).ToList()
            : [];
        if (manifests.Count == 0)
        {
            Console.Error.WriteLine("CAPABILITY_CERT_FAIL reason=no_manifests");
            return 1;
        }
        var run = new Dictionary<string, object?>
        {
            ["run_id"] = Guid.NewGuid().ToString(),
            ["started_at"] = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ"),
        };
        foreach (var kv in GitBinding(root))
            run[kv.Key] = kv.Value;
        if (Convert.ToInt64(run["special_index_bytes"]) > SpecialIndexByteCap)
        {
            Console.Error.WriteLine(
                "CAPABILITY_CERT_FAIL reason=special_index_too_large:"
                + $"{run["special_index_bytes"]}>{SpecialIndexByteCap} "
                + "refusing to certify against a binding that does not cover the tree");
            return 1;
        }
        Console.WriteLine(
            $"CAPABILITY_CERT_RUN run_id={run["run_id"]} started={run["started_at"]} "
            + $"commit={run["commit"]} worktree={Convert.ToString(run["worktree_sha256"])![..16]} "
            + $"dirty={run["worktree_dirty_files"]} "
            + $"special_index={Convert.ToString(run["special_index_sha256"])![..16]} "
            + $"special_index_files={run["special_index_files"]}");

        var results = new List<Dictionary<string, object?>>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var manifestPath in manifests)
        {
            Dictionary<string, object?> result;
            try
            {
                result = RunManifest(root, manifestPath, run);
                var id = Convert.ToString(result["capability_id"])!;
                if (!seen.Add(id))
                    throw new ArgumentException($"duplicate capability_id: {id}");
            }
            catch (Exception ex) when (ex is IOException or ArgumentException or JsonException or TimeoutException)
            {
                result = new Dictionary<string, object?>
                {
                    ["capability_id"] = Path.GetFileNameWithoutExtension(manifestPath),
                    ["status"] = "failed",
                    ["error"] = ex.Message,
                    ["run_id"] = run["run_id"],
                };
            }
            results.Add(result);
            if (result.GetValueOrDefault("receipt_problems") is List<string> problems)
            {
                foreach (var problem in problems)
                    Console.WriteLine($"CAPABILITY_RECEIPT id={result["capability_id"]} problem={problem}");
            }
            if (result.GetValueOrDefault("missing_markers") is List<string> markers)
            {
                foreach (var marker in markers)
                    Console.WriteLine($"CAPABILITY_MARKER id={result["capability_id"]} missing='{marker}'");
            }
            if (result.TryGetValue("error", out var err) && err is not null)
                Console.WriteLine($"CAPABILITY_ERROR id={result["capability_id"]} {err}");
            Console.WriteLine(
                $"CAPABILITY id={result["capability_id"]} "
                + $"status={result.GetValueOrDefault("status")} metric={Convert.ToDouble(result.GetValueOrDefault("metric") ?? 0):0.000} "
                + $"source={result.GetValueOrDefault("metric_source") ?? "none"} "
                + $"receipt={(Convert.ToBoolean(result.GetValueOrDefault("receipt_ok")) ? "ok" : "missing")}");
        }
        var certified = results.Count(r => Convert.ToString(r["status"]) == "certified");
        var report = new Dictionary<string, object?>
        {
            ["schema_version"] = SchemaVersion,
            ["certified"] = certified,
            ["total"] = results.Count,
            ["run"] = run,
            ["results"] = results,
        };
        JsonUtil.WriteJsonAtomic(outputPath, report);
        if (certified != results.Count)
        {
            Console.WriteLine($"CAPABILITY_CERT_FAIL certified={certified}/{results.Count}");
            return 1;
        }
        Console.WriteLine(
            $"CAPABILITY_CERT_PASS certified={certified}/{results.Count} "
            + $"run_id={run["run_id"]} commit={run["commit"]}");
        return 0;
    }
}
