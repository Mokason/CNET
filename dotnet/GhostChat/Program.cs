// ghost-chat — terminal chat over ICnetInferenceSession + ghost memory.
//
// The conversation never dies at the context window: every turn is rebuilt
// from keyword recall over the persistent blob store plus the recent turns,
// and every exchange is stored back. The store outlives sessions, processes,
// and even engines — the same file serves the managed CPU backend, the native
// cnet.so harness, and Ollama-served models (including *:cloud models whose
// weights never touch this machine).
//
//   ghost-chat --backend managed --model ~/.cnet-llm/test-cache/.../SmolLM.gguf
//   ghost-chat --backend native  --model <gguf>      (needs libcnet_harness.so)
//   ghost-chat --backend ollama  --model minimax-m3:cloud
//
// Commands inside the REPL: /exit, /stats, /show <id>, /recall <query>.

using CNET.Cce.CnetHarness;
using CNET.Cce.Llm;
using CNET.Cce.Llm.Memory;
using CNET.Cce.Llm.Ollama;

var args_ = ParseArgs(args, out bool helpRequested);
if (args_ is null) return helpRequested ? 0 : 2;
var a = args_.Value;

if (a.Window < 256 || a.Window > 1u << 20)
{
    Console.Error.WriteLine($"--window must be in [256, {1u << 20}], got {a.Window}");
    return 2;
}
if (a.MaxTokens == 0 || a.MaxTokens > 65536)
{
    // 65536 is the harness ABI's shared generate-options ceiling; exceeding it
    // surfaces as a raw ArgumentException from the native backend mid-REPL.
    Console.Error.WriteLine($"--max-tokens must be in [1, 65536], got {a.MaxTokens}");
    return 2;
}

string storePath = a.Store ?? Path.Combine(
    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
    ".cnet-llm", "ghost", "memory.jsonl");

// ── open the chosen backend behind the one interface ──
ICnetInferenceSession session;
Func<string, int> countTokens;
int window;
OllamaSession? ollama = null;

try
{
switch (a.Backend)
{
    case "managed":
    {
        var s = CnetLlmInferenceSession.Open(HarnessConfig(a));
        session = s;
        countTokens = s.CountTokens;
        window = s.EffectiveContextTokens;
        break;
    }
    case "native":
    {
        var s = CnetHarnessSession.Open(HarnessConfig(a));
        session = s;
        countTokens = s.CountTokens;
        window = (int)a.Window;
        break;
    }
    case "ollama":
    {
        ollama = OllamaSession.Open(a.Model, a.OllamaUrl);
        // Keep the server's context equal to the budgeter's window, or the
        // server silently truncates memory-packed prompts to its default.
        ollama.ContextTokens = (int)a.Window;
        session = ollama;
        countTokens = ollama.CountTokens;
        window = (int)a.Window;
        break;
    }
    default:
        Console.Error.WriteLine($"unknown backend '{a.Backend}' (managed|native|ollama)");
        return 2;
}
}
catch (CnetHarnessException ex)
{
    Console.Error.WriteLine($"cannot open {a.Backend} session: [{ex.Status}] {ex.Message}");
    return 1;
}
catch (Exception ex) when (ex is IOException or ArgumentException)
{
    Console.Error.WriteLine($"cannot start: {ex.Message}");
    return 1;
}

using var _ = session;
using var store = BlobStore.Open(storePath);
var memory = new ConversationMemory(store, countTokens);
var ghost = new MemorySession(session, memory, window);

Console.WriteLine($"ghost-chat | backend={a.Backend} model={a.Model} window={window}");
Console.WriteLine($"store={storePath} ({store.Count} memories" +
    (store.CorruptLinesSkipped > 0 ? $", {store.CorruptLinesSkipped} corrupt lines skipped" : "") + ")");
Console.WriteLine("/exit /stats /show <id> /recall <query> /forget <id>");
Console.WriteLine();

// Ollama streams; local backends print at once.
if (ollama is not null)
    ollama.OnToken = chunk => Console.Write(chunk);

