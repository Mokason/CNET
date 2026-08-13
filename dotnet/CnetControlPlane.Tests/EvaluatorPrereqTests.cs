using System.Text.Json;
using CnetControlPlane.Capability;

namespace CnetControlPlane.Tests;

public class EvaluatorPrereqTests
{
    private static JsonElement ManifestFor(
        string binary = "bin/evaluator",
        string[]? sources = null,
        string[]? evaluator = null,
        string[]? prepare = null)
    {
        var body = new Dictionary<string, object?>
        {
            ["capability_id"] = "fixture_capability",
            ["evaluator"] = (evaluator ?? ["dotnet", "test", "p"]).Cast<object?>().ToList(),
            ["evaluator_sources"] = (sources ?? ["src/evaluator.c"]).Cast<object?>().ToList(),
            ["evaluator_binary"] = binary,
        };
        if (prepare is not null)
            body["evaluator_prepare"] = prepare.Cast<object?>().ToList();
        return JsonDocument.Parse(JsonSerializer.Serialize(body)).RootElement.Clone();
    }

    [Fact]
    public void Non_building_evaluator_without_prepare_is_refused()
    {
        var problems = EvaluatorPrereq.DeclarationProblems(ManifestFor());
        Assert.Contains(problems, p => p.Contains("evaluator_prepare"));
    }

    [Fact]
    public void Make_evaluator_needs_no_prepare()
    {
        Assert.Empty(EvaluatorPrereq.DeclarationProblems(
            ManifestFor(evaluator: ["make", "some_target"])));
    }

    [Fact]
    public void Shell_prepare_is_refused()
    {
        var problems = EvaluatorPrereq.DeclarationProblems(
            ManifestFor(prepare: ["sh", "-c", "touch /tmp/pwned"]));
        Assert.Contains(problems, p => p.Contains("allowlisted"));
    }

    [Fact]
    public void Prepare_without_a_declared_binary_is_refused()
    {
        var json = """{"capability_id":"fixture_capability","evaluator":["dotnet","test","p"],"evaluator_sources":["src/evaluator.c"],"evaluator_prepare":["make","thing"]}""";
        using var doc = JsonDocument.Parse(json);
        var problems = EvaluatorPrereq.DeclarationProblems(doc.RootElement);
        Assert.Contains(problems, p => p.Contains("evaluator_binary"));
    }

    [Fact]
    public void Empty_prepare_argv_is_refused()
    {
        var problems = EvaluatorPrereq.DeclarationProblems(ManifestFor(prepare: []));
        Assert.NotEmpty(problems);
    }

