using System.Text.Json;
using CnetControlPlane.Ingest;
using Microsoft.Data.Sqlite;

namespace CnetControlPlane.Tests;

public class IngestTests : IDisposable
{
    private readonly string _root;
    private readonly string _db;
    private readonly string _jsonl;
    private readonly string _report;

    private const string Schema = """
        CREATE TABLE suggestions (
         id INTEGER PRIMARY KEY AUTOINCREMENT, hash TEXT UNIQUE NOT NULL,
         area TEXT NOT NULL, title TEXT NOT NULL, suggestion TEXT NOT NULL,
         target_file TEXT DEFAULT '', priority INTEGER DEFAULT 3,
         source TEXT DEFAULT 'research', status TEXT DEFAULT 'proposed',
         original_id INTEGER, notes TEXT DEFAULT '', created_at TEXT NOT NULL,
         updated_at TEXT, verified_at TEXT, score REAL DEFAULT 0.0,
         meta TEXT DEFAULT '{}'
        );
        """;

    public IngestTests()
    {
        _root = Directory.CreateTempSubdirectory("cnet-ingest-").FullName;
        _db = Path.Combine(_root, "registry.db");
        using (var connection = new SqliteConnection($"Data Source={_db};Pooling=False"))
        {
            connection.Open();
            using var cmd = connection.CreateCommand();
            cmd.CommandText = Schema;
            cmd.ExecuteNonQuery();
        }

        _jsonl = Path.Combine(_root, "suggestions.jsonl");
        var rows = new[]
        {
            new Dictionary<string, object?>
            {
                ["id"] = "cnet-phase-4",
                ["title"] = "Hermes integration",
                ["source"] = "cnet_model_compression_phase_log",
                ["plan"] = "plan.md",
                ["status"] = "implemented",
                ["priority"] = 0.84,
                ["description"] = "Implemented and verified.",
                ["evidence"] = new Dictionary<string, object?> { ["date"] = "2026-07-06", ["tests"] = new[] { "make phase4" } },
                ["next_steps"] = new[] { "old step" },
                ["tags"] = new[] { "cnet" },
            },
        };
        File.WriteAllText(_jsonl, string.Join('\n', rows.Select(r => JsonSerializer.Serialize(r))) + "\n");
        _report = Path.Combine(_root, "report.json");
        File.WriteAllText(_report, JsonSerializer.Serialize(new
        {
            overall_pass = true,
            verdict = "PASS_CANDIDATE_QUARANTINED",
            artifacts = new { candidate = new { sha256 = new string('a', 64), path = "/tmp/bad.gguf" } },
            admission = new { admitted = false, reasons = new[] { "quality_regression", "candidate_probe_failure" } },
            quality = new { quality_delta = -0.66 },
        }));
    }

    public void Dispose()
    {
        SqliteConnection.ClearAllPools();
        try { Directory.Delete(_root, true); } catch { /* Windows file locks */ }
    }

    [Fact]
    public void Import_preserves_implemented_status_and_creates_evidence_followup()
    {
        var result = SuggestionIngest.Ingest(_jsonl, _db, _report);
        Assert.Equal(2, result["inserted"]);
        Assert.Equal(1, result["proposed"]);
        using (var connection = new SqliteConnection($"Data Source={_db};Pooling=False"))
        {
            connection.Open();
            using var cmd = connection.CreateCommand();
            cmd.CommandText = "SELECT title,status,priority,meta FROM suggestions ORDER BY id";
            using var reader = cmd.ExecuteReader();
            Assert.True(reader.Read());
            Assert.Equal("implemented", reader.GetString(1));
            Assert.True(reader.Read());
            Assert.Equal("proposed", reader.GetString(1));
            Assert.Equal(5, reader.GetInt32(2));
            using var meta = JsonDocument.Parse(reader.GetString(3));
            Assert.Equal(new string('a', 64), meta.RootElement.GetProperty("candidate_sha256").GetString());
            Assert.Contains(meta.RootElement.GetProperty("admission_reasons").EnumerateArray().Select(x => x.GetString()),
                r => r == "quality_regression");
        }
    }

    [Fact]
    public void Reingest_is_deduplicated_by_stable_hash()
    {
        var first = SuggestionIngest.Ingest(_jsonl, _db, _report);
        var second = SuggestionIngest.Ingest(_jsonl, _db, _report);
        Assert.Equal(2, first["inserted"]);
        Assert.Equal(0, second["inserted"]);
        Assert.Equal(2, second["skipped"]);
        using (var connection = new SqliteConnection($"Data Source={_db};Pooling=False"))
        {
            connection.Open();
            using var cmd = connection.CreateCommand();
            cmd.CommandText = "SELECT COUNT(*) FROM suggestions";
            Assert.Equal(2L, (long)cmd.ExecuteScalar()!);
        }
    }

    [Fact]
    public void Admitted_candidate_does_not_schedule_repair()
    {
        using var doc = JsonDocument.Parse(File.ReadAllText(_report));
        var node = JsonSerializer.Deserialize<Dictionary<string, JsonElement>>(File.ReadAllText(_report))!;
        var report = JsonSerializer.Deserialize<Dictionary<string, object>>(File.ReadAllText(_report))!;
        File.WriteAllText(_report, """
            {"overall_pass":true,"verdict":"PASS_CANDIDATE_ADMITTED","artifacts":{"candidate":{"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","path":"/tmp/bad.gguf"}},"admission":{"admitted":true,"reasons":[]},"quality":{"quality_delta":0.0}}
            """);
        var result = SuggestionIngest.Ingest(_jsonl, _db, _report);
        Assert.Equal(1, result["inserted"]);
        Assert.Equal(0, result["proposed"]);
    }
}
