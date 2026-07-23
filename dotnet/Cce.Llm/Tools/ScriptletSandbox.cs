using System.Reflection;
using System.Runtime.Loader;
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Emit;

namespace CNET.Cce.Llm.Tools;

/// <summary>
/// Static gate over scriptlet source, run BEFORE compilation. The model
/// supplies only a method body for <c>string Transform(string input)</c>; this
/// rejects any construct that could reach outside pure computation — the first
/// and most important line of defense, because it needs no code to run.
/// </summary>
/// <remarks>
/// Honesty about the threat model: full in-process isolation of arbitrary .NET
/// is not achievable, and this does not claim it. The generator here is a
/// cooperative LLM on a single-user machine, not an adversary crafting
/// exploits. Defense is layered — this denylist, a restricted reference set so
/// dangerous APIs do not even resolve, an execution timeout, and behavioral
/// certification against contract examples — which is proportionate to that
/// threat model, not a substitute for OS-level sandboxing.
/// </remarks>
public static class ScriptletGuard
{
    // Identifiers that must not appear anywhere in the source. Case-sensitive
    // (C# is); covers IO, network, process, reflection, interop, threading,
    // environment, and unmanaged escape hatches.
    private static readonly HashSet<string> ForbiddenIdentifiers = new(StringComparer.Ordinal)
    {
        "System", "using",                       // no namespace qualification or usings at all
        "File", "Directory", "Stream", "Path", "FileStream",
        "Socket", "HttpClient", "WebClient", "Dns", "TcpClient",
        "Process", "ProcessStartInfo", "Environment", "AppContext", "AppDomain",
        "Reflection", "Assembly", "Activator", "GetType", "typeof",
        "Marshal", "DllImport", "GCHandle", "NativeMemory", "Unsafe",
        "Thread", "Task", "ThreadPool", "Timer", "Mutex", "Monitor",
        "GC", "RuntimeHelpers", "MethodInfo", "Delegate", "Expression",
        "Console", "Debug", "Trace", "Registry",
        "stackalloc", "fixed", "unsafe", "__makeref", "__arglist",
        "dynamic", "nint", "nuint",
    };

    // Syntax the parser must not contain, regardless of identifiers.
    private const int MaxSourceLength = 4000;

    /// <summary>
    /// Drops leading `using Namespace;` DIRECTIVES the model habitually writes
    /// — the scaffold already provides the safe set, and dropping a
    /// `using System.IO;` grants nothing because the File/Directory/etc.
    /// identifiers stay on the denylist. `using` STATEMENTS (resource blocks)
    /// are left in place and still rejected by the guard.
    /// </summary>
    internal static string StripLeadingUsings(string body)
    {
        var lines = body.Split('\n').ToList();
        int i = 0;
        while (i < lines.Count)
        {
            string t = lines[i].Trim();
            if (t.Length == 0) { i++; continue; }
            // Directive: `using X.Y;` or `using a = X;` — not `using (` / `using var`.
            if (System.Text.RegularExpressions.Regex.IsMatch(t,
                    @"^using\s+[A-Za-z_][\w.]*(\s*=\s*[A-Za-z_][\w.<>]*)?\s*;$"))
            {
                lines.RemoveAt(i);
                continue;
            }
            break;   // first non-blank, non-directive line: body starts here
        }
        return string.Join('\n', lines);
    }

    /// <summary>Validates source. Returns null when safe, else the reason.</summary>
    public static string? Reject(string sourceRaw)
    {
        string source = StripLeadingUsings(sourceRaw);
        if (string.IsNullOrWhiteSpace(source)) return "empty source";
        if (source.Length > MaxSourceLength) return $"source over {MaxSourceLength} chars";

        SyntaxTree tree = CSharpSyntaxTree.ParseText(GuardWrap(source));
        SyntaxNode root = tree.GetRoot();

        if (tree.GetDiagnostics().Any(d => d.Severity == DiagnosticSeverity.Error))
            return "does not parse as a method body";

        foreach (SyntaxNode node in root.DescendantNodes())
        {
            switch (node)
            {
                case UsingDirectiveSyntax:
                case UsingStatementSyntax:
                    return "using is not allowed";
                case UnsafeStatementSyntax:
                    return "unsafe is not allowed";
                case PointerTypeSyntax:
                case FunctionPointerTypeSyntax:
                    return "pointer types are not allowed";
                case StackAllocArrayCreationExpressionSyntax:
                    return "stackalloc is not allowed";
                case FixedStatementSyntax:
                    return "fixed is not allowed";
                case TypeOfExpressionSyntax:
                    return "typeof is not allowed";
                // No obvious unbounded loops — the timeout is the backstop, but
                // reject the clearest ones so a runaway never starts.
                case WhileStatementSyntax { Condition: LiteralExpressionSyntax lit }
                    when lit.Token.ValueText == "True" || lit.Token.Text == "true":
                    return "while(true) is not allowed";
            }
        }

        foreach (SyntaxToken token in root.DescendantTokens())
        {
            if (token.IsKind(SyntaxKind.IdentifierToken) &&
                ForbiddenIdentifiers.Contains(token.ValueText))
                return $"identifier '{token.ValueText}' is not allowed";
        }

        return null;
    }

