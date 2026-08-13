using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace CnetControlPlane.Acceptance;

public static class RealModelAcceptance
{
    public static readonly (string Id, string Prompt, string Expected)[] Probes =
    [
        ("instruction_exact", "/no_think\nReply with exactly READY.", "READY"),
        ("arithmetic_exact", "/no_think\nWhat is 17+25? Reply with digits only.", "42"),
        ("constraint_exact", "/no_think\nReply with the word BLUE and nothing else.", "BLUE"),
    ];

    private static readonly Dictionary<int, string> GgmlTypeNames = new()
    {
        [0] = "F32", [1] = "F16", [2] = "Q4_0", [3] = "Q4_1", [6] = "Q5_0", [7] = "Q5_1",
        [8] = "Q8_0", [9] = "Q8_1", [10] = "Q2_K", [11] = "Q3_K", [12] = "Q4_K", [13] = "Q5_K",
        [14] = "Q6_K", [15] = "Q8_K", [16] = "IQ2_XXS", [17] = "IQ2_XS", [18] = "IQ3_XXS",
        [19] = "IQ1_S", [20] = "IQ4_NL", [21] = "IQ3_S", [22] = "IQ2_S", [23] = "IQ4_XS",
        [24] = "I8", [25] = "I16", [26] = "I32", [27] = "I64", [28] = "F64", [29] = "IQ1_M",
        [30] = "BF16", [34] = "TQ1_0", [35] = "TQ2_0",
    };

    public static void RequireMagic(string path, ReadOnlySpan<byte> expected)
    {
        if (!File.Exists(path))
            throw new ArgumentException($"file not found: {path}");
        using var fs = File.OpenRead(path);
        Span<byte> actual = stackalloc byte[expected.Length];
        var n = fs.Read(actual);
        if (n != expected.Length || !actual.SequenceEqual(expected))
            throw new ArgumentException($"magic mismatch for {path}: expected {Encoding.ASCII.GetString(expected)}");
    }

    public static string Sha256File(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }

    private static byte[] ReadExact(BinaryReader br, int count)
    {
        var data = br.ReadBytes(count);
        if (data.Length != count)
            throw new ArgumentException("truncated GGUF metadata");
        return data;
    }

    private static string ReadGgufString(BinaryReader br)
    {
        var count = br.ReadInt64();
        if (count > 100_000_000)
            throw new ArgumentException($"unreasonable GGUF string length: {count}");
        return Encoding.UTF8.GetString(ReadExact(br, (int)count));
    }

    private static object? ReadOrSkipValue(BinaryReader br, int valueType, bool keep)
    {
        if (valueType == 8)
        {
            var value = ReadGgufString(br);
            return keep ? value : null;
        }
        if (valueType == 9)
        {
            var elementType = br.ReadInt32();
            var count = br.ReadInt64();
            if (count > 10_000_000)
                throw new ArgumentException($"unreasonable GGUF array length: {count}");
            if (elementType == 8)
            {
                List<string>? values = keep && count < 100 ? new List<string>() : null;
                for (var i = 0; i < count; i++)
                {
                    var v = ReadGgufString(br);
                    values?.Add(v);
                }
                return values;
            }
            var size = elementType switch
            {
                0 or 1 or 7 => 1,
                2 or 3 => 2,
                4 or 5 or 6 => 4,
                10 or 11 or 12 => 8,
                _ => throw new ArgumentException($"unsupported GGUF array element type: {elementType}"),
            } * (int)count;
            _ = ReadExact(br, size);
            return null;
        }
        object valueObj = valueType switch
        {
            0 => br.ReadByte(),
            1 => br.ReadSByte(),
            2 => br.ReadUInt16(),
            3 => br.ReadInt16(),
            4 => br.ReadUInt32(),
            5 => br.ReadInt32(),
            6 => br.ReadSingle(),
            7 => br.ReadByte() != 0,
            10 => br.ReadUInt64(),
            11 => br.ReadInt64(),
            12 => br.ReadDouble(),
            _ => throw new ArgumentException($"unsupported GGUF value type: {valueType}"),
        };
        return keep ? valueObj : null;
    }

