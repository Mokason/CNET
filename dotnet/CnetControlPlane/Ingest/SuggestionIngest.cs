using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Data.Sqlite;

namespace CnetControlPlane.Ingest;

public static class SuggestionIngest
{
    private static readonly HashSet<string> RequiredColumns =
    [
        "hash", "area", "title", "suggestion", "target_file", "priority",
        "source", "status", "notes", "created_at", "score", "meta",
    ];

    public static string Digest(string key)
    {
        var hash = SHA256.HashData(Encoding.UTF8.GetBytes(key));
        return Convert.ToHexString(hash).ToLowerInvariant();
    }

    public static string Timestamp(string? value = null)
    {
        if (!string.IsNullOrEmpty(value))
            return value.Contains('T') ? value : $"{value}T00:00:00Z";
        return DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss");
    }

    public static IEnumerable<Dictionary<string, JsonElement>> ReadJsonl(string path)
    {
        var lineNumber = 0;
        foreach (var line in File.ReadLines(path))
        {
            lineNumber++;
            if (string.IsNullOrWhiteSpace(line))
                continue;
            using var doc = JsonDocument.Parse(line);
            if (doc.RootElement.ValueKind != JsonValueKind.Object
                || !doc.RootElement.TryGetProperty("id", out _))
            {
                throw new ArgumentException($"invalid suggestion row at {path}:{lineNumber}");
            }
            yield return doc.RootElement.EnumerateObject()
                .ToDictionary(p => p.Name, p => p.Value.Clone());
        }
    }

    public static Dictionary<string, object?> MilestoneRecord(Dictionary<string, JsonElement> row)
    {
        var id = row["id"].GetString()!;
        var originalStatus = row.TryGetValue("status", out var st) ? st.GetString() ?? "" : "";
        var status = originalStatus.StartsWith("implemented", StringComparison.Ordinal)
            ? "implemented" : "proposed";
        var priorityRaw = row.TryGetValue("priority", out var pr) ? pr.GetDouble() : 0.6;
        var priority = Math.Max(1, Math.Min(5, (int)Math.Round(priorityRaw * 5)));
        Dictionary<string, object?> evidence = new();
        if (row.TryGetValue("evidence", out var ev) && ev.ValueKind == JsonValueKind.Object)
            evidence = JsonUtil.ToDict(ev)!;

        var meta = new JsonObject
        {
            ["cnet_id"] = id,
            ["plan"] = row.TryGetValue("plan", out var plan) ? plan.GetString() : null,
            ["original_status"] = originalStatus,
            ["evidence"] = JsonSerializer.SerializeToNode(evidence),
            ["next_steps"] = row.TryGetValue("next_steps", out var ns)
                ? JsonSerializer.SerializeToNode(JsonUtil.ToClr(ns))
                : new JsonArray(),
            ["tags"] = row.TryGetValue("tags", out var tags)
                ? JsonSerializer.SerializeToNode(JsonUtil.ToClr(tags))
                : new JsonArray(),
            ["actionable"] = status == "proposed",
        };

        var date = evidence.TryGetValue("date", out var d) ? d?.ToString() : null;
        return new Dictionary<string, object?>
        {
            ["hash"] = Digest($"cnet:milestone:{id}"),
            ["area"] = "cnet-model-compression",
            ["title"] = row.TryGetValue("title", out var title) && title.ValueKind == JsonValueKind.String
                ? title.GetString()! : id,
            ["suggestion"] = row.TryGetValue("description", out var desc) && desc.ValueKind == JsonValueKind.String
                ? desc.GetString()! : "CNET evidence milestone",
            ["target_file"] = row.TryGetValue("plan", out var p) && p.ValueKind == JsonValueKind.String
                ? p.GetString()! : "",
            ["priority"] = priority,
            ["source"] = row.TryGetValue("source", out var src) && src.ValueKind == JsonValueKind.String
                ? src.GetString()! : "cnet",
            ["status"] = status,
            ["notes"] = "Imported from CNET Phase 5 evidence export.",
            ["created_at"] = Timestamp(string.IsNullOrEmpty(date) ? null : date),
            ["score"] = row.TryGetValue("priority", out var score) ? score.GetDouble() : 0.0,
            ["meta"] = JsonUtil.CompactCanonical(meta),
        };
    }