    /// <summary>Guard-only wrap: no scaffold usings, so a user 'using' or a
    /// 'System.' qualifier is the user's and gets caught, not the scaffold's.</summary>
    private static string GuardWrap(string body) =>
        "public static class Scriptlet {\n" +
        "  public static string Transform(string input) {\n" +
        body + "\n  }\n}\n";

    /// <summary>Wraps a method body in the fixed, safe scaffold (for compilation).</summary>
    internal static string WrapSource(string body) =>
        "using System;\n" +
        "using System.Linq;\n" +
        "using System.Text;\n" +
        "using System.Collections.Generic;\n" +
        "using System.Text.RegularExpressions;\n" +
        "public static class Scriptlet {\n" +
        "  public static string Transform(string input) {\n" +
        body + "\n" +
        "  }\n" +
        "}\n";
}

/// <summary>
/// Compiles a guarded scriptlet against a RESTRICTED reference set — only the
/// handful of BCL assemblies pure computation needs — so anything the guard
/// missed (an IO or network type) fails to resolve at compile time. Returns a
/// delegate; the compiled assembly is loaded into a collectible context.
/// </summary>
public static class ScriptletCompiler
{
    private static readonly Lazy<MetadataReference[]> References = new(BuildReferences);

    private static MetadataReference[] BuildReferences()
    {
        // Curated whitelist by file name. Caveat, made explicit: many
        // dangerous types (File, Environment, GC) live in the mega-assembly
        // System.Private.CoreLib, which pure computation also needs — so the
        // reference set alone cannot exclude them. It DOES exclude
        // separate-assembly APIs (System.Net.Http, System.Diagnostics.Process,
        // …). The SYNTAX GUARD is the primary defense against CoreLib-resident
        // escape hatches; this reference set is the second layer for the rest.
        string[] allowed =
        [
            "System.Private.CoreLib.dll", "System.Runtime.dll",
            "System.Linq.dll", "System.Collections.dll",
            "System.Text.RegularExpressions.dll", "System.Text.Encoding.dll",
            "netstandard.dll", "System.Runtime.Numerics.dll",
        ];
        string tpa = (string)(AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") ?? "");
        var refs = new List<MetadataReference>();
        foreach (string path in tpa.Split(Path.PathSeparator))
        {
            string file = Path.GetFileName(path);
            if (allowed.Contains(file, StringComparer.OrdinalIgnoreCase) && File.Exists(path))
                refs.Add(MetadataReference.CreateFromFile(path));
        }
        return refs.ToArray();
    }

    /// <summary>Compiles a scriptlet body. Returns the delegate, or the error.</summary>
    public static bool TryCompile(string bodyRaw, out Func<string, string>? fn, out string error)
    {
        fn = null;
        error = "";
        string body = ScriptletGuard.StripLeadingUsings(bodyRaw);

        SyntaxTree tree = CSharpSyntaxTree.ParseText(ScriptletGuard.WrapSource(body),
            new CSharpParseOptions(LanguageVersion.Latest));
        var compilation = CSharpCompilation.Create(
            "scriptlet_" + Guid.NewGuid().ToString("N"),
            [tree], References.Value,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary,
                optimizationLevel: OptimizationLevel.Release,
                allowUnsafe: false));

        using var ms = new MemoryStream();
        EmitResult emit = compilation.Emit(ms);
        if (!emit.Success)
        {
            error = string.Join("; ", emit.Diagnostics
                .Where(d => d.Severity == DiagnosticSeverity.Error)
                .Take(2).Select(d => d.GetMessage()));
            return false;
        }

        ms.Position = 0;
        var alc = new AssemblyLoadContext("scriptlet", isCollectible: true);
        Assembly asm = alc.LoadFromStream(ms);
        MethodInfo? method = asm.GetType("Scriptlet")?.GetMethod("Transform",
            BindingFlags.Public | BindingFlags.Static);
        if (method is null) { error = "Transform method not found"; return false; }

        fn = (Func<string, string>)Delegate.CreateDelegate(typeof(Func<string, string>), method);
        return true;
    }
}

/// <summary>Runs a compiled scriptlet under a hard wall-clock budget.</summary>
public static class ScriptletSandbox
{
    /// <summary>
    /// Executes on a dedicated background thread with a timeout. On timeout
    /// the thread is abandoned (background, so it never blocks shutdown) —
    /// the honest cost of not having managed thread-abort; the guard rejects
    /// the obvious infinite loops that would trigger it.
    /// </summary>
    public static bool TryRun(Func<string, string> fn, string input, out string output,
                              int timeoutMs = 200, int maxOutput = 8192)
    {
        string? result = null;
        Exception? failure = null;
        var done = new ManualResetEventSlim(false);

        var thread = new Thread(() =>
        {
            try { result = fn(input); }
            catch (Exception ex) { failure = ex; }
            finally { done.Set(); }
        })
        { IsBackground = true, Name = "scriptlet-run" };
        thread.Start();

        if (!done.Wait(timeoutMs)) { output = ""; return false; }   // timed out
        if (failure is not null || result is null) { output = ""; return false; }
        if (result.Length > maxOutput) { output = ""; return false; }
        output = result;
        return true;
    }
}
