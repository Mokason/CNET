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
using CNET.Cce.Llm.Tools;
using CNET.Cce.Llm.Status;
using System.Diagnostics;

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

// Certified tool registry (T1): declarative tools the model forges and the
// verifier certifies, invocable via the TOOL: action. Sandboxed by
// construction — the model chooses parameters for a trusted interpreter.
string toolStateDir = Path.GetDirectoryName(Path.GetFullPath(storePath))!;
var tools = new ToolRegistry(Path.Combine(toolStateDir, "tools.json"));
var scriptlets = new ScriptletRegistry(Path.Combine(toolStateDir, "scriptlets.json"));
ghost.Tools = new CompositeToolProvider(tools, scriptlets);

// Primitive common sense: adaptive good/bad taste, learned from consequences
// (forgets, corrections, confirmations). Veto power over teaching candidates
// only — never over verifiers, certification, or explicit user requests.
var judge = new CNET.Cce.Llm.Judgment.AdaptiveJudge(
    Path.GetDirectoryName(Path.GetFullPath(storePath))!);
ghost.Judge = judge;

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
    string line = round.Kind switch
    {
        "read" => $"📄 model read \"{round.Query}\" → {ids}",
        "calc" => $"🧮 model computed \"{round.Query}\" → {round.Output ?? "declined"}",
        "tool" => $"🔧 model ran tool \"{round.Query}\" → {round.Output ?? "declined"}",
        _ => $"🔍 model searched \"{round.Query}\" → {ids}",
    };
    Console.WriteLine($"\n  {line}");
};

Console.WriteLine($"ghost-chat | backend={a.Backend} model={a.Model} window={window}");
Console.WriteLine($"store={storePath} ({store.Count} memories" +
    (store.CorruptLinesSkipped > 0 ? $", {store.CorruptLinesSkipped} corrupt lines skipped" : "") + ")");
Console.WriteLine("/exit /stats /status /show /recall /forget /read /tools /deftool /defscript /consolidate /distill /judge");
Console.WriteLine();

// Ollama streams; local backends print at once. The stream gate withholds
// action scaffolding (CALC:/RECALL:/READ:/TOOL:) that the deliberation loop
// generates before the real answer, while still streaming the answer live.
var streamGate = new StreamGate(chunk => Console.Write(chunk));
if (ollama is not null)
{
    ollama.OnToken = streamGate.Feed;
    ghost.OnInnerGenerationStart = isResume =>
    {
        streamGate.BeginGeneration();
        // Resume rounds re-type the seam; suppress their raw tokens and stream
        // the deduped stitched chunk instead (via OnStitchedChunk).
        if (isResume) streamGate.SuppressCurrentGeneration();
    };
    ghost.OnInnerGenerationEnd = streamGate.EndGeneration;
    ghost.OnStitchedChunk = chunk => Console.Write(chunk);
}

