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
using CNET.Cce.Llm.Verify;

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

if (a.AutoContinue > 16)
{
    Console.Error.WriteLine($"--auto-continue must be in [0, 16], got {a.AutoContinue}");
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
var ghost = new MemorySession(session, memory, window, countTokens,
                              maxAutoContinues: (int)a.AutoContinue)
{
    ExactLane = !a.NoExact,
};

// Rung 4: certified records answer before the model when the gap inbox (and
// therefore the records dir + ledger) is known.
string? routeInbox = a.GapInbox ?? Environment.GetEnvironmentVariable("CNET_GAP_INBOX");
if (routeInbox is not null && routeInbox.EndsWith(".inbox", StringComparison.Ordinal))
{
    string baseNoExt = routeInbox[..^".inbox".Length];
    if (File.Exists(baseNoExt + ".gaps.txt"))
        ghost.Router = new CNET.Cce.Llm.Routing.RecordRouter(
            baseNoExt + ".records", baseNoExt + ".gaps.txt");
}

// Narrate model-directed lookups: when the model decides its context is
// missing a memory, it emits "RECALL: <keywords>" instead of an answer; the
// layer searches the store and asks again. Streaming backends already showed
// the RECALL line — this adds what the store served before the retry streams.
ghost.OnLookup = round =>
{
    string ids = round.BlobIds.Count > 0
        ? string.Join(" ", round.BlobIds.Select(i => $"#{i}"))
        : "nothing";
    Console.WriteLine($"\n  🔍 model searched \"{round.Query}\" → {ids}");
};

Console.WriteLine($"ghost-chat | backend={a.Backend} model={a.Model} window={window}");
Console.WriteLine($"store={storePath} ({store.Count} memories" +
    (store.CorruptLinesSkipped > 0 ? $", {store.CorruptLinesSkipped} corrupt lines skipped" : "") + ")");
Console.WriteLine("/exit /stats /show <id> /recall <query> /forget <id> /consolidate [commit] /distill <prompt>");
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

    if (input.StartsWith("/distill ", StringComparison.Ordinal))
    {
        // Generate-and-verify: sample the model, keep only what the JSON
        // verifier accepts, distill the survivor into a teaching record.
        // The model proposes; the parser disposes; the lane certifies.
        string inbox0 = a.GapInbox ?? Environment.GetEnvironmentVariable("CNET_GAP_INBOX") ?? "";
        if (inbox0.Length == 0)
        {
            Console.WriteLine("  no gap inbox: pass --gap-inbox <path> or set CNET_GAP_INBOX");
            continue;
        }
        string recordsDir0 = inbox0.EndsWith(".inbox", StringComparison.Ordinal)
            ? inbox0[..^".inbox".Length] + ".records" : inbox0 + ".records";
        var distiller = new VerifiedRecords(session, new JsonVerifier());
        Console.WriteLine("  sampling (verifier: json)...");
        // Candidates are raw material, not conversation — do not stream them.
        if (ollama is not null) ollama.OnToken = null;
        try
        {
            var receipt = distiller.Distill(input["/distill ".Length..].Trim(),
                inbox0, recordsDir0, samples: 4, maxTokens: a.MaxTokens);
            Console.WriteLine(receipt.SkillName is null
                ? $"  nothing verified in {receipt.Candidates} samples — no record, nothing taught"
                : $"  verified after {receipt.Candidates} sample(s) -> {receipt.SkillName} " +
                  $"({receipt.RecordPath}) — the lane certifies it from the record on its next tick");
        }
        catch (CnetHarnessException ex)
        {
            Console.WriteLine($"  distill error [{ex.Status}]: {ex.Message}");
        }
        finally
        {
            if (ollama is not null) ollama.OnToken = chunk => Console.Write(chunk);
        }
        continue;
    }

    if (input == "/consolidate" || input == "/consolidate commit")
    {
        // The hippocampus->cortex seam: extract memories worth teaching and
        // (on commit) note them into the gap-lane inbox, where the native
        // teaching cycle trains certified specialists from them.
        var consolidator = new GhostConsolidator(store);
        var items = consolidator.Extract();
        if (items.Count == 0)
        {
            Console.WriteLine("  (nothing to consolidate — no memory has earned teaching yet)");
            continue;
        }
        foreach (var item in items)
            Console.WriteLine($"  [#{item.BlobId} -> {item.SkillName}] {item.Reason}: " +
                $"{(item.Text.Length > 70 ? item.Text[..70] + "\u2026" : item.Text)}");

        if (input == "/consolidate")
        {
            Console.WriteLine($"  ({items.Count} teachable — dry run; '/consolidate commit' notes them into the gap lane)");
            continue;
        }

        string inbox = a.GapInbox
            ?? Environment.GetEnvironmentVariable("CNET_GAP_INBOX")
            ?? "";
        if (inbox.Length == 0)
        {
            Console.WriteLine("  no gap inbox: pass --gap-inbox <path> or set CNET_GAP_INBOX");
            continue;
        }
        var receipts = consolidator.Emit(items, inbox);
        // Corrections go through the record-as-oracle path: the user's own
        // words become the certifying truth, no LM teacher in the loop.
        string recordsDir = inbox.EndsWith(".inbox", StringComparison.Ordinal)
            ? inbox[..^".inbox".Length] + ".records"
            : inbox + ".records";
        receipts.AddRange(consolidator.EmitCorrections(
            consolidator.ExtractCorrections(), inbox, recordsDir));
        foreach (var r in receipts)
            Console.WriteLine(r.Emitted
                ? $"  noted {r.Item.SkillName} -> {inbox}"
                : $"  FAILED {r.Item.SkillName}: {r.Error}");
        int ok = receipts.Count(r => r.Emitted);
        Console.WriteLine($"  ({ok}/{receipts.Count} noted — the gap-lane daemon trains specialists from these on its next tick)");
        continue;
    }

    if (input.StartsWith('/'))
    {
        // Unknown or malformed command: never generate from it, never store it.
        Console.WriteLine("  commands: /exit /stats /show <id> /recall <query> /forget <id> /consolidate [commit]");
        continue;
    }

    try
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        MemoryGenerationResult r = ghost.Generate(
            a.System, input, a.MaxTokens, a.Sampling);
        sw.Stop();

        // Local backends did not stream; print now. Ollama already streamed.
        // Exact and certified answers never touched a backend — print always.
        if (ollama is null || r.Exact || r.CertifiedUnit is not null)
            Console.Write(r.Result.Text.Trim());
        Console.WriteLine();

        if (string.IsNullOrWhiteSpace(r.Result.Text) && r.Result.GeneratedTokens > 0)
            Console.WriteLine(
                $"  (no visible answer — the model spent all {r.Result.GeneratedTokens} tokens on " +
                "hidden reasoning; raise --max-tokens or use a non-thinking model)");

        if (r.Truncated)
            Console.WriteLine(a.AutoContinue > 0
                ? $"  (still unfinished after {r.AutoContinues} auto-continues — type \"continue\" for more, or raise --auto-continue)"
                : "  (cut off by --max-tokens — type \"continue\" to resume exactly where it stopped)");

        string receipts = r.UsedBlobIds.Count > 0
            ? string.Join(" ", r.UsedBlobIds.Select(i => $"#{i}"))
            : "none";
        string autoNote = r.AutoContinues > 0 ? $" | auto-continued ×{r.AutoContinues}" : "";
        Console.WriteLine(r.Exact
            ? $"  ── exact: computed in {sw.Elapsed.TotalMilliseconds:F1}ms — no model, cannot be wrong ──"
            : r.CertifiedUnit is not null
            ? $"  ── certified: {r.CertifiedUnit} — served from sealed knowledge, model not consulted ──"
            : $"  ── memory: {receipts} | prompt {r.Result.PromptTokens} tok | " +
              $"{r.Result.GeneratedTokens} tok in {sw.Elapsed.TotalSeconds:F1}s{autoNote} ──");
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
            case "--gap-inbox": a.GapInbox = Next(); break;
            case "--no-exact": a.NoExact = true; break;
            case "--auto-continue":
                if (!ParseU("--auto-continue", out uint ac)) return null;
                a.AutoContinue = ac; break;
            case "--help":
                helpRequested = true;
                goto default;
            default:
                Console.Error.WriteLine(
                    "usage: ghost-chat --backend managed|native|ollama --model <gguf-path|ollama-name>\n" +
                    "       [--store <jsonl>] [--system <text>] [--window N] [--max-tokens N]\n" +
                    "       [--sampling auto|deterministic|focused|balanced|exploratory]\n" +
                    "       [--auto-continue N   resume truncated answers automatically, default 8]\n" +
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
    public uint AutoContinue = 8;
    public string? GapInbox = null;
    public bool NoExact = false;
    public CnetHarnessSamplingMode Sampling = CnetHarnessSamplingMode.Balanced;
    public string OllamaUrl = "http://localhost:11434";
    public Args() { }
}