    public static Dictionary<string, object?>? CandidateRecord(JsonElement report)
    {
        if (!report.TryGetProperty("overall_pass", out var overall) || overall.ValueKind != JsonValueKind.True)
            throw new ArgumentException("acceptance report is not a valid passing campaign");

        var admission = report.TryGetProperty("admission", out var adm) ? adm : default;
        if (admission.ValueKind == JsonValueKind.Object
            && admission.TryGetProperty("admitted", out var admitted)
            && admitted.ValueKind == JsonValueKind.True)
        {
            return null;
        }

        var candidate = report.TryGetProperty("artifacts", out var arts)
            && arts.TryGetProperty("candidate", out var cand)
            ? cand : default;
        var digest = candidate.ValueKind == JsonValueKind.Object
            && candidate.TryGetProperty("sha256", out var sha)
            ? sha.GetString() ?? "" : "";
        var reasons = new List<string>();
        if (admission.ValueKind == JsonValueKind.Object
            && admission.TryGetProperty("reasons", out var r)
            && r.ValueKind == JsonValueKind.Array)
        {
            reasons.AddRange(r.EnumerateArray().Select(x => x.ToString()));
        }
        if (digest.Length != 64 || reasons.Count == 0)
            throw new ArgumentException("quarantined candidate lacks digest or admission reasons");

        var quality = report.TryGetProperty("quality", out var q) ? q : default;
        var delta = quality.ValueKind == JsonValueKind.Object
            && quality.TryGetProperty("quality_delta", out var qd)
            ? qd.GetDouble() : 0.0;

        var meta = new JsonObject
        {
            ["candidate_sha256"] = digest,
            ["candidate_path"] = candidate.TryGetProperty("path", out var path) ? path.GetString() : null,
            ["admission_reasons"] = JsonSerializer.SerializeToNode(reasons),
            ["campaign_verdict"] = report.TryGetProperty("verdict", out var v) ? v.GetString() : null,
            ["quality"] = quality.ValueKind == JsonValueKind.Object
                ? JsonSerializer.SerializeToNode(JsonUtil.ToDict(quality))
                : new JsonObject(),
            ["actionable"] = true,
        };

        var generated = report.TryGetProperty("generated_at", out var ga) ? ga.GetString() : null;
        return new Dictionary<string, object?>
        {
            ["hash"] = Digest($"cnet:candidate-quarantine:{digest}"),
            ["area"] = "cnet-model-compression",
            ["title"] = "Repair quarantined real-model compression candidate",
            ["suggestion"] =
                "Repair or replace the quarantined candidate and rerun the bounded CPU "
                + $"acceptance campaign. Admission failed: {string.Join(", ", reasons)}.",
            ["target_file"] = "dotnet/CnetControlPlane (real-model-acceptance)",
            ["priority"] = 5,
            ["source"] = "cnet_real_model_acceptance",
            ["status"] = "proposed",
            ["notes"] = "Generated only from a valid campaign with candidate admission denied.",
            ["created_at"] = Timestamp(string.IsNullOrEmpty(generated) ? null : generated),
            ["score"] = Math.Abs(delta),
            ["meta"] = JsonUtil.CompactCanonical(meta),
        };
    }

    private static void ValidateSchema(SqliteConnection connection)
    {
        var columns = new HashSet<string>();
        using var cmd = connection.CreateCommand();
        cmd.CommandText = "PRAGMA table_info(suggestions)";
        using var reader = cmd.ExecuteReader();
        while (reader.Read())
            columns.Add(reader.GetString(1));
        var missing = RequiredColumns.Except(columns).OrderBy(x => x).ToList();
        if (missing.Count > 0)
            throw new ArgumentException($"SuggestionRegistry schema missing columns: [{string.Join(", ", missing.Select(m => $"'{m}'"))}]");
    }

    public static Dictionary<string, int> Ingest(string suggestions, string db, string? acceptanceReport = null)
    {
        var records = new List<Dictionary<string, object?>>();
        foreach (var row in ReadJsonl(suggestions))
            records.Add(MilestoneRecord(row));

        if (acceptanceReport is not null)
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(acceptanceReport));
            var followup = CandidateRecord(doc.RootElement);
            if (followup is not null)
                records.Add(followup);
        }

        var inserted = 0;
        using var connection = new SqliteConnection($"Data Source={db};Pooling=False");
        connection.Open();
        ValidateSchema(connection);
        using (var tx = connection.BeginTransaction())
        {
            foreach (var record in records)
            {
                using var cmd = connection.CreateCommand();
                cmd.Transaction = tx;
                cmd.CommandText = """
                    INSERT OR IGNORE INTO suggestions
                    (hash,area,title,suggestion,target_file,priority,source,status,notes,created_at,score,meta)
                    VALUES ($hash,$area,$title,$suggestion,$target_file,$priority,$source,$status,$notes,$created_at,$score,$meta)
                    """;
                foreach (var key in new[]
                         {
                             "hash", "area", "title", "suggestion", "target_file", "priority",
                             "source", "status", "notes", "created_at", "score", "meta",
                         })
                {
                    cmd.Parameters.AddWithValue("$" + key, record[key] ?? DBNull.Value);
                }
                inserted += cmd.ExecuteNonQuery();
            }
            tx.Commit();
        }

        using var countCmd = connection.CreateCommand();
        countCmd.CommandText = """
            SELECT COUNT(*) FROM suggestions
            WHERE source IN ($s1,$s2) AND status='proposed'
            """;
        countCmd.Parameters.AddWithValue("$s1", "cnet_model_compression_phase_log");
        countCmd.Parameters.AddWithValue("$s2", "cnet_real_model_acceptance");
        var proposed = Convert.ToInt32(countCmd.ExecuteScalar());
        return new Dictionary<string, int>
        {
            ["read"] = records.Count,
            ["inserted"] = inserted,
            ["skipped"] = records.Count - inserted,
            ["proposed"] = proposed,
        };
    }

    public static int RunCli(string[] args)
    {
        string? suggestions = null, db = null, acceptance = null;
        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--suggestions": suggestions = args[++i]; break;
                case "--db": db = args[++i]; break;
                case "--acceptance-report": acceptance = args[++i]; break;
                default: throw new ArgumentException($"unknown argument: {args[i]}");
            }
        }
        if (suggestions is null || db is null)
            throw new ArgumentException("--suggestions and --db are required");
        try
        {
            var result = Ingest(suggestions, db, acceptance);
            var payload = new Dictionary<string, object?> { ["status"] = "ok" };
            foreach (var kv in result) payload[kv.Key] = kv.Value;
            Console.WriteLine(JsonSerializer.Serialize(payload, new JsonSerializerOptions { WriteIndented = false }));
            return 0;
        }
        catch (Exception ex) when (ex is IOException or ArgumentException or JsonException or SqliteException)
        {
            Console.WriteLine(JsonSerializer.Serialize(new { status = "error", reason = ex.Message }));
            return 1;
        }
    }
}
