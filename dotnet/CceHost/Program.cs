using System;
using System.Collections.Generic;
using System.Linq;
using CNET.Cce;
using CNET.CceHost;

// CNET .NET host.
//   default        -> cognitive recall test over the certified base
//   --agent [task] -> Milestone 2: a REAL LLM controller drives tools + the
//                     certified CNET skill (ollama; CNET_LLM_MODEL to pick model)

bool agentMode = args.Contains("--agent");
string basePath = args.FirstOrDefault(a => a.EndsWith(".cnb"))
    ?? "/home/marble/AI/CNET/soul_gemma4v2_final.cnb";

if (agentMode)
{
    string model = Environment.GetEnvironmentVariable("CNET_LLM_MODEL") ?? "qwen2.5:7b";
    int taskIdx = Array.IndexOf(args, "--agent") + 1;
    string task = (taskIdx > 0 && taskIdx < args.Length && !args[taskIdx].EndsWith(".cnb"))
        ? args[taskIdx]
        : "Compute 47 times 13 with the calculator. Store the result in memory under key 'product'. "
          + "Then recall it to double-check. Also consult the certified gemma4 skill (cnet_recall) for "
          + "its top-3 next tokens given window token 0 then token 1, and note its reliability. "
          + "Finish with a one-line summary of everything you found.";

    Console.WriteLine("=== CNET .NET host — Milestone 2: real LLM controller ===");
    Console.WriteLine($"LLM: {model} (ollama)   base: {basePath}\n");
    McpTools.MemoryInit();
    using var soulA = new SoulHost(basePath);
    var agent = new Agent(new OllamaClient(model), soulA);
    await agent.RunAsync(task);
    return;
}

// ---- default: cognitive recall test ----

// Window token ids (gemma4's top continuations of <bos>) -> text, decoded from
// the model's OWN embedded vocab. Index i in a unit's one-hot IS window[i].
int[] windowIds = { 176938, 216001, 117514, 60730, 159876, 99544, 53877, 52141,
                    226926, 64506, 6164, 221965, 178945, 135086, 224777, 238110 };
var text = new System.Collections.Generic.Dictionary<int, string> {
    [176938]="正如", [216001]="もう少し", [117514]="であれば", [60730]="və",
    [159876]="様に", [99544]="puisque", [53877]="者的", [52141]="တွေ",
    [226926]="も含", [64506]="akin", [6164]="いる", [221965]="그걸",
    [178945]="heps", [135086]="дода", [224777]="günst", [238110]="စ" };
string Label(int idx) => idx >= 0 && idx < windowIds.Length
    ? $"{text.GetValueOrDefault(windowIds[idx], "?")}" : $"[win#{idx}]";

Console.WriteLine("=== CNET .NET host — cognitive test ===");
Console.WriteLine($"base: {basePath}\n");

using var soul = new SoulHost(basePath);
Console.WriteLine("[host] certified base loaded via soul_host shim.\n");

// A conditioning token (the unit) + a current token (the one-hot input) form a
// context; the unit recalls the model's certified ordered top-3 continuation.
int[] condIdx = { 0, 2, 4 };      // 正如 / であれば / 様に
int[] curIdx  = { 1, 7, 11 };     // もう少し / တွေ / 그걸

Console.WriteLine("Q: after [conditioning][current], what are gemma4's top-3 next tokens?\n");
foreach (int ci in condIdx)
{
    string unit = $"acq_tk{windowIds[ci]}q{windowIds[ci]}";
    var (inT, outT) = soul.UnitDims(unit);
    double rel = soul.Reliability(unit);
    Console.WriteLine($"  unit {unit}  (in={inT}, out={outT}, reliability={rel:F3})");
    foreach (int wi in curIdx)
    {
        int[] top3 = soul.TopK(unit, wi);   // certified ordered top-3, as window indices
        string ans = string.Join(" / ", Array.ConvertAll(top3, Label));
        Console.WriteLine($"    after [{Label(ci)}][{Label(wi)}]  ->  {ans}");
    }
    Console.WriteLine();
}

// Prove real typed-port routing (this returned -2 before the fix).
double[] rin = new double[256]; rin[1] = 1.0;
double[] rout = soul.Route($"tk{windowIds[0]}q{windowIds[0]}", rin);
int r0 = 0; for (int j = 1; j < 256; j++) if (rout[j] > rout[r0]) r0 = j;
Console.WriteLine($"[route] soul_route(tk{windowIds[0]}...) executed a real typed-port plan; top field-0 = {Label(r0)}\n");

Console.WriteLine("=== honest verdict ===");
Console.WriteLine("REAL: the .NET host loads the certified base and RECALLS gemma4's ranked");
Console.WriteLine("      next-token preferences verbatim, through a size-safe, real-port ABI");
Console.WriteLine("      with genuine (Laplace) reliability. That IS a narrow cognitive act,");
Console.WriteLine("      faithfully reproduced and auditable.");
Console.WriteLine("NOT YET: this is recall over a bare-<bos> multilingual window, not reasoning.");
Console.WriteLine("      No planner is deciding sub-goals; there is no LLM in the loop. A real");
Console.WriteLine("      agent needs an LLM controller + real-context skills (Milestones 2/3).");
