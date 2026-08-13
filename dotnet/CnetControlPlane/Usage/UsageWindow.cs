using System.Text.Json;

namespace CnetControlPlane.Usage;

public static class UsageWindow
{
    public const string DefaultTokenizerDir = "/home/marble/AI/Models/gemma-4-31B-it-int4-AutoRound";

    public static HashSet<int> LoadExcludes(IEnumerable<string> paths)
    {
        var seen = new HashSet<int>();
        foreach (var p in paths)
        {
            foreach (var line in File.ReadAllLines(p))
            {
                var trimmed = line.Trim();
                if (trimmed.Length == 0)
                    continue;
                var tok = trimmed.Split(' ', StringSplitOptions.RemoveEmptyEntries)[0];
                if (int.TryParse(tok, out var id))
                    seen.Add(id);
                else if (tok.StartsWith("tk", StringComparison.Ordinal))
                {
                    var digits = tok[2..].Split('q')[0];
                    if (int.TryParse(digits, out var tid))
                        seen.Add(tid);
                }
            }
        }
        return seen;
    }

    public static (List<(int Tid, int Count)> Picked, int DemandDistinct, int Lines, string OutPath) BuildWindow(
        string usagePath,
        int windowSize,
        IEnumerable<string> excludes,
        string? outPath,
        string tokenizerDir)
    {
        var demand = new Dictionary<int, int>();
        var lines = 0;
        foreach (var raw in File.ReadAllLines(usagePath))
        {
            if (string.IsNullOrWhiteSpace(raw))
                continue;
            try
            {
                using var doc = JsonDocument.Parse(raw);
                lines++;
                if (!doc.RootElement.TryGetProperty("uncovered", out var uncovered)
                    || uncovered.ValueKind != JsonValueKind.Array)
                {
                    continue;
                }
                foreach (var tid in uncovered.EnumerateArray())
                {
                    var id = tid.GetInt32();
                    demand[id] = demand.GetValueOrDefault(id) + 1;
                }
            }
            catch (JsonException)
            {
                /* skip */
            }
        }

        var excluded = LoadExcludes(excludes);
        var ranked = demand.OrderByDescending(kv => kv.Value)
            .ThenBy(kv => kv.Key)
            .Where(kv => !excluded.Contains(kv.Key))
            .Select(kv => (kv.Key, kv.Value))
            .ToList();
        if (ranked.Count < windowSize)
        {
            Console.Error.WriteLine(
                $"note: only {ranked.Count} uncovered ids in demand "
                + $"(from {lines} verifications); window will be short of "
                + $"V={windowSize} — mine it with V={ranked.Count}");
        }
        var picked = ranked.Take(windowSize).ToList();
        if (picked.Count == 0)
            throw new InvalidOperationException("no uncovered demand recorded — nothing to mine");

        var outFile = outPath ?? usagePath.Replace(".usage.jsonl", "") + $".demand_window_{picked.Count}.txt";
        File.WriteAllText(outFile, string.Concat(picked.Select(p => $"{p.Key}\n")));

        var tokJson = Path.Combine(tokenizerDir, "tokenizer.json");
        if (File.Exists(tokJson))
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(tokJson));
            var vocab = doc.RootElement.GetProperty("model").GetProperty("vocab");
            var id2tok = new Dictionary<int, string>();
            foreach (var prop in vocab.EnumerateObject())
                id2tok[prop.Value.GetInt32()] = prop.Name;
            var side = Path.ChangeExtension(outFile, ".words.txt");
            if (side == outFile)
                side = outFile + ".words.txt";
            // Python: out.with_suffix(".words.txt") — for .txt becomes .words.txt
            side = Path.Combine(
                Path.GetDirectoryName(outFile) ?? ".",
                Path.GetFileNameWithoutExtension(outFile) + ".words.txt");
            File.WriteAllText(side, string.Concat(
                picked.Select(p => $"{p.Key}\t{id2tok.GetValueOrDefault(p.Key, "?")}\t{p.Value}\n")));
            Console.WriteLine($"wrote {outFile} and {side}");
        }
        else
        {
            Console.WriteLine($"wrote {outFile}");
        }
        Console.WriteLine(
            $"demand: {demand.Count} distinct uncovered ids over {lines} "
            + $"verifications; window: {picked.Count}");
        return (picked, demand.Count, lines, outFile);
    }

    public static int RunCli(string[] args)
    {
        if (args.Length == 0)
        {
            Console.Error.WriteLine("usage: usage-window <usage.jsonl> [-V 256] [--exclude ...] [--tokenizer-dir DIR] [-o out]");
            return 2;
        }
        var usage = args[0];
        var window = 256;
        var excludes = new List<string>();
        var tokenizerDir = DefaultTokenizerDir;
        string? outPath = null;
        for (var i = 1; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "-V": window = int.Parse(args[++i]); break;
                case "--exclude": excludes.Add(args[++i]); break;
                case "--tokenizer-dir": tokenizerDir = args[++i]; break;
                case "-o":
                case "--out": outPath = args[++i]; break;
                default: throw new ArgumentException($"unknown argument: {args[i]}");
            }
        }
        try
        {
            BuildWindow(usage, window, excludes, outPath, tokenizerDir);
            return 0;
        }
        catch (InvalidOperationException ex)
        {
            Console.Error.WriteLine(ex.Message);
            return 1;
        }
    }
}
