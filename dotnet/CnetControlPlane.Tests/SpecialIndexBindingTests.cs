using System.Diagnostics;
using CnetControlPlane.Capability;

namespace CnetControlPlane.Tests;

/// <summary>
/// Core special-index binding behaviour from tests/test_evidence_special_index.py
/// (worktree binding sees assume-unchanged mutations; symlinks bound by target).
/// </summary>
public class SpecialIndexBindingTests
{
    private static void Git(string repo, params string[] args)
    {
        var psi = new ProcessStartInfo
        {
            FileName = "git",
            WorkingDirectory = repo,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        foreach (var a in args)
            psi.ArgumentList.Add(a);
        using var p = Process.Start(psi)!;
        p.WaitForExit(60_000);
        if (p.ExitCode != 0)
            throw new InvalidOperationException($"git {string.Join(' ', args)} failed: {p.StandardError.ReadToEnd()}");
    }

    [Fact]
    public void Assume_unchanged_mutation_changes_special_index_digest()
    {
        var tmp = Directory.CreateTempSubdirectory("cnet-special-").FullName;
        try
        {
            var repo = Path.Combine(tmp, "repo");
            Directory.CreateDirectory(Path.Combine(repo, "src"));
            File.WriteAllText(Path.Combine(repo, "src", "watched.c"), "int watched(void){return 1;}\n");
            File.WriteAllText(Path.Combine(repo, ".gitignore"), "logs/\n");
            Git(repo, "init", "-q", "-b", "main");
            Git(repo, "config", "user.email", "integrity@example.invalid");
            Git(repo, "config", "user.name", "integrity");
            Git(repo, "add", "-A");
            Git(repo, "commit", "-q", "-m", "fixture");
            Git(repo, "update-index", "--assume-unchanged", "src/watched.c");

            var (before, files, _) = CapabilityCertRunner.SpecialIndexBinding(repo);
            Assert.True(files >= 1);
            File.WriteAllText(Path.Combine(repo, "src", "watched.c"), "/* rewritten */\n");
            var (after, _, _) = CapabilityCertRunner.SpecialIndexBinding(repo);
            Assert.NotEqual(before, after);

            // Premise: git status is blind.
            var statusPsi = new ProcessStartInfo
            {
                FileName = "git",
                WorkingDirectory = repo,
                RedirectStandardOutput = true,
                UseShellExecute = false,
            };
            statusPsi.ArgumentList.Add("status");
            statusPsi.ArgumentList.Add("--porcelain=v1");
            using var status = Process.Start(statusPsi)!;
            var porcelain = status.StandardOutput.ReadToEnd().Trim();
            status.WaitForExit();
            Assert.Equal("", porcelain);
        }
        finally { try { Directory.Delete(tmp, true); } catch { /* git locks on Windows */ } }
    }

    [Fact]
    public void Symlink_is_bound_by_identity_not_target_bytes()
    {
        if (OperatingSystem.IsWindows() && !IsDeveloperModeOrAdmin())
        {
            // Symlink creation may require elevation; skip rather than false red.
            return;
        }
        var tmp = Directory.CreateTempSubdirectory("cnet-symlink-").FullName;
        try
        {
            var repo = Path.Combine(tmp, "repo");
            Directory.CreateDirectory(Path.Combine(repo, "src"));
            File.WriteAllText(Path.Combine(repo, "src", "watched.c"), "int x;\n");
            Git(repo, "init", "-q", "-b", "main");
            Git(repo, "config", "user.email", "integrity@example.invalid");
            Git(repo, "config", "user.name", "integrity");
            var link = Path.Combine(repo, "src", "pointer");
            File.CreateSymbolicLink(link, OperatingSystem.IsWindows() ? @"C:\Windows\System32\drivers\etc\hosts" : "/dev/zero");
            Git(repo, "add", "-A");
            Git(repo, "commit", "-q", "-m", "fixture");
            Git(repo, "update-index", "--assume-unchanged", "src/pointer");
            var (zeroDigest, _, _) = CapabilityCertRunner.SpecialIndexBinding(repo);
            File.Delete(link);
            File.CreateSymbolicLink(link, OperatingSystem.IsWindows() ? @"C:\Windows\System32\drivers\etc\protocol" : "/dev/null");
            var (nullDigest, _, _) = CapabilityCertRunner.SpecialIndexBinding(repo);
            Assert.NotEqual(zeroDigest, nullDigest);
        }
        finally { try { Directory.Delete(tmp, true); } catch { /* git locks on Windows */ } }
    }

    private static bool IsDeveloperModeOrAdmin()
    {
        try
        {
            var probe = Path.Combine(Path.GetTempPath(), $"cnet-symlink-probe-{Guid.NewGuid():N}");
            File.CreateSymbolicLink(probe, Path.GetTempPath());
            File.Delete(probe);
            return true;
        }
        catch
        {
            return false;
        }
    }
}
