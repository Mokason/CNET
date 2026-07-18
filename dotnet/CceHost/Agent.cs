using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using CNET.Cce;

namespace CNET.CceHost;

/// <summary>
/// LLM controller + certified JSON tool-call spine.
/// Loop: task → LLM JSON → <see cref="JsonToolCall"/> certified classify →
/// execute MCP tools / soul skills → observation until final.
/// Unknown tools note a gap (jtc_feat→json_tool) for the personal-AI lane.
/// </summary>
public sealed class Agent
{
    private readonly IChatClient _llm;
    private readonly SoulHost _soul;
    private readonly string? _gapInbox;
    private readonly bool _jtcAvailable;

    private static readonly int[] WindowIds =
        { 176938, 216001, 117514, 60730, 159876, 99544, 53877, 52141,
          226926, 64506, 6164, 221965, 178945, 135086, 224777, 238110 };
    private static readonly Dictionary<int, string> Text = new() {
        [176938]="正如", [216001]="もう少し", [117514]="であれば", [60730]="və",
        [159876]="様に", [99544]="puisque", [53877]="者的", [52141]="တွေ",
        [226926]="も含", [64506]="akin", [6164]="いる", [221965]="그걸",
        [178945]="heps", [135086]="дода", [224777]="günst", [238110]="စ" };

    private const string SystemPrompt = """
You are an autonomous agent. Each turn you MUST reply with EXACTLY ONE JSON object and nothing else:
  {"thought":"<brief reasoning>","tool":"<name>","args":{...}}   to call a tool, or
  {"thought":"<brief reasoning>","final":"<answer>"}             when the task is done.
Tools (closed set — certified CNET classifier may override/confirm):
  calculator     args {"expr":"<e.g. 23 * 19 or sqrt(3*3+4*4)>"}  -> real math_eval (+ - * / ^ sqrt parentheses).
  memory_store   args {"key":"<k>","value":"<v>"} -> persist a fact.
  memory_recall  args {"query":"<k>"}             -> retrieve a stored fact.
  file_read      args {"path":"<relative path>"}  -> read a sandboxed file.
  web_search     args {"query":"<search terms>"}  -> look up live facts (web + wiki fallback). Use when you do not already know.
  wiki_lookup    args {"query":"<person/topic>"}  -> Wikipedia summary for a named entity or topic.
  cnet_recall    args {"cond":<0-15>,"current":<0-15>} -> consult the CERTIFIED gemma4 skill:
                 its top-3 next tokens given window token[cond] then window[current]. Auditable, with reliability.
When the task needs external knowledge you do not have, call web_search or wiki_lookup before final.
Think step by step, use tools, then give a final answer. Only ONE JSON object per turn.
""";

    public Agent(IChatClient llm, SoulHost soul, string? gapInbox = null)
    {
        _llm = llm;
        _soul = soul;
        _gapInbox = gapInbox
            ?? Environment.GetEnvironmentVariable("CNET_GAP_INBOX")
            ?? InferInboxBesideBase();
        _jtcAvailable = DetectJtc(soul);
    }

    private static string? InferInboxBesideBase()
    {
        // Best-effort: CNET_BASE_PATH.inbox
        string? basePath = Environment.GetEnvironmentVariable("CNET_BASE_PATH")
            ?? Environment.GetEnvironmentVariable("CNET_MODEL_PATH");
        return string.IsNullOrEmpty(basePath) ? null : basePath + ".inbox";
    }

    private static bool DetectJtc(SoulHost soul)
    {
        try
        {
            foreach (var u in soul.Units())
                if (u == JsonToolCall.UnitName) return true;
            return false;
        }
        catch
        {
            return false;
        }
    }

