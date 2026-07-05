using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using CNET.Cce;

namespace CNET.CceHost;

/// <summary>
/// Milestone 2 — a REAL LLM controller. The LLM (local, via ollama) decides
/// each step; tools are the genuine C MCP implementations + the certified CNET
/// skill (soul_host). Loop: task -> LLM picks a tool (JSON) -> C driver runs it
/// -> observation fed back -> repeat until the LLM gives a final answer.
/// </summary>
public sealed class Agent
{
    private readonly OllamaClient _llm;
    private readonly SoulHost _soul;

    // window token ids (gemma4's <bos> continuations) -> text, for cnet_recall.
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
Tools:
  calculator     args {"expr":"<e.g. 23 * 19>"}  -> real arithmetic (single binary op: a OP b).
  memory_store   args {"key":"<k>","value":"<v>"} -> persist a fact.
  memory_recall  args {"query":"<k>"}             -> retrieve a stored fact.
  cnet_recall    args {"cond":<0-15>,"current":<0-15>} -> consult the CERTIFIED gemma4 skill:
                 its top-3 next tokens given window token[cond] then window[current]. Auditable, with reliability.
  file_read      args {"path":"<relative path>"}  -> read a sandboxed file.
Think step by step, use tools, then give a final answer. Only ONE JSON object per turn.
""";

    public Agent(OllamaClient llm, SoulHost soul) { _llm = llm; _soul = soul; }

    public async Task RunAsync(string task, int maxSteps = 8)
    {
        var msgs = new List<(string, string)> { ("system", SystemPrompt), ("user", "TASK: " + task) };
        Console.WriteLine($"TASK: {task}\n");

        for (int step = 0; step < maxSteps; step++)
        {
            string reply = await _llm.ChatAsync(msgs.ToArray());
            if (!TryExtractJson(reply, out var json))
            {
                Console.WriteLine($"[step {step}] LLM (unparseable): {reply.Trim()}");
                msgs.Add(("user", "Your reply was not a single JSON object. Reply with one JSON object only."));
                continue;
            }
            msgs.Add(("assistant", json));

            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            string thought = root.TryGetProperty("thought", out var t) ? t.GetString() ?? "" : "";

            string tool = root.TryGetProperty("tool", out var tl) ? tl.GetString() ?? "" : "";
            var args = root.TryGetProperty("args", out var a) ? a : default;
            // final answer: either a "final" field, or tool=="final"/"finish"/"answer"
            if (root.TryGetProperty("final", out var fin) || tool is "final" or "finish" or "answer")
            {
                string ans = fin.ValueKind == JsonValueKind.String ? fin.GetString() ?? ""
                    : (args.ValueKind == JsonValueKind.Object && args.TryGetProperty("answer", out var an) ? an.GetString() : null)
                      ?? (args.ValueKind == JsonValueKind.Object && args.TryGetProperty("value", out var av) ? av.ToString() : null)
                      ?? thought;
                Console.WriteLine($"[step {step}] LLM thought: {thought}");
                Console.WriteLine($"\n=== FINAL ANSWER (from the LLM): {ans} ===");
                return;
            }

            Console.WriteLine($"[step {step}] LLM decided: {tool}  (thought: {thought})");

            string obs = Execute(tool, args);
            Console.WriteLine($"          observation: {obs}\n");
            msgs.Add(("user", "OBSERVATION: " + obs));
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
                    return McpTools.Calculator(args.GetProperty("expr").GetString() ?? "");
                case "memory_store":
                    McpTools.Memorize(AsString(args, "key", "k"), AsString(args, "value", ""));
                    return "stored.";
                case "memory_recall":
                    return McpTools.Recall(args.GetProperty("query").GetString() ?? "");
                case "file_read":
                    return McpTools.FileRead(args.GetProperty("path").GetString() ?? "");
                case "cnet_recall":
                {
                    int cond = args.GetProperty("cond").GetInt32();
                    int cur = args.GetProperty("current").GetInt32();
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

    /// <summary>Read an arg as string whether the LLM sent a string or a number.</summary>
    private static string AsString(JsonElement args, string key, string fallback)
    {
        if (args.ValueKind != JsonValueKind.Object || !args.TryGetProperty(key, out var v)) return fallback;
        return v.ValueKind == JsonValueKind.String ? v.GetString() ?? fallback : v.ToString();
    }

    private static string Label(int idx) =>
        idx >= 0 && idx < WindowIds.Length ? Text.GetValueOrDefault(WindowIds[idx], "?") : $"[win#{idx}]";

    /// <summary>Extract the first balanced {...} object from arbitrary LLM text.</summary>
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
