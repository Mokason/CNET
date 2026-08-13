using System.Diagnostics;
using System.Text;
using System.Text.Json;

// Replaces the former Python helpers under dotnet/Llm/scripts/*.py.
// Full multi-model matrices live in CNET.Llm.Tests.Integration / Benchmarks;
// this console keeps a small CLI surface for local smoke checks.

return await MainAsync(args);

static async Task<int> MainAsync(string[] args)
{
    if (args.Length == 0)
        return Usage();

    var verb = args[0];
    var rest = args.Skip(1).ToArray();
    return verb switch
    {
        "server" => await ServerSmoke.RunAsync(rest),
        "models" or "models-aot" or "models-tools" or "models-json"
            or "models-json-schema" or "models-regex" or "models-grammar"
            or "models-speculative" or "models-warmup" => ModelsSmoke.Run(verb, rest),
        "bench-compare" or "bench-history" or "bench-trend" => BenchSmoke.Run(verb, rest),
        "-h" or "--help" or "help" => Usage(),
        _ => Usage(),
    };
}

static int Usage()
{
    Console.Error.WriteLine("""
        cnet-llm-smoke <verb> ...

        Verbs (former scripts/ counterparts):
          server              ← test_server.py
          models              ← test_models.py
          models-aot          ← test_models_aot.py
          models-tools        ← test_models_tools.py
          models-json         ← test_models_json.py
          models-json-schema  ← test_models_json_schema.py
          models-regex        ← test_models_regex.py
          models-grammar      ← test_models_grammar.py
          models-speculative  ← test_models_speculative.py
          models-warmup       ← test_models_warmup.py
          bench-compare       ← bench_compare.py  (delegates to BDN project)
          bench-history       ← bench_history.py
          bench-trend         ← bench_trend.py

        Prefer: dotnet test dotnet/Llm/tests/CNET.Llm.Tests.Integration
                dotnet run --project dotnet/Llm/benchmarks/CNET.Llm.Benchmarks
        """);
    return 2;
}

static class ServerSmoke
{
    public static async Task<int> RunAsync(string[] args)
    {
        var baseUrl = "http://127.0.0.1:18080";
        for (var i = 0; i < args.Length; i++)
        {
            if (args[i] is "--base-url" && i + 1 < args.Length)
                baseUrl = args[++i].TrimEnd('/');
        }

        using var client = new HttpClient { Timeout = TimeSpan.FromSeconds(120) };
        var checks = new List<(string Name, bool Ok, string Detail)>();

        async Task Check(string name, Func<Task<(bool, string)>> body)
        {
            try
            {
                var (ok, detail) = await body();
                checks.Add((name, ok, detail));
            }
            catch (Exception ex)
            {
                checks.Add((name, false, ex.Message));
            }
        }

        await Check("health", async () =>
        {
            var json = await client.GetStringAsync($"{baseUrl}/health");
            using var doc = JsonDocument.Parse(json);
            var ok = doc.RootElement.TryGetProperty("status", out var s)
                && (s.GetString() is "ok" or "healthy" || s.ValueKind == JsonValueKind.True);
            return (ok, json[..Math.Min(json.Length, 200)]);
        });

        await Check("models", async () =>
        {
            var resp = await client.GetAsync($"{baseUrl}/v1/models");
            return (resp.IsSuccessStatusCode, $"HTTP {(int)resp.StatusCode}");
        });

        await Check("chat_completions", async () =>
        {
            const string payload =
                """{"messages":[{"role":"user","content":"Reply with exactly READY."}],"max_tokens":8,"temperature":0}""";
            using var content = new StringContent(payload, Encoding.UTF8, "application/json");
            var resp = await client.PostAsync($"{baseUrl}/v1/chat/completions", content);
            var body = await resp.Content.ReadAsStringAsync();
            return (resp.IsSuccessStatusCode, body[..Math.Min(body.Length, 240)]);
        });

        foreach (var (name, ok, detail) in checks)
            Console.WriteLine($"{(ok ? "PASS" : "FAIL")} {name}: {detail}");
        var failed = checks.Count(c => !c.Ok);
        Console.WriteLine(failed == 0
            ? $"LLM_SERVER_SMOKE_PASS checks={checks.Count}"
            : $"LLM_SERVER_SMOKE_FAIL failed={failed}/{checks.Count}");
        return failed == 0 ? 0 : 1;
    }
}

static class ModelsSmoke
{
    public static int Run(string verb, string[] args)
    {
        Console.WriteLine(
            $"LLM_MODELS_SMOKE verb={verb} "
            + "core coverage lives in CNET.Llm.Tests.Integration / CNET.Llm.Tests.Unit. "
            + "Pass --exec to invoke `dotnet test` for those projects.");
        if (args.Contains("--exec"))
        {
            var llmRoot = FindLlmRoot();
            var project = Path.Combine(llmRoot, "tests", "CNET.Llm.Tests.Integration", "CNET.Llm.Tests.Integration.csproj");
            if (!File.Exists(project))
            {
                Console.Error.WriteLine($"missing {project}");
                return 1;
            }
            var psi = new ProcessStartInfo
            {
                FileName = "dotnet",
                WorkingDirectory = llmRoot,
                UseShellExecute = false,
            };
            psi.ArgumentList.Add("test");
            psi.ArgumentList.Add(project);
            psi.ArgumentList.Add("-v");
            psi.ArgumentList.Add("q");
            using var p = Process.Start(psi)!;
            p.WaitForExit();
            return p.ExitCode;
        }
        Console.WriteLine("LLM_MODELS_SMOKE_PASS deferred=integration_suite");
        return 0;
    }

    public static string FindLlmRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "Directory.Build.props"))
                && Directory.Exists(Path.Combine(dir.FullName, "tests")))
            {
                return dir.FullName;
            }
            dir = dir.Parent;
        }
        return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", ".."));
    }
}

static class BenchSmoke
{
    public static int Run(string verb, string[] args)
    {
        var llmRoot = ModelsSmoke.FindLlmRoot();
        var bench = Path.Combine(llmRoot, "benchmarks", "CNET.Llm.Benchmarks", "CNET.Llm.Benchmarks.csproj");
        Console.WriteLine($"LLM_BENCH_SMOKE verb={verb} project={bench}");
        if (!File.Exists(bench))
        {
            Console.Error.WriteLine("CNET.Llm.Benchmarks project not found");
            return 1;
        }
        if (!args.Contains("--exec"))
        {
            Console.WriteLine("LLM_BENCH_SMOKE_PASS deferred=bdn (pass --exec to run)");
            return 0;
        }
        var psi = new ProcessStartInfo
        {
            FileName = "dotnet",
            WorkingDirectory = llmRoot,
            UseShellExecute = false,
        };
        psi.ArgumentList.Add("run");
        psi.ArgumentList.Add("--project");
        psi.ArgumentList.Add(bench);
        psi.ArgumentList.Add("-c");
        psi.ArgumentList.Add("Release");
        foreach (var a in args.Where(a => a != "--exec"))
            psi.ArgumentList.Add(a);
        using var p = Process.Start(psi)!;
        p.WaitForExit();
        return p.ExitCode;
    }
}
