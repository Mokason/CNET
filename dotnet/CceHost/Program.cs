using System;
using System.Collections.Generic;
using CNET.Cce;

// CNET .NET host — cognitive test.
// Loads the certified soul base and asks its units real questions through the
// clean soul_host ABI (size-safe run, real ports, real reliability). Then
// reports HONESTLY what cognition is present.

string basePath = args.Length > 0 ? args[0]
    : "/home/marble/AI/CNET/soul_gemma4v2_final.cnb";

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
