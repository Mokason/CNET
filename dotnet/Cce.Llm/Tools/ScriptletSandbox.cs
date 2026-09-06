using System.Runtime.CompilerServices;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace CNET.Cce.Llm.Tools;

/// <summary>
/// Static gate over scriptlet source, run BEFORE compilation. The model
/// supplies only a method body for <c>string Transform(string input)</c>; this
/// rejects unsupported syntax before sending source to the isolated worker.
/// </summary>
/// <remarks>
/// This guard is a policy filter, not a security boundary. Source compilation
/// and execution take place only in the Linux Landlock/seccomp worker.
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

    /// <summary>Returns null when source passes policy, else a refusal reason.</summary>
    public static string? Reject(string sourceRaw)
    {
        if (string.IsNullOrWhiteSpace(sourceRaw)) return "empty source";
        if (sourceRaw.Length > MaxSourceLength) return $"source over {MaxSourceLength} chars";
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

}

/// <summary>
/// Validates and compiles source inside the isolated worker. Returned delegates
/// retain source only and invoke a fresh isolated worker; they never load an
/// untrusted assembly into the host process.
/// </summary>
public static class ScriptletCompiler
{
    private sealed record BoundSource(string Body);
    private static readonly ConditionalWeakTable<Func<string, string>, BoundSource> Bound = new();
    internal static bool TryGetSource(Func<string, string> fn, out string source)
    {
        source = "";
        if (fn is null || !Bound.TryGetValue(fn, out BoundSource? bound)) return false;
        source = bound.Body; return true;
    }

    /// <summary>Compiles a scriptlet body. Returns the delegate, or the error.</summary>
    public static bool TryCompile(string bodyRaw, out Func<string, string>? fn, out string error)
    {
        fn = null;
        error = ScriptletGuard.Reject(bodyRaw) ?? "";
        if (error.Length != 0) { error = "guard: " + error; return false; }
        string body = ScriptletGuard.StripLeadingUsings(bodyRaw);
        if (!ScriptletProcess.Run(body, "", true, 200, 8192, out _, out error, out _)) return false;
        fn = input =>
        {
            if (!ScriptletSandbox.TryRunSource(body, input, out string output))
                throw new InvalidOperationException("scriptlet execution refused");
            return output;
        };
        Bound.Add(fn, new BoundSource(body));
        return true;
    }
}

/// <summary>Runs source-bound scriptlets in an OS sandbox with bounded lifetime.</summary>
public static class ScriptletSandbox
{
    /// <summary>
    /// Compatibility entry point for delegates returned by ScriptletCompiler.
    /// Arbitrary delegates are no longer supported and refuse without invocation.
    /// Use TryRunSource for new callers. Compilation/startup has a separate
    /// five-second deadline; timeoutMs bounds execution and worker teardown.
    /// </summary>
    public static bool TryRun(Func<string, string> fn, string input, out string output,
                              int timeoutMs = 200, int maxOutput = 8192)
    {
        output = "";
        return ScriptletCompiler.TryGetSource(fn, out string source) &&
            ScriptletProcess.Run(source, input, false, timeoutMs, maxOutput,
                out output, out _, out _);
    }

    /// <summary>Guards source, then compiles and executes only inside the worker.</summary>
    public static bool TryRunSource(string source, string input, out string output,
        int timeoutMs = 200, int maxOutput = 8192)
    {
        output = "";
        if (ScriptletGuard.Reject(source) is not null) return false;
        return ScriptletProcess.Run(ScriptletGuard.StripLeadingUsings(source), input,
            false, timeoutMs, maxOutput, out output, out _, out _);
    }
}