    public async Task RunAsync(string task, int maxSteps = 8)
    {
        using var conversation = new CnetHarnessConversation(
            _llm, SystemPrompt,
            maxRetainedTurns: Math.Clamp(maxSteps, 1, 8),
            maxRetainedCharacters: 32768,
            ownsClient: false);
        string nextUser = "TASK: " + task;
        Console.WriteLine($"TASK: {task}");
        Console.WriteLine($"jtc_certified={( _jtcAvailable ? "yes" : "no")} gap_inbox={_gapInbox ?? "(unset)"}\n");

        for (int step = 0; step < maxSteps; step++)
        {
            string reply = await conversation.SendAsync(nextUser);
            if (!TryExtractJson(reply, out var json))
            {
                Console.WriteLine($"[step {step}] LLM (unparseable): {reply.Trim()}");
                nextUser = "Your reply was not a single JSON object. Reply with one JSON object only.";
                continue;
            }

            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            string thought = root.TryGetProperty("thought", out var t) ? t.GetString() ?? "" : "";

            string llmTool = root.TryGetProperty("tool", out var tl) ? tl.GetString() ?? "" : "";
            var args = root.TryGetProperty("args", out var a) ? a : default;

            // --- A: certified classify before execute ---
            string tool = llmTool;
            string jtcNote = "";
            if (_jtcAvailable)
            {
                var (certTool, source, gapNoted) = JsonToolCall.ClassifyOrGap(_soul, json, _gapInbox);
                if (certTool != null)
                {
                    string normLlm = JsonToolCall.NormalizeTool(llmTool);
                    if (!string.IsNullOrEmpty(llmTool) &&
                        !string.Equals(normLlm, certTool, StringComparison.OrdinalIgnoreCase) &&
                        root.TryGetProperty("final", out _) == false)
                    {
                        jtcNote = $" [jtc:{source} preferred '{certTool}' over LLM '{llmTool}']";
                        tool = certTool;
                    }
                    else if (string.IsNullOrEmpty(tool))
                    {
                        tool = certTool;
                        jtcNote = $" [jtc:{source}]";
                    }
                    else
                    {
                        tool = certTool;
                        jtcNote = $" [jtc:{source} ok]";
                    }
                }
                else if (gapNoted)
                {
                    jtcNote = " [jtc:miss gap_noted]";
                }
            }

            // final answer
            if (root.TryGetProperty("final", out var fin) || tool is "final" or "finish" or "answer")
            {
                string ans = fin.ValueKind == JsonValueKind.String ? fin.GetString() ?? ""
                    : (args.ValueKind == JsonValueKind.Object && args.TryGetProperty("answer", out var an) ? an.GetString() : null)
                      ?? (args.ValueKind == JsonValueKind.Object && args.TryGetProperty("value", out var av) ? av.ToString() : null)
                      ?? thought;
                Console.WriteLine($"[step {step}] LLM thought: {thought}{jtcNote}");
                Console.WriteLine($"\n=== FINAL ANSWER (from the LLM): {ans} ===");
                return;
            }

            // E: unknown tool → gap + refuse execute
            if (!JsonToolCall.IsKnownTool(tool))
            {
                bool noted = JsonToolCall.NoteGap(_gapInbox);
                Console.WriteLine($"[step {step}] unknown tool '{tool}'{jtcNote} gap_noted={noted}");
                nextUser = $"OBSERVATION: unknown tool '{tool}' (not in closed set). gap_noted={noted}. Use only: calculator, memory_store, memory_recall, file_read, web_search, wiki_lookup, cnet_recall, or final.";
                continue;
            }

            tool = JsonToolCall.NormalizeTool(tool);
            Console.WriteLine($"[step {step}] tool={tool}{jtcNote}  (thought: {thought})");

            string obs = Execute(tool, args);
            Console.WriteLine($"          observation: {obs}\n");
            nextUser = "OBSERVATION: " + obs;
        }
        Console.WriteLine("\n=== stopped: max steps reached ===");
    }

    private string Execute(string tool, JsonElement args)
    {
        try
        {
            switch (tool)
            {
                case "calculator":
                    return McpTools.Calculator(args.ValueKind == JsonValueKind.Object && args.TryGetProperty("expr", out var ex)
                        ? ex.GetString() ?? "" : "");
                case "memory_store":
                    McpTools.Memorize(AsString(args, "key", "k"), AsString(args, "value", ""));
                    return "stored.";
                case "memory_recall":
                    return McpTools.Recall(args.ValueKind == JsonValueKind.Object && args.TryGetProperty("query", out var q)
                        ? q.GetString() ?? "" : "");
                case "file_read":
                    return McpTools.FileRead(args.ValueKind == JsonValueKind.Object && args.TryGetProperty("path", out var p)
                        ? p.GetString() ?? "" : "");
                case "web_search":
                    return McpTools.WebSearch(args.ValueKind == JsonValueKind.Object && args.TryGetProperty("query", out var wq)
                        ? wq.GetString() ?? "" : AsString(args, "q", ""));
                case "wiki_lookup":
                    return McpTools.WikiLookup(args.ValueKind == JsonValueKind.Object && args.TryGetProperty("query", out var wiq)
                        ? wiq.GetString() ?? "" : AsString(args, "q", ""));
                case "cnet_recall":
                {
                    int cond = args.ValueKind == JsonValueKind.Object && args.TryGetProperty("cond", out var c) && c.ValueKind == JsonValueKind.Number
                        ? c.GetInt32() : 0;
                    int cur = args.ValueKind == JsonValueKind.Object && args.TryGetProperty("current", out var cu) && cu.ValueKind == JsonValueKind.Number
                        ? cu.GetInt32() : 0;
                    if (cond < 0 || cond > 15 || cur < 0 || cur > 15) return "cnet_recall: indices must be 0-15";
                    string unit = $"acq_tk{WindowIds[cond]}q{WindowIds[cond]}";
                    int[] top3 = _soul.TopK(unit, cur);
                    double rel = _soul.Reliability(unit);
                    string ans = string.Join(" / ", Array.ConvertAll(top3, Label));
                    return $"certified top-3 after [{Label(cond)}][{Label(cur)}] = {ans}  (reliability {rel:F3}, from unit {unit})";
                }
                default:
                    return $"unknown tool '{tool}'";
            }
        }
        catch (Exception ex) { return $"tool error: {ex.Message}"; }
    }

    private static string AsString(JsonElement args, string key, string fallback)
    {
        if (args.ValueKind != JsonValueKind.Object || !args.TryGetProperty(key, out var v)) return fallback;
        return v.ValueKind == JsonValueKind.String ? v.GetString() ?? fallback : v.ToString();
    }

    private static string Label(int idx) =>
        idx >= 0 && idx < WindowIds.Length ? Text.GetValueOrDefault(WindowIds[idx], "?") : $"[win#{idx}]";

    private static bool TryExtractJson(string s, out string json)
    {
        json = "";
        int start = s.IndexOf('{');
        if (start < 0) return false;
        int depth = 0; bool inStr = false, esc = false;
        for (int i = start; i < s.Length; i++)
        {
            char c = s[i];
            if (inStr) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') inStr = false; }
            else if (c == '"') inStr = true;
            else if (c == '{') depth++;
            else if (c == '}') { if (--depth == 0) { json = s.Substring(start, i - start + 1); return true; } }
        }
        return false;
    }
}