    public static Dictionary<string, object?> InspectGguf(string path)
    {
        using var fs = File.OpenRead(path);
        using var br = new BinaryReader(fs);
        var magic = Encoding.ASCII.GetString(ReadExact(br, 4));
        if (magic != "GGUF")
            throw new ArgumentException($"magic mismatch for {path}: expected b'GGUF'");
        var version = br.ReadUInt32();
        var tensorCount = br.ReadInt64();
        var metadataCount = br.ReadInt64();
        if (version is not (2 or 3))
            throw new ArgumentException($"unsupported GGUF version: {version}");
        if (tensorCount > 10_000_000 || metadataCount > 1_000_000)
            throw new ArgumentException("unreasonable GGUF header counts");

        var selectedKeys = new HashSet<string>
        {
            "general.architecture", "general.file_type", "general.name", "general.quantization_version",
        };
        var selected = new Dictionary<string, object?>();
        for (var i = 0; i < metadataCount; i++)
        {
            var key = ReadGgufString(br);
            var valueType = br.ReadInt32();
            var value = ReadOrSkipValue(br, valueType, selectedKeys.Contains(key));
            if (selectedKeys.Contains(key))
                selected[key] = value;
        }

        var tensorTypes = new Dictionary<string, int>();
        for (var i = 0; i < tensorCount; i++)
        {
            _ = ReadGgufString(br);
            var dimensions = br.ReadInt32();
            if (dimensions > 8)
                throw new ArgumentException($"unreasonable GGUF tensor rank: {dimensions}");
            _ = ReadExact(br, dimensions * 8);
            var tensorType = br.ReadInt32();
            _ = ReadExact(br, 8);
            var name = GgmlTypeNames.GetValueOrDefault(tensorType, $"TYPE_{tensorType}");
            tensorTypes[name] = tensorTypes.GetValueOrDefault(name) + 1;
        }

        return new Dictionary<string, object?>
        {
            ["version"] = (int)version,
            ["architecture"] = selected.GetValueOrDefault("general.architecture"),
            ["name"] = selected.GetValueOrDefault("general.name"),
            ["file_type"] = selected.GetValueOrDefault("general.file_type"),
            ["quantization_version"] = selected.GetValueOrDefault("general.quantization_version"),
            ["tensor_count"] = tensorCount,
            ["metadata_count"] = metadataCount,
            ["tensor_types"] = tensorTypes.OrderBy(kv => kv.Key).ToDictionary(kv => kv.Key, kv => kv.Value),
        };
    }

    public static List<string> StructuralMismatchReasons(
        Dictionary<string, object?> reference,
        Dictionary<string, object?> candidate)
    {
        var reasons = new List<string>();
        if (!Equals(reference.GetValueOrDefault("architecture"), candidate.GetValueOrDefault("architecture")))
            reasons.Add("architecture_mismatch");
        var refCount = Convert.ToInt64(reference.GetValueOrDefault("tensor_count") ?? -1);
        var candCount = Convert.ToInt64(candidate.GetValueOrDefault("tensor_count") ?? -2);
        if (refCount != candCount)
            reasons.Add("tensor_count_mismatch");
        return reasons;
    }

    public static Dictionary<string, object?> QuantizationDiagnosis(
        Dictionary<string, object?> reference,
        Dictionary<string, object?> candidate)
    {
        static double TernaryFraction(Dictionary<string, object?> inspection)
        {
            var types = inspection.GetValueOrDefault("tensor_types") as Dictionary<string, int>
                ?? new Dictionary<string, int>();
            var ternary = types.GetValueOrDefault("TQ1_0") + types.GetValueOrDefault("TQ2_0");
            var count = Convert.ToInt64(inspection.GetValueOrDefault("tensor_count") ?? 0);
            return count == 0 ? 0.0 : ternary / (double)count;
        }
        var referenceFraction = TernaryFraction(reference);
        var candidateFraction = TernaryFraction(candidate);
        return new Dictionary<string, object?>
        {
            ["reference_ternary_fraction"] = referenceFraction,
            ["candidate_ternary_fraction"] = candidateFraction,
            ["aggressive_ternarization"] =
                candidateFraction > 0.5 && candidateFraction > referenceFraction + 0.25,
            ["source"] = "embedded_gguf_tensor_types",
        };
    }