    [Fact]
    public void Committed_manifests_satisfy_the_rule()
    {
        var root = CnetControlPlane.RepoPaths.FindRoot();
        var manifestDir = Path.Combine(root, "config", "capability_manifests");
        var manifests = Directory.GetFiles(manifestDir, "*.json");
        Assert.NotEmpty(manifests);
        foreach (var path in manifests)
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            Assert.Empty(EvaluatorPrereq.DeclarationProblems(doc.RootElement));
        }
    }

    [Fact]
    public void Prepare_that_fails_is_refused()
    {
        var root = Directory.CreateTempSubdirectory("cnet-prereq-").FullName;
        try
        {
            Directory.CreateDirectory(Path.Combine(root, "src"));
            Directory.CreateDirectory(Path.Combine(root, "bin"));
            File.WriteAllText(Path.Combine(root, "src", "evaluator.c"), "int main(void){return 0;}\n");
            File.WriteAllText(Path.Combine(root, "Makefile"), "build:\n\t@echo ok\n");
            var problems = EvaluatorPrereq.EnsureReady(root, ManifestFor(prepare: ["make", "definitely_missing_target"]));
            Assert.Contains(problems, p => p.Contains("evaluator_prepare failed"));
        }
        finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void Prepare_that_produces_nothing_is_refused()
    {
        var root = Directory.CreateTempSubdirectory("cnet-prereq2-").FullName;
        try
        {
            Directory.CreateDirectory(Path.Combine(root, "src"));
            Directory.CreateDirectory(Path.Combine(root, "bin"));
            File.WriteAllText(Path.Combine(root, "src", "evaluator.c"), "int main(void){return 0;}\n");
            File.WriteAllText(Path.Combine(root, "Makefile"), OperatingSystem.IsWindows()
                ? "build:\n\t@echo ok\n"
                : "build:\n\t@true\n");
            var problems = EvaluatorPrereq.EnsureReady(root, ManifestFor(prepare: ["make", "build"]));
            Assert.Contains(problems, p => p.Contains("absent after prepare"));
        }
        finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void Stale_binary_is_refused()
    {
        var root = Directory.CreateTempSubdirectory("cnet-prereq3-").FullName;
        try
        {
            Directory.CreateDirectory(Path.Combine(root, "src"));
            Directory.CreateDirectory(Path.Combine(root, "bin"));
            File.WriteAllText(Path.Combine(root, "src", "evaluator.c"), "int main(void){return 0;}\n");
            File.WriteAllText(Path.Combine(root, "Makefile"), OperatingSystem.IsWindows()
                ? "build:\n\t@echo ok\n"
                : "build:\n\t@true\n");
            var binary = Path.Combine(root, "bin", "evaluator");
            File.WriteAllText(binary, "stale\n");
            var source = Path.Combine(root, "src", "evaluator.c");
            File.SetLastWriteTimeUtc(binary, DateTime.UnixEpoch.AddSeconds(1000));
            File.SetLastWriteTimeUtc(source, DateTime.UnixEpoch.AddSeconds(2000));
            var problems = EvaluatorPrereq.EnsureReady(root, ManifestFor(prepare: ["make", "build"]));
            Assert.Contains(problems, p => p.Contains("older than its declared source"));
        }
        finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void Fresh_binary_is_accepted()
    {
        var root = Directory.CreateTempSubdirectory("cnet-prereq4-").FullName;
        try
        {
            Directory.CreateDirectory(Path.Combine(root, "src"));
            Directory.CreateDirectory(Path.Combine(root, "bin"));
            File.WriteAllText(Path.Combine(root, "src", "evaluator.c"), "int main(void){return 0;}\n");
            File.WriteAllText(Path.Combine(root, "Makefile"), OperatingSystem.IsWindows()
                ? "build:\n\t@copy /Y src\\evaluator.c bin\\evaluator >NUL\n"
                : "build:\n\t@cp src/evaluator.c bin/evaluator\n");
            Assert.Empty(EvaluatorPrereq.EnsureReady(root, ManifestFor(prepare: ["make", "build"])));
        }
        finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void Dotnet_prepare_is_pinned_to_an_empty_local_source()
    {
        var root = Directory.CreateTempSubdirectory("cnet-prep-iso-").FullName;
        try
        {
            var argv = EvaluatorPrereq.NugetIsolationArgv(["dotnet", "build", "x.csproj"], root);
            var joined = string.Join(' ', argv);
            Assert.Contains("-p:NuGetAudit=false", argv);
            Assert.Contains("-p:RestoreConfigFile=", joined);
            Assert.Contains("-p:RestoreSources=", joined);
            var config = argv.First(a => a.StartsWith("-p:RestoreConfigFile=")).Split('=', 2)[1];
            var source = argv.First(a => a.StartsWith("-p:RestoreSources=")).Split('=', 2)[1];
            Assert.True(File.Exists(config));
            Assert.True(Directory.Exists(source));
            Assert.Empty(Directory.EnumerateFileSystemEntries(source));
            var body = File.ReadAllText(config);
            Assert.DoesNotContain("nuget.org", body);
            Assert.DoesNotContain("http://", body);
            Assert.DoesNotContain("https://", body);
        }
        finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void Make_prepare_gets_no_nuget_flags()
    {
        Assert.Empty(EvaluatorPrereq.NugetIsolationArgv(["make", "target"], "/tmp"));
    }

    [Fact]
    public void Unknown_capability_is_refused()
    {
        var root = CnetControlPlane.RepoPaths.FindRoot();
        var code = EvaluatorPrereq.RunCli(["no_such_capability"], root);
        Assert.Equal(1, code);
    }
}
