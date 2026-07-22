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

var args_ = ParseArgs(args);
if (args_ is null) return 2;
var a = args_.Value;

string storePath = a.Store ?? Path.Combine(
    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
    ".cnet-llm", "ghost", "memory.jsonl");

// ── open the chosen backend behind the one interface ──
ICnetInferenceSession session;
Func<string, int> countTokens;
int window;
OllamaSession? ollama = null;

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
        session = ollama;
        countTokens = ollama.CountTokens;
        window = (int)a.Window;
        break;
    }
    default:
        Console.Error.WriteLine($"unknown backend '{a.Backend}' (managed|native|ollama)");
        return 2;
}

using var _ = session;
using var store = BlobStore.Open(storePath);
var memory = new ConversationMemory(store, countTokens);
var ghost = new MemorySession(session, memory, window);

Console.WriteLine($"ghost-chat | backend={a.Backend} model={a.Model} window={window}");
Console.WriteLine($"store={storePath} ({store.Count} memories" +
    (store.CorruptLinesSkipped > 0 ? $", {store.CorruptLinesSkipped} corrupt lines skipped" : "") + ")");
Console.WriteLine("/exit /stats /show <id> /recall <query>");
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
        foreach (MemoryBlob b in store.Recall(input[8..], 5))
            Console.WriteLine($"  [#{b.Id} | {b.Role}] {(b.Text.Length > 100 ? b.Text[..100] + "…" : b.Text)}");
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

static Args? ParseArgs(string[] argv)
{
    var a = new Args();
    for (int i = 0; i < argv.Length; i++)
    {
        string? Next() => i + 1 < argv.Length ? argv[++i] : null;
        switch (argv[i])
        {
            case "--backend": a.Backend = Next() ?? a.Backend; break;
            case "--model": a.Model = Next() ?? a.Model; break;
            case "--store": a.Store = Next(); break;
            case "--system": a.System = Next(); break;
            case "--window": a.Window = uint.TryParse(Next(), out uint w) ? w : a.Window; break;
            case "--max-tokens": a.MaxTokens = uint.TryParse(Next(), out uint m) ? m : a.MaxTokens; break;
            case "--sampling":
                a.Sampling = Enum.TryParse(Next(), ignoreCase: true,
                    out CnetHarnessSamplingMode s) ? s : a.Sampling;
                break;
            case "--ollama-url": a.OllamaUrl = Next() ?? a.OllamaUrl; break;
            case "--help":
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