while (true)
{
    Console.Write("you> ");
    string? input = Console.ReadLine();
    if (input is null) break;
    input = input.Trim();
    if (input.Length == 0) continue;

    if (input == "/exit") break;

    if (input == "/stats")
    {
        Console.WriteLine($"  memories={store.Count} session={memory.SessionId} turn={memory.Turn} window={window}");
        continue;
    }

    if (input.StartsWith("/show ", StringComparison.Ordinal) &&
        long.TryParse(input[6..].Trim().TrimStart('#'), out long id))
    {
        MemoryBlob? blob = store.Get(id);
        Console.WriteLine(blob is null
            ? $"  no memory #{id}"
            : $"  [#{blob.Id} | {blob.TimestampUtc} | s:{blob.SessionId} t:{blob.Turn} | {blob.Role}]\n  {blob.Text}");
        continue;
    }

    if (input.StartsWith("/recall ", StringComparison.Ordinal))
    {
        string query = input[8..].Trim();
        if (long.TryParse(query.TrimStart('#'), out long asId))
        {
            Console.WriteLine($"  (that looks like an id — try /show {asId}; /recall takes keywords)");
            continue;
        }
        var found = store.Recall(query, 5);
        if (found.Count == 0)
            Console.WriteLine("  (nothing recalled — no stored memory shares a distinctive keyword with that)");
        foreach (MemoryBlob b in found)
            Console.WriteLine($"  [#{b.Id} | {b.Role}] {(b.Text.Length > 100 ? b.Text[..100] + "…" : b.Text)}");
        continue;
    }

    if (input.StartsWith("/forget ", StringComparison.Ordinal) &&
        long.TryParse(input[8..].Trim().TrimStart('#'), out long forgetId))
    {
        MemoryBlob? victim = store.Get(forgetId);
        if (victim is null)
        {
            Console.WriteLine($"  no memory #{forgetId}");
        }
        else if (store.Forget(forgetId))
        {
            string snippet = victim.Text.Length > 80 ? victim.Text[..80] + "…" : victim.Text;
            Console.WriteLine($"  forgot [#{forgetId} | {victim.Role}] {snippet}");
            Console.WriteLine("  (the line stays in the store file as history; recall will never serve it again)");
        }
        continue;
    }

    if (input.StartsWith('/'))
    {
        // Unknown or malformed command: never generate from it, never store it.
        Console.WriteLine("  commands: /exit /stats /show <id> /recall <query> /forget <id>");
        continue;
    }

    try
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        MemoryGenerationResult r = ghost.Generate(
            a.System, input, a.MaxTokens, a.Sampling);
        sw.Stop();

        // Local backends did not stream; print now. Ollama already streamed.
        if (ollama is null)
            Console.Write(r.Result.Text.Trim());
        Console.WriteLine();

        if (string.IsNullOrWhiteSpace(r.Result.Text) && r.Result.GeneratedTokens > 0)
            Console.WriteLine(
                $"  (no visible answer — the model spent all {r.Result.GeneratedTokens} tokens on " +
                "hidden reasoning; raise --max-tokens or use a non-thinking model)");

        string receipts = r.UsedBlobIds.Count > 0
            ? string.Join(" ", r.UsedBlobIds.Select(i => $"#{i}"))
            : "none";
        Console.WriteLine(
            $"  ── memory: {receipts} | prompt {r.Result.PromptTokens} tok | " +
            $"{r.Result.GeneratedTokens} tok in {sw.Elapsed.TotalSeconds:F1}s ──");
        Console.WriteLine();
    }
    catch (CnetHarnessException ex)
    {
        Console.WriteLine($"  error [{ex.Status}]: {ex.Message}");
    }
    catch (InvalidOperationException ex)
    {
        Console.WriteLine($"  budget error: {ex.Message}");
    }
    catch (ArgumentException ex)
    {
        // The native backend validates generate options with raw
        // ArgumentExceptions rather than CnetHarnessException.
        Console.WriteLine($"  invalid request: {ex.Message}");
    }
}

return 0;

static CnetHarnessConfig HarnessConfig(in Args a) => new()
{
    ModelId = Path.GetFileNameWithoutExtension(a.Model),
    ModelPath = a.Model,
    Resource = CnetHarnessResource.Cpu,
    BudgetBytes = 8UL << 30,
    ContextTokens = a.Window,
    BatchTokens = Math.Min(512u, a.Window),
    Threads = (uint)Math.Max(1, Environment.ProcessorCount / 2),
};

static Args? ParseArgs(string[] argv, out bool helpRequested)
{
    helpRequested = false;
    var a = new Args();
    for (int i = 0; i < argv.Length; i++)
    {
        string? Next() => i + 1 < argv.Length ? argv[++i] : null;
        bool ParseU(string flag, out uint value)
        {
            string? raw = Next();
            if (uint.TryParse(raw, out value)) return true;
            Console.Error.WriteLine($"{flag} needs a positive integer, got '{raw}'");
            return false;
        }
        switch (argv[i])
        {
            case "--backend": a.Backend = Next() ?? a.Backend; break;
            case "--model": a.Model = Next() ?? a.Model; break;
            case "--store": a.Store = Next(); break;
            case "--system": a.System = Next(); break;
            case "--window":
                if (!ParseU("--window", out uint w)) return null;
                a.Window = w; break;
            case "--max-tokens":
                if (!ParseU("--max-tokens", out uint m)) return null;
                a.MaxTokens = m; break;
            case "--sampling":
            {
                string? raw = Next();
                if (!Enum.TryParse(raw, ignoreCase: true, out CnetHarnessSamplingMode s))
                {
                    Console.Error.WriteLine($"--sampling: unknown mode '{raw}'");
                    return null;
                }
                a.Sampling = s; break;
            }
            case "--ollama-url": a.OllamaUrl = Next() ?? a.OllamaUrl; break;
            case "--help":
                helpRequested = true;
                goto default;
            default:
                Console.Error.WriteLine(
                    "usage: ghost-chat --backend managed|native|ollama --model <gguf-path|ollama-name>\n" +
                    "       [--store <jsonl>] [--system <text>] [--window N] [--max-tokens N]\n" +
                    "       [--sampling auto|deterministic|focused|balanced|exploratory]\n" +
                    "       [--ollama-url http://localhost:11434]");
                return null;
        }
    }
    if (string.IsNullOrEmpty(a.Model))
    {
        Console.Error.WriteLine("--model is required");
        return null;
    }
    return a;
}

struct Args
{
    public string Backend = "managed";
    public string Model = "";
    public string? Store = null;
    public string? System = "You are a helpful, terse assistant with persistent memory.";
    public uint Window = 8192;
    public uint MaxTokens = 512;
    public CnetHarnessSamplingMode Sampling = CnetHarnessSamplingMode.Deterministic;
    public string OllamaUrl = "http://localhost:11434";
    public Args() { }
}