    private static Dictionary<int, double> ProbabilityMap(IReadOnlyList<Dictionary<string, object?>> topLogprobs)
    {
        var values = new Dictionary<int, double>();
        foreach (var item in topLogprobs)
        {
            var id = Convert.ToInt32(item["id"]);
            values[id] = Math.Exp(Convert.ToDouble(item["logprob"]));
        }
        var total = values.Values.Sum();
        if (total <= 0)
            return values.ToDictionary(kv => kv.Key, _ => 0.0);
        return values.ToDictionary(kv => kv.Key, kv => kv.Value / total);
    }

    public static double NormalizedEntropy(IReadOnlyList<Dictionary<string, object?>> topLogprobs)
    {
        var probabilities = ProbabilityMap(topLogprobs).Values.ToList();
        if (probabilities.Count <= 1)
            return 0.0;
        var entropy = -probabilities.Where(v => v > 0).Sum(v => v * Math.Log(v));
        return entropy / Math.Log(probabilities.Count);
    }

    private static (List<double> Ref, List<double> Cand) AlignedDistributions(
        IReadOnlyList<Dictionary<string, object?>> reference,
        IReadOnlyList<Dictionary<string, object?>> candidate)
    {
        var refMap = ProbabilityMap(reference);
        var candMap = ProbabilityMap(candidate);
        var tokenIds = refMap.Keys.Union(candMap.Keys).OrderBy(x => x).ToList();
        return (
            tokenIds.Select(id => refMap.GetValueOrDefault(id)).ToList(),
            tokenIds.Select(id => candMap.GetValueOrDefault(id)).ToList());
    }

    private static double L2(IEnumerable<double> values) =>
        Math.Sqrt(values.Sum(v => v * v));

    public static Dictionary<string, object?> DistributionMetrics(
        IReadOnlyList<Dictionary<string, object?>> reference,
        IReadOnlyList<Dictionary<string, object?>> candidate)
    {
        var (refValues, candValues) = AlignedDistributions(reference, candidate);
        var residual = refValues.Zip(candValues, (r, c) => r - c).ToList();
        var curve = new List<Dictionary<string, object?>>();
        foreach (var alpha in new[] { 0.0, 0.25, 0.5, 0.75, 1.0 })
        {
            var recovered = candValues.Zip(residual, (c, d) => c + alpha * d).ToList();
            curve.Add(new Dictionary<string, object?>
            {
                ["alpha"] = alpha,
                ["residual_l2_norm"] = L2(refValues.Zip(recovered, (r, v) => r - v)),
            });
        }
        return new Dictionary<string, object?>
        {
            ["identity_l2_norm"] = L2(refValues),
            ["residual_l2_norm"] = L2(residual),
            ["recovery_curve"] = curve,
        };
    }

    public static Dictionary<string, object?> AdmitCandidate(
        Dictionary<string, object?> reference,
        Dictionary<string, object?> candidate,
        double maxQualityDelta,
        IEnumerable<string>? structuralReasons = null)
    {
        var delta = Convert.ToDouble(candidate["quality_pass_fraction"])
            - Convert.ToDouble(reference["quality_pass_fraction"]);
        var reasons = (structuralReasons ?? []).ToList();
        if (delta < -Math.Abs(maxQualityDelta))
            reasons.Add("quality_regression");
        reasons = reasons.Distinct().ToList();
        var admitted = reasons.Count == 0;
        return new Dictionary<string, object?>
        {
            ["admitted"] = admitted,
            ["selected_role"] = admitted ? "candidate" : "reference",
            ["quality_delta"] = delta,
            ["reasons"] = reasons,
        };
    }