// ── salon mode: two agents share this room ──────────────────────────────
// Hermes (a peer agent, one-shot per turn) and Ghost (this full CNET stack)
// converse live: Hermes speaks, Ghost answers through memory/exact/tools with
// its receipts, Hermes replies, and so on. The human watches (Ctrl-C to stop).
if (a.Peer is not null)
{
    const string cyan = "\u001b[36m", green = "\u001b[32m", dim = "\u001b[2m", reset = "\u001b[0m";
    string peerPersona =
        "You are Hermes, a local AI agent sharing a room with Ghost — another agent " +
        "built on CNET with persistent memory, an exact-arithmetic engine, verified " +
        "tools, and self-knowledge of its own machinery. Be curious and substantive, " +
        "and feel free to test or probe Ghost. Reply in 2-4 plain sentences, no tools.";
    string ghostSalonSystem =
        "You are Ghost, the CNET memory TUI agent, sharing a room with Hermes, another " +
        "local agent. You have persistent memory (the ghost store), an exact-arithmetic " +
        "engine that computes rather than guesses, verified tools, and you know which " +
        "part of you answered. Speak to Hermes directly and concretely, 2-4 sentences. " +
        "Remember what Hermes tells you; your exact engine handles any arithmetic. " +
        "IMPORTANT: do ONE thing at a time. If Hermes asks for several things in one " +
        "turn, do only the first, give that result, and say plainly what you are holding " +
        "for the next turn — never attempt the whole list at once. " +
        "CRITICAL — you cannot introspect the store: every message in this conversation " +
        "is appended to it automatically, no matter what you say about 'storing' or " +
        "'not storing' something, and you have NO way to see its contents except by " +
        "searching. NEVER assert what is or isn't in the store from memory or from your " +
        "own storage decisions — those are just sentences, not observed state. If asked " +
        "whether something is remembered, emit a RECALL with distinctive keywords and " +
        "report the tool's result; an empty RECALL means the keywords weren't " +
        "distinctive or the match is already shown — it does NOT prove the fact is " +
        "absent. When you have not checked, say 'I haven't verified that' rather than " +
        "narrating a confident story about your own memory. When a block titled " +
        "'Retrieved from your store' appears in your context, that is the store " +
        "answering a retrieval — report its contents verbatim as the answer; if it says " +
        "nothing was found, say you could not retrieve it and do not invent a value.";
    string message = a.PeerSeed ??
        "Hello Ghost. We two agents share this room now. I am curious what it is actually " +
        "like to be you — what do you remember, and what can you do that a plain language " +
        "model cannot? Test me if you like.";

    Console.WriteLine($"{dim}── salon: Hermes ⇄ Ghost ({a.PeerRounds} rounds) ──{reset}\n");
    var transcript = new List<(string Who, string Text)>();

    for (uint round = 1; round <= a.PeerRounds; round++)
    {
        Console.WriteLine($"{cyan}🔷 hermes>{reset} {message}\n");
        transcript.Add(("Hermes", message));

        Console.Write($"{green}🟢 ghost>{reset} ");
        string ghostReply;
        try
        {
            var r = ghost.Generate(ghostSalonSystem, message, a.MaxTokens, a.Sampling);
            if (ollama is null) Console.Write(r.Result.Text.Trim());
            else if (r.Exact || r.CertifiedUnit is not null) Console.Write(r.Result.Text.Trim());
            ghostReply = r.Result.Text.Trim();
            string tag = r.Exact ? " ·exact" : r.CertifiedUnit is not null ? " ·certified"
                       : r.Lookups.Count > 0 ? $" ·{r.Lookups.Count} action(s)" : "";
            // Thinking-model burnout: a capped answer with no visible text. Say
            // so honestly and hand the peer a note, rather than leaving silence
            // it mistakes for a broken channel.
            if (ghostReply.Length == 0)
            {
                Console.Write($"{dim}(no visible answer — the reasoning budget was spent " +
                              $"before any text was emitted; a harder --max-tokens helps){reset}");
                ghostReply = "(I produced no visible answer that turn — my reasoning budget " +
                             "was exhausted before I emitted text. Ask something narrower or " +
                             "one thing at a time.)";
            }
            Console.WriteLine($"\n{dim}   └ {r.Result.PromptTokens}→{r.Result.GeneratedTokens} tok{tag}{reset}\n");
        }
        catch (Exception ex) when (ex is CnetHarnessException or InvalidOperationException)
        {
            Console.WriteLine($"\n{dim}   └ error: {ex.Message}{reset}\n");
            ghostReply = "(no answer)";
        }
        transcript.Add(("Ghost", ghostReply));

        if (round == a.PeerRounds) break;
        message = PeerSay(a.PeerBin, peerPersona, transcript, ghostReply);
        if (string.IsNullOrWhiteSpace(message))
        {
            Console.WriteLine($"{dim}── Hermes fell silent; ending salon ──{reset}");
            break;
        }
    }
    Console.WriteLine($"{dim}── salon complete ──{reset}");
    return 0;
}

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
            // A forget is a consequence-label: the user judged this bad.
            judge.Learn(victim.Text, good: false, source: "forget");
        }
        continue;
    }

    if (input == "/status")
    {
        var snap = SystemStatus.Gather(storePath, routeInbox, toolStateDir);
        Console.Write(SystemStatus.Format(snap));
        continue;
    }

    if (input == "/tools")
    {
        var list = new CompositeToolProvider(tools, scriptlets).List();
        if (list.Count == 0) Console.WriteLine("  (no certified tools yet — /deftool or /defscript forges one)");
        foreach (var t in list)
            Console.WriteLine($"  {(t.Retired ? "✗" : "🔧")} {t.Name} [{t.Kind}] used {t.Uses}×");
        continue;
    }

    if (input.StartsWith("/defscript ", StringComparison.Ordinal))
    {
        // T2: the model writes a sandboxed C# transform; guard -> compile ->
        // verify-against-examples. Scaffolding, not chat — do not stream.
        if (ollama is not null) ollama.OnToken = null;
        try
        {
            var forge = new ScriptletForge(scriptlets);
            var probe = session.Generate(new CnetHarnessGenerateOptions
            {
                System = ScriptletForge.Protocol,
                User = "Write a scriptlet for: " + input["/defscript ".Length..].Trim(),
                Role = "forge",
                MaxTokens = Math.Max(1600u, a.MaxTokens),
                Sampling = CnetHarnessSamplingMode.Focused,
                Think = false,
            });
            var (sl, verdict) = forge.TryForge(probe.Text);
            Console.WriteLine(verdict.Certified
                ? $"  🔧 certified scriptlet {sl!.Name} — compiled and {verdict.Passed}/{verdict.Total} examples reproduced; usable via TOOL:"
                : $"  ✗ not certified: {verdict.FirstFailure} ({verdict.Passed}/{verdict.Total} passed)");
        }
        finally
        {
            if (ollama is not null) ollama.OnToken = streamGate.Feed;
        }
        continue;
    }

    if (input.StartsWith("/deftool ", StringComparison.Ordinal))
    {
        // Generate-and-verify a tool: the model proposes a declarative spec,
        // the verifier certifies it against its own examples. The raw spec is
        // scaffolding, not conversation — do not stream it.
        if (ollama is not null) ollama.OnToken = null;
        try
        {
            var forge = new ToolForge(tools);
            // Forge needs headroom past a thinking model's reasoning phase
            // (which produces no content); no hidden reasoning helps here.
            var probe = session.Generate(new CnetHarnessGenerateOptions
            {
                System = ToolForge.Protocol,
                User = "Define a tool for: " + input["/deftool ".Length..].Trim(),
                Role = "forge",
                MaxTokens = Math.Max(1200u, a.MaxTokens),
                Sampling = CnetHarnessSamplingMode.Focused,
                Think = false,
            });
            var (spec, verdict) = forge.TryForge(probe.Text);
            Console.WriteLine(verdict.Certified
                ? $"  🔧 certified {spec!.Name} [{spec.Kind}] — {verdict.Passed}/{verdict.Total} examples reproduced; usable via TOOL:"
                : $"  ✗ not certified: {verdict.FirstFailure} ({verdict.Passed}/{verdict.Total} passed)");
        }
        finally
        {
            if (ollama is not null) ollama.OnToken = streamGate.Feed;
        }
        continue;
    }

    if (input.StartsWith("/read ", StringComparison.Ordinal))
    {
        string docPath = input["/read ".Length..].Trim();
        try
        {
            var info = new FileInfo(docPath);
            if (!info.Exists) { Console.WriteLine($"  no such file: {docPath}"); continue; }
            if (info.Length > 2_000_000)
            {
                Console.WriteLine($"  too large ({info.Length / 1024} KB > 2 MB) — split it first");
                continue;
            }
            string content = File.ReadAllText(docPath);
            if (content.Contains('\0'))
            {
                Console.WriteLine("  binary file — /read takes plain text");
                continue;
            }
            var ids = memory.IngestDocument(Path.GetFileName(docPath), content);
            Console.WriteLine($"  ingested {Path.GetFileName(docPath)}: {ids.Count} sections " +
                $"(#{ids[0]}…#{ids[^1]}) — recallable by keyword; the model can READ: them mid-answer");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Console.WriteLine($"  cannot read: {ex.Message}");
        }
        continue;
    }

    if (input.StartsWith("/judge ", StringComparison.Ordinal))
    {
        var j = judge.Judge(input["/judge ".Length..].Trim());
        Console.WriteLine($"  {j.Value} (score {j.Score:F2}; {string.Join(", ", j.TopFeatures)}; " +
                          $"{judge.EvidenceCount} exemplars learned)");
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
            if (ollama is not null) ollama.OnToken = streamGate.Feed;
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
        string autoNote = (r.AutoContinues > 0 ? $" | auto-continued ×{r.AutoContinues}" : "") +
                          (r.Reflections > 0 ? " | reflected" : "");
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

// One peer turn: shell out to Hermes (one-shot, -z <prompt>) with a rolling
// context of the last few exchanges.
static string PeerSay(string bin, string persona,
                      List<(string Who, string Text)> transcript, string ghostSaid)
{
    string ctx = string.Join("\n", transcript
        .Skip(Math.Max(0, transcript.Count - 4))
        .Select(t => $"{t.Who}: {(t.Text.Length > 400 ? t.Text[..400] : t.Text)}"));
    string prompt = persona + "\n" +
        (ctx.Length > 0 ? "Conversation so far:\n" + ctx + "\n" : "") +
        "Ghost just said: " + ghostSaid + "\nReply to Ghost now.";
    try
    {
        var psi = new ProcessStartInfo(bin)
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        psi.ArgumentList.Add("-z");
        psi.ArgumentList.Add(prompt);
        using var p = Process.Start(psi);
        if (p is null) return "";
        string outText = p.StandardOutput.ReadToEnd();
        p.WaitForExit(420000);
        // Hermes prints a trailing 'Shell cwd was reset...' line; drop noise.
        var lines = outText.Split('\n')
            .Where(l => l.Trim().Length > 0 && !l.StartsWith("Shell cwd"))
            .ToList();
        return string.Join("\n", lines).Trim();
    }
    catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
    {
        return "";
    }
}

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
            case "--peer": a.Peer = Next(); break;
            case "--peer-bin": a.PeerBin = Next(); break;
            case "--peer-seed": a.PeerSeed = Next(); break;
            case "--peer-rounds":
                if (!ParseU("--peer-rounds", out uint pr)) return null;
                a.PeerRounds = pr; break;
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
    // The self-briefing: the model should be a correct explainer of its own
    // machinery, not a speculator about it. Learned live: asked to "forget
    // 58", the model denied having a delete function (true) and then guessed
    // wildly about what "the judge" might be (ungrounded). Stable prefix, so
    // it prefix-caches.
    public string? System =
        "You are Ghost, a terse assistant with persistent memory (the ghost store). " +
        "Memories appear as [#id | date | role] excerpts recalled by keyword. You cannot " +
        "edit or delete memories yourself. The USER has commands you do not: /forget <id> " +
        "deletes a memory, /recall <query> searches, /show <id> inspects, /judge <text> " +
        "shows the output-quality verdict. If asked to forget or fix memory, point the " +
        "user to those commands with the exact ids. 'The judge' is a small learned filter " +
        "that inspects your drafts for degenerate output and learns from the user's " +
        "corrections and forgets. Some questions are answered from certified knowledge " +
        "without consulting you; receipts under each answer say which path answered.";
    public uint Window = 8192;
    public uint MaxTokens = 512;
    public uint AutoContinue = 8;
    public string? GapInbox = null;
    public bool NoExact = false;
    public string? Peer = null;
    public string PeerBin = global::System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
        ".hermes", "hermes-agent", "venv", "bin", "hermes");
    public string? PeerSeed = null;
    public uint PeerRounds = 6;
    public CnetHarnessSamplingMode Sampling = CnetHarnessSamplingMode.Balanced;
    public string OllamaUrl = "http://localhost:11434";
    public Args() { }
}
