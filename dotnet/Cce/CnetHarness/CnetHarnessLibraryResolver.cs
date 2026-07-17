// Deterministic native library resolver for the CNET .NET harness.
//
// When the process needs to P/Invoke into libcnet_harness.so, this resolver
// decides where to look before hitting the default operating-system search
// order. It only handles the harness library name; any other library name
// returns IntPtr.Zero so the normal loader keeps its behavior for
// unrelated assemblies.
//
// Candidate order:
//   1) CNET_HARNESS_LIBRARY (full path override, deterministic)
//   2) $CNET_HARNESS_BIN_DIR / libcnet_harness.so
//   3) AppBase / libcnet_harness.so
//   4) AppBase / bin / libcnet_harness.so
//   5) AppBase / .. / bin / libcnet_harness.so
//   6) Fall through to the OS default (returns 0 -> resolver chain continues)
//
// The resolver is idempotently registered exactly once per AppDomain.

using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;

namespace CNET.Cce.CnetHarness;

internal static class CnetHarnessLibraryResolver
{
    internal const string LibraryName = "cnet_harness";
    internal const string LibraryFileName = "libcnet_harness.so";
    internal const string EnvOverride = "CNET_HARNESS_LIBRARY";
    internal const string EnvBinDir = "CNET_HARNESS_BIN_DIR";

    private static int _registered;

    static CnetHarnessLibraryResolver()
    {
        EnsureRegistered();
    }

    internal static void EnsureRegistered()
    {
        if (System.Threading.Interlocked.CompareExchange(ref _registered, 1, 0) != 0)
            return;
        try
        {
            NativeLibrary.SetDllImportResolver(
                typeof(CnetHarnessLibraryResolver).Assembly,
                Resolve);
        }
        catch
        {
            System.Threading.Volatile.Write(ref _registered, 0);
            throw;
        }
    }

    private static IntPtr Resolve(string libraryName, Assembly assembly,
                                   DllImportSearchPath? searchPath)
    {
        if (!string.Equals(libraryName, LibraryName, StringComparison.Ordinal))
        {
            /* Only handle our own library; unrelated names fall through. */
            return IntPtr.Zero;
        }

        string? overridePath = Environment.GetEnvironmentVariable(EnvOverride);
        if (!string.IsNullOrWhiteSpace(overridePath))
        {
            if (!Path.IsPathFullyQualified(overridePath))
            {
                throw new DllNotFoundException(
                    $"{EnvOverride} must be an absolute path; got '{overridePath}'.");
            }
            if (NativeLibrary.TryLoad(overridePath, out IntPtr overrideHandle))
            {
                return overrideHandle;
            }
            throw new DllNotFoundException(
                $"{EnvOverride} was set but could not be loaded: '{overridePath}'.");
        }

        foreach (string candidate in EnumerateCandidates(Environment.GetEnvironmentVariable))
        {
            if (NativeLibrary.TryLoad(candidate, out IntPtr handle))
            {
                return handle;
            }
        }
        /* Let the OS loader try LD_LIBRARY_PATH / RPATH as a last resort. */
        return NativeLibrary.TryLoad(LibraryFileName, out IntPtr fallback)
            ? fallback : IntPtr.Zero;
    }

    /// <summary>
    /// Pure, testable candidate enumeration. Paths are derived from
    /// AppContext.BaseDirectory or an env-selected bin directory.
    /// </summary>
    internal static string[] EnumerateCandidates(Func<string, string?> getEnv)
    {
        var list = new System.Collections.Generic.List<string>(4);

        string? binDir = getEnv(EnvBinDir);
        if (!string.IsNullOrEmpty(binDir))
        {
            list.Add(Path.Combine(binDir, LibraryFileName));
        }

        string baseDir = AppContext.BaseDirectory;
        if (!string.IsNullOrEmpty(baseDir))
        {
            list.Add(Path.Combine(baseDir, LibraryFileName));
            list.Add(Path.Combine(baseDir, "bin", LibraryFileName));
            string? parent = Path.GetDirectoryName(baseDir.TrimEnd(Path.DirectorySeparatorChar));
            if (!string.IsNullOrEmpty(parent))
            {
                list.Add(Path.Combine(parent, "bin", LibraryFileName));
            }
        }

        return list.ToArray();
    }
}