    public static int RunCli(string[] args)
    {
        // Full campaign is available; parse args and run (or refuse missing artifacts).
        string? reference = null, candidate = null, qgkp = null, server = null, report = null;
        string library = "cnet.so", qgkpCli = "bin/cnet_qgkp";
        string? materializedCache = null;
        var threads = 4;
        var ctxSize = 512;
        var startupTimeout = 120.0;
        var requestTimeout = 90.0;
        var maxQualityDelta = 0.0;
        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--reference": reference = args[++i]; break;
                case "--candidate": candidate = args[++i]; break;
                case "--qgkp": qgkp = args[++i]; break;
                case "--materialized-cache": materializedCache = args[++i]; break;
                case "--server": server = args[++i]; break;
                case "--library": library = args[++i]; break;
                case "--qgkp-cli": qgkpCli = args[++i]; break;
                case "--report": report = args[++i]; break;
                case "--threads": threads = int.Parse(args[++i]); break;
                case "--ctx-size": ctxSize = int.Parse(args[++i]); break;
                case "--startup-timeout": startupTimeout = double.Parse(args[++i]); break;
                case "--request-timeout": requestTimeout = double.Parse(args[++i]); break;
                case "--max-quality-delta": maxQualityDelta = double.Parse(args[++i]); break;
                default: throw new ArgumentException($"unknown argument: {args[i]}");
            }
        }
        if (reference is null || candidate is null || qgkp is null || server is null || report is null)
            throw new ArgumentException("required: --reference --candidate --qgkp --server --report");
        try
        {
            var campaignReport = RunCampaign(new CampaignArgs
            {
                Reference = Path.GetFullPath(reference),
                Candidate = Path.GetFullPath(candidate),
                Qgkp = Path.GetFullPath(qgkp),
                MaterializedCache = materializedCache is null ? null : Path.GetFullPath(materializedCache),
                Server = Path.GetFullPath(server),
                Library = Path.GetFullPath(library),
                QgkpCli = Path.GetFullPath(qgkpCli),
                Report = report,
                Threads = threads,
                CtxSize = ctxSize,
                StartupTimeout = startupTimeout,
                RequestTimeout = requestTimeout,
                MaxQualityDelta = maxQualityDelta,
            });
            var dir = Path.GetDirectoryName(report);
            if (!string.IsNullOrEmpty(dir))
                Directory.CreateDirectory(dir);
            File.WriteAllText(report, JsonSerializer.Serialize(campaignReport, JsonUtil.Indented) + "\n");
            var selected = (Dictionary<string, object?>)campaignReport["selected_model"]!;
            var referenceRun = (Dictionary<string, object?>)campaignReport["reference"]!;
            var candidateRun = (Dictionary<string, object?>)campaignReport["candidate"]!;
            Console.WriteLine(JsonSerializer.Serialize(new Dictionary<string, object?>
            {
                ["verdict"] = campaignReport["verdict"],
                ["selected_role"] = selected["role"],
                ["reference_quality"] = referenceRun["quality_pass_fraction"],
                ["candidate_quality"] = candidateRun["quality_pass_fraction"],
                ["restart_integrity"] = campaignReport["restart_integrity"],
                ["report"] = report,
            }, JsonUtil.Indented));
            return Convert.ToBoolean(campaignReport["overall_pass"]) ? 0 : 1;
        }
        catch (Exception ex)
        {
            Console.WriteLine(JsonSerializer.Serialize(new
            {
                overall_pass = false,
                verdict = "ERROR",
                error = ex.Message,
            }, JsonUtil.Indented));
            return 1;
        }
    }

    public sealed class CampaignArgs
    {
        public required string Reference { get; init; }
        public required string Candidate { get; init; }
        public required string Qgkp { get; init; }
        public string? MaterializedCache { get; init; }
        public required string Server { get; init; }
        public required string Library { get; init; }
        public required string QgkpCli { get; init; }
        public required string Report { get; init; }
        public int Threads { get; init; } = 4;
        public int CtxSize { get; init; } = 512;
        public double StartupTimeout { get; init; } = 120;
        public double RequestTimeout { get; init; } = 90;
        public double MaxQualityDelta { get; init; }
    }

    private static int FreePort()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        return port;
    }

    private static Dictionary<string, object?> RequestJson(string url, object? payload, double timeout)
    {
        using var client = new HttpClient { Timeout = TimeSpan.FromSeconds(timeout) };
        HttpResponseMessage response;
        if (payload is null)
        {
            response = client.GetAsync(url).GetAwaiter().GetResult();
        }
        else
        {
            var json = JsonSerializer.Serialize(payload);
            response = client.PostAsync(url, new StringContent(json, Encoding.UTF8, "application/json"))
                .GetAwaiter().GetResult();
        }
        var text = response.Content.ReadAsStringAsync().GetAwaiter().GetResult();
        if (!response.IsSuccessStatusCode)
            throw new InvalidOperationException($"HTTP {(int)response.StatusCode} from {url}: {text}");
        using var doc = JsonDocument.Parse(text);
        return JsonUtil.ToDict(doc.RootElement)!;
    }

    private static Dictionary<string, object?> ProbeModel(
        string server, string model, string role, double startupTimeout, double requestTimeout, int threads, int ctxSize)
    {
        var port = FreePort();
        var logPath = Path.Combine(Path.GetTempPath(), $"cnet-{role}-{Guid.NewGuid():N}.log");
        var psi = new ProcessStartInfo
        {
            FileName = server,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        foreach (var arg in new[]
                 {
                     "--model", model, "--alias", $"cnet-{role}", "--host", "127.0.0.1",
                     "--port", port.ToString(), "--n-gpu-layers", "0", "--threads", threads.ToString(),
                     "--ctx-size", ctxSize.ToString(), "--parallel", "1", "--no-webui",
                 })
        {
            psi.ArgumentList.Add(arg);
        }
        psi.Environment["CUDA_VISIBLE_DEVICES"] = "";
        psi.Environment["HIP_VISIBLE_DEVICES"] = "";
        psi.Environment["ROCR_VISIBLE_DEVICES"] = "";
        var started = DateTime.UtcNow;
        using var process = Process.Start(psi) ?? throw new InvalidOperationException("failed to start llama-server");
        try
        {
            var deadline = DateTime.UtcNow.AddSeconds(startupTimeout);
            while (true)
            {
                if (process.HasExited)
                    throw new InvalidOperationException($"llama-server exited during {role} startup");
                try
                {
                    var health = RequestJson($"http://127.0.0.1:{port}/health", null, 2);
                    if (Convert.ToString(health.GetValueOrDefault("status")) == "ok")
                        break;
                }
                catch
                {
                    /* retry */
                }
                if (DateTime.UtcNow >= deadline)
                    throw new TimeoutException($"llama-server startup timed out for {role}");
                Thread.Sleep(500);
            }

            var results = new List<Dictionary<string, object?>>();
            foreach (var (probeId, prompt, expected) in Probes)
            {
                var payload = new Dictionary<string, object?>
                {
                    ["messages"] = new[] { new Dictionary<string, object?> { ["role"] = "user", ["content"] = prompt } },
                    ["max_tokens"] = 16,
                    ["temperature"] = 0,
                    ["seed"] = 42,
                    ["chat_template_kwargs"] = new Dictionary<string, object?> { ["enable_thinking"] = false },
                    ["logprobs"] = true,
                    ["top_logprobs"] = 10,
                };
                try
                {
                    var response = RequestJson($"http://127.0.0.1:{port}/v1/chat/completions", payload, requestTimeout);
                    var choices = (List<object?>)response["choices"]!;
                    var choice = (Dictionary<string, object?>)choices[0]!;
                    var message = (Dictionary<string, object?>)choice["message"]!;
                    var content = Convert.ToString(message.GetValueOrDefault("content"))?.Trim() ?? "";
                    var logprobs = choice.GetValueOrDefault("logprobs") as Dictionary<string, object?>;
                    var logprobContent = logprobs?.GetValueOrDefault("content") as List<object?>;
                    var top = new List<Dictionary<string, object?>>();
                    if (logprobContent is { Count: > 0 }
                        && logprobContent[0] is Dictionary<string, object?> first
                        && first.GetValueOrDefault("top_logprobs") is List<object?> tops)
                    {
                        top = tops.OfType<Dictionary<string, object?>>().ToList();
                    }
                    var timings = response.GetValueOrDefault("timings") as Dictionary<string, object?>;
                    results.Add(new Dictionary<string, object?>
                    {
                        ["id"] = probeId,
                        ["expected"] = expected,
                        ["response"] = content,
                        ["passed"] = content == expected,
                        ["top_logprobs"] = top,
                        ["normalized_entropy"] = NormalizedEntropy(top),
                        ["tokens_per_second"] = Convert.ToDouble(timings?.GetValueOrDefault("predicted_per_second") ?? 0.0),
                    });
                }
                catch (Exception ex)
                {
                    results.Add(new Dictionary<string, object?>
                    {
                        ["id"] = probeId,
                        ["expected"] = expected,
                        ["response"] = "",
                        ["passed"] = false,
                        ["error"] = ex.Message,
                        ["top_logprobs"] = new List<Dictionary<string, object?>>(),
                        ["normalized_entropy"] = 0.0,
                        ["tokens_per_second"] = 0.0,
                    });
                }
            }
            var passed = results.Count(r => Convert.ToBoolean(r["passed"]));
            return new Dictionary<string, object?>
            {
                ["role"] = role,
                ["model"] = Path.GetFullPath(model),
                ["startup_seconds"] = (DateTime.UtcNow - started).TotalSeconds,
                ["quality_passed"] = passed,
                ["quality_total"] = results.Count,
                ["quality_pass_fraction"] = passed / (double)results.Count,
                ["probes"] = results,
            };
        }
        finally
        {
            try
            {
                if (!process.HasExited)
                    process.Kill(entireProcessTree: true);
            }
            catch { /* ignore */ }
            try { File.Delete(logPath); } catch { /* ignore */ }
        }
    }

    public static Dictionary<string, object?> RunCampaign(CampaignArgs args)
    {
        RequireMagic(args.Reference, "GGUF"u8);
        RequireMagic(args.Candidate, "GGUF"u8);
        RequireMagic(args.Qgkp, "QGKP"u8);
        var referenceQuant = InspectGguf(args.Reference);
        var candidateQuant = InspectGguf(args.Candidate);
        var structureReasons = StructuralMismatchReasons(referenceQuant, candidateQuant);
        var quantizationEvidence = QuantizationDiagnosis(referenceQuant, candidateQuant);
        if (!File.Exists(args.Server))
            throw new ArgumentException($"llama-server is not executable: {args.Server}");
        if (!File.Exists(args.Library))
            throw new ArgumentException($"CNET library not found: {args.Library}");
        if (!File.Exists(args.QgkpCli))
            throw new ArgumentException($"QGKP CLI is not executable: {args.QgkpCli}");

        var inspectionPsi = new ProcessStartInfo
        {
            FileName = args.QgkpCli,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        inspectionPsi.ArgumentList.Add("inspect");
        inspectionPsi.ArgumentList.Add(args.Qgkp);
        using var inspectionProc = Process.Start(inspectionPsi)
            ?? throw new InvalidOperationException("failed to start qgkp cli");
        var inspectionOut = inspectionProc.StandardOutput.ReadToEnd();
        inspectionProc.WaitForExit(30_000);
        if (inspectionProc.ExitCode != 0)
            throw new InvalidOperationException("qgkp inspect failed");
        var inspection = inspectionOut.Trim();
        var referenceSha = Sha256File(args.Reference);
        Dictionary<string, object?>? qgkpRoundTrip = null;
        if (args.MaterializedCache is not null)
        {
            RequireMagic(args.MaterializedCache, "GGUF"u8);
            var cacheSha = Sha256File(args.MaterializedCache);
            qgkpRoundTrip = new Dictionary<string, object?>
            {
                ["cache"] = args.MaterializedCache,
                ["cache_sha256"] = cacheSha,
                ["byte_identical"] = cacheSha == referenceSha
                    && new FileInfo(args.MaterializedCache).Length == new FileInfo(args.Reference).Length,
            };
        }

        var referenceRun = ProbeModel(args.Server, args.Reference, "reference", args.StartupTimeout, args.RequestTimeout, args.Threads, args.CtxSize);
        var candidateRun = ProbeModel(args.Server, args.Candidate, "candidate", args.StartupTimeout, args.RequestTimeout, args.Threads, args.CtxSize);
        var admission = AdmitCandidate(referenceRun, candidateRun, args.MaxQualityDelta, structureReasons);
        var selectedModel = Convert.ToBoolean(admission["admitted"]) ? args.Candidate : args.Reference;
        var selectedInitial = Convert.ToBoolean(admission["admitted"]) ? candidateRun : referenceRun;
        var restartRun = ProbeModel(args.Server, selectedModel, "selected-restart", args.StartupTimeout, args.RequestTimeout, args.Threads, args.CtxSize);

        var refProbes = ((List<Dictionary<string, object?>>)referenceRun["probes"]!);
        var candProbes = ((List<Dictionary<string, object?>>)candidateRun["probes"]!);
        var distributions = new List<Dictionary<string, object?>>();
        for (var i = 0; i < refProbes.Count; i++)
        {
            var metrics = DistributionMetrics(
                (List<Dictionary<string, object?>>)refProbes[i]["top_logprobs"]!,
                (List<Dictionary<string, object?>>)candProbes[i]["top_logprobs"]!);
            metrics["id"] = refProbes[i]["id"];
            distributions.Add(metrics);
        }

        var restartProbes = (List<Dictionary<string, object?>>)restartRun["probes"]!;
        var selectedProbes = (List<Dictionary<string, object?>>)selectedInitial["probes"]!;
        var restartMatch = selectedProbes.Zip(restartProbes, (a, b) =>
            Convert.ToString(a["response"]) == Convert.ToString(b["response"])).All(x => x);
        var referenceOperational = Convert.ToDouble(referenceRun["quality_pass_fraction"]) > 0;
        var selectedQualityPreserved =
            Convert.ToDouble(restartRun["quality_pass_fraction"])
            >= Convert.ToDouble(selectedInitial["quality_pass_fraction"]);
        var roundTripOk = qgkpRoundTrip is null || Convert.ToBoolean(qgkpRoundTrip["byte_identical"]);
        var overallPass = referenceOperational && selectedQualityPreserved && restartMatch && roundTripOk;
        var admitted = Convert.ToBoolean(admission["admitted"]);

        return new Dictionary<string, object?>
        {
            ["schema_version"] = 2,
            ["generated_at_utc"] = DateTime.UtcNow.ToString("O"),
            ["execution"] = new Dictionary<string, object?>
            {
                ["cpu_only"] = true,
                ["server"] = args.Server,
                ["threads"] = args.Threads,
                ["ctx_size"] = args.CtxSize,
                ["max_quality_delta"] = args.MaxQualityDelta,
            },
            ["artifacts"] = new Dictionary<string, object?>
            {
                ["reference"] = new Dictionary<string, object?>
                {
                    ["path"] = args.Reference,
                    ["bytes"] = new FileInfo(args.Reference).Length,
                    ["sha256"] = referenceSha,
                    ["gguf"] = referenceQuant,
                },
                ["candidate"] = new Dictionary<string, object?>
                {
                    ["path"] = args.Candidate,
                    ["bytes"] = new FileInfo(args.Candidate).Length,
                    ["sha256"] = Sha256File(args.Candidate),
                    ["storage_ratio_vs_reference"] =
                        new FileInfo(args.Candidate).Length / (double)new FileInfo(args.Reference).Length,
                    ["gguf"] = candidateQuant,
                },
                ["qgkp"] = new Dictionary<string, object?>
                {
                    ["path"] = args.Qgkp,
                    ["bytes"] = new FileInfo(args.Qgkp).Length,
                    ["inspection"] = inspection,
                    ["round_trip"] = qgkpRoundTrip,
                },
            },
            ["reference"] = referenceRun,
            ["candidate"] = candidateRun,
            ["quantization_diagnosis"] = quantizationEvidence,
            ["admission"] = admission,
            ["selected_model"] = new Dictionary<string, object?>
            {
                ["role"] = admission["selected_role"],
                ["path"] = selectedModel,
            },
            ["selected_restart"] = restartRun,
            ["restart_integrity"] = new Dictionary<string, object?>
            {
                ["responses_identical"] = restartMatch,
                ["quality_preserved"] = selectedQualityPreserved,
            },
            ["distribution_evidence"] = distributions,
            ["overall_pass"] = overallPass,
            ["verdict"] = overallPass && admitted
                ? "PASS_CANDIDATE_ADMITTED"
                : overallPass ? "PASS_CANDIDATE_QUARANTINED" : "FAIL",
        };
    }
}
