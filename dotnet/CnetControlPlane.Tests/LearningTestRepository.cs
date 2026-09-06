using System.Runtime.CompilerServices;

namespace CnetControlPlane.Tests;

// Discovery of already-built test prerequisites only: no runtime installation,
// environment overrides, builds, or production authority is granted here.
internal static class LearningTestRepository
{
    private static readonly string[] NativeNames =
        ["cnet_table_capsule", "cnet_table_verify", "cnet_learning_snapshot", "cnet_capsulectl", "cnetd", "libcnet_capsule_core.so"];

    internal static string RequireBuilt(IReadOnlyList<string> requiredNativeNames, [CallerFilePath] string callerFile = "") =>
        RequireBuiltFrom(requiredNativeNames, Directory.GetCurrentDirectory(), AppContext.BaseDirectory, callerFile);

    // Explicit origins keep lookup tests deterministic without changing the
    // testhost's process-wide CWD. CallerFilePath handles an external output
    // directory while the original source checkout remains available.
    internal static string RequireBuiltFrom(IReadOnlyList<string> requiredNativeNames,
        string currentDirectory, string baseDirectory, string callerFile)
    {
        if (requiredNativeNames.Count is < 1 or > 6 || requiredNativeNames.Any(name => !NativeNames.Contains(name)))
            throw new ArgumentException("learning_test_native_names");
        foreach (var origin in new[] { currentDirectory, baseDirectory, Path.GetDirectoryName(callerFile) })
        {
            if (origin is null || !Path.IsPathFullyQualified(origin)) continue;
            for (var cursor = new DirectoryInfo(origin); cursor is not null; cursor = cursor.Parent)
            {
                if (!File.Exists(Path.Combine(cursor.FullName, "dotnet", "CnetControlPlane.Tests", "CnetControlPlane.Tests.csproj"))) continue;
                // Once a checkout is selected, do not borrow missing binaries
                // from another checkout or assemble a mixed-worktree runtime.
                foreach (var name in requiredNativeNames)
                    if (!File.Exists(Path.Combine(cursor.FullName, "bin", name)))
                        throw new InvalidOperationException("learning_test_native_prerequisite_missing: " + name);
                return cursor.FullName;
            }
        }
        throw new InvalidOperationException("learning_test_repository_not_found");
    }
}
