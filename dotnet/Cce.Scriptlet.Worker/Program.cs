using System.Buffers.Binary;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static class Program
{
    private sealed record Request(string Source, string Input, bool CompileOnly);

    [DllImport("libcnet-scriptlet-seal.so", EntryPoint = "cnet_scriptlet_seal")]
    private static extern int Seal();

    private static int Main()
    {
        // No request bytes, source parsing, compilation or assembly loading
        // precedes the OS seal. Landlock was inherited from the native launcher.
        if (!OperatingSystem.IsLinux() || RuntimeInformation.ProcessArchitecture != Architecture.X64)
            return 120;
        try
        {
            // CLR's lazy resolver uses path metadata, which the final filter
            // denies. Preload only the trusted, deployment-bound TPA inventory
            // while Landlock is already active and no request has been read.
            string[] platform = ((string?)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") ?? "")
                .Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries);
            if (platform.Length is < 1 or > 256) return 121;
            string[] preload = ["Microsoft.CodeAnalysis", "Microsoft.CodeAnalysis.CSharp",
                "System.Text.Json", "System.Text.Encodings.Web", "System.Collections.Immutable",
                "System.Reflection.Metadata", "System.Collections.Concurrent", "System.Linq",
                "System.Linq.Expressions", "System.Collections", "System.Runtime", "System.Runtime.Loader",
                "System.Runtime.InteropServices", "System.Runtime.Numerics", "System.Text.RegularExpressions",
                "System.Text.Encoding.Extensions", "System.Security.Cryptography", "System.Memory",
                "System.Diagnostics.StackTrace", "System.Diagnostics.Debug", "System.Diagnostics.TraceSource",
                "System.Reflection.Emit.Lightweight", "System.Reflection.Emit.ILGeneration", "System.Threading",
                "System.Threading.Tasks", "System.Threading.Thread", "System.Threading.ThreadPool",
                "System.Threading.Tasks.Parallel", "System.Console", "System.ComponentModel.Primitives",
                "System.Diagnostics.Process", "System.Net.Sockets", "System.Net.Primitives", "netstandard",
                "System.Numerics.Vectors", "System.ObjectModel", "System.Private.Uri", "System.Private.Xml",
                "System.Private.Xml.Linq", "System.IO.FileSystem", "System.IO.MemoryMappedFiles",
                "System.Reflection.Primitives", "System.Xml.XDocument", "System.Xml.ReaderWriter"];
            foreach (string assembly in platform.Where(path => preload.Contains(Path.GetFileNameWithoutExtension(path), StringComparer.Ordinal)))
                if (!AssemblyLoadContext.Default.Assemblies.Any(loaded => loaded.Location == assembly))
                    AssemblyLoadContext.Default.LoadFromAssemblyPath(assembly);
            _ = System.Security.Cryptography.RandomNumberGenerator.GetBytes(1);
            // Console's native signal thread must also exist before clone is
            // denied. An empty write initializes it without protocol output.
            using (Stream console = Console.OpenStandardOutput())
                console.Write(ReadOnlySpan<byte>.Empty);
            // Runtime-owned threads are created before TSYNC; untrusted source
            // cannot create additional threads, processes or anonymous files.
            if (!ThreadPool.SetMinThreads(2, 2) || !ThreadPool.SetMaxThreads(2, 2)) return 121;
            using var started = new CountdownEvent(2);
            using var finished = new CountdownEvent(2);
            using var release = new ManualResetEventSlim();
            for (int i = 0; i < 2; i++)
                ThreadPool.QueueUserWorkItem(_ => { started.Signal(); release.Wait(); finished.Signal(); });
            if (!started.Wait(2000)) return 121;
            release.Set();
            if (!finished.Wait(2000)) return 121;
            if (Seal() != 0) return 121;
        }
        catch { return 121; }
        try { return Execute(); }
        catch { return 123; }
    }

    private static int Execute()
    {
        using Stream input = Console.OpenStandardInput();
        using Stream output = Console.OpenStandardOutput();
        Span<byte> header = stackalloc byte[4];
        input.ReadExactly(header);
        int length = BinaryPrimitives.ReadInt32LittleEndian(header);
        if (length is < 1 or > 65536) return 124;
        byte[] bytes = new byte[length]; input.ReadExactly(bytes);
        if (input.ReadByte() != -1) return 124;
        Request? request = JsonSerializer.Deserialize<Request>(bytes);
        if (request is null || request.Source is null || request.Input is null ||
            request.Source.Length > 4000 || request.Input.Length > 8192) return 124;

        // The host guard is policy, not the security boundary. Even source that
        // bypasses it compiles/runs only after Landlock + seccomp are enforced.
        string source = "using System; using System.Linq; using System.Text; " +
            "using System.Collections.Generic; using System.Text.RegularExpressions; " +
            "public static class Scriptlet { public static string Transform(string input) {\n" +
            request.Source + "\n} }";
        string tpa = (string)(AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") ?? "");
        string[] allowed = ["System.Private.CoreLib.dll", "System.Runtime.dll",
            "System.Linq.dll", "System.Collections.dll", "System.Text.RegularExpressions.dll",
            "System.Text.Encoding.dll", "netstandard.dll", "System.Runtime.Numerics.dll",
            "System.Console.dll", "System.Runtime.InteropServices.dll", "System.Threading.Thread.dll",
            "System.Diagnostics.Process.dll", "System.Net.Sockets.dll", "System.Net.Primitives.dll",
            "System.ComponentModel.Primitives.dll"];
        var references = tpa.Split(Path.PathSeparator)
            .Where(path => allowed.Contains(Path.GetFileName(path), StringComparer.Ordinal))
            .Select(path => MetadataReference.CreateFromFile(path));
        var compilation = CSharpCompilation.Create("isolated_scriptlet",
            [CSharpSyntaxTree.ParseText(source)], references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary,
                optimizationLevel: OptimizationLevel.Release, allowUnsafe: false,
                concurrentBuild: false));
        using var assembly = new MemoryStream();
        if (!compilation.Emit(assembly).Success) return 125;
        if (request.CompileOnly)
        {
            output.Write("COMPILED\n"u8); output.Flush(); return 0;
        }
        assembly.Position = 0;
        Assembly loaded = AssemblyLoadContext.Default.LoadFromStream(assembly);
        var function = loaded.GetType("Scriptlet")?.GetMethod("Transform",
            BindingFlags.Static | BindingFlags.Public)?.CreateDelegate<Func<string, string>>();
        if (function is null) return 125;
        output.Write("READY\n"u8); output.Flush();
        string result = function(request.Input);
        if (result is null || result.Length > 8192) return 126;
        byte[] encoded = Encoding.UTF8.GetBytes(result);
        BinaryPrimitives.WriteInt32LittleEndian(header, encoded.Length);
        output.Write(header); output.Write(encoded); output.Flush();
        return 0;
    }
}
