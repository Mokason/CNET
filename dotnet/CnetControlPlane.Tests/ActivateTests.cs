using System.Text.Json;
using CnetControlPlane.Activate;
using Microsoft.Data.Sqlite;

namespace CnetControlPlane.Tests;

public class ActivateTests : IDisposable
{
    private readonly string _root;
    private readonly string _db;
    private readonly string _recovery;
    private readonly string _acceptance;
    private const string OldSha = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    private const string NewSha = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

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

    public ActivateTests()
    {
        _root = Directory.CreateTempSubdirectory("cnet-activate-").FullName;
        _db = Path.Combine(_root, "registry.db");
        _recovery = Path.Combine(_root, "recovery.json");
        _acceptance = Path.Combine(_root, "acceptance.json");
        using (var connection = new SqliteConnection($"Data Source={_db};Pooling=False"))
        {
            connection.Open();
            using var cmd = connection.CreateCommand();
            cmd.CommandText = Schema;
            cmd.ExecuteNonQuery();
        }
        WriteReports();
    }

    public void Dispose()
    {
        SqliteConnection.ClearAllPools();
        try { Directory.Delete(_root, true); } catch { /* Windows file locks */ }
    }

    private long Insert(string status = "proposed", bool actionable = true, int priority = 5,
        string source = "cnet_real_model_acceptance", string oldSha = OldSha)
    {
        var meta = JsonSerializer.Serialize(new
        {
            actionable,
            candidate_sha256 = oldSha,
            admission_reasons = new[] { "quality_regression" },
        });
        using var connection = new SqliteConnection($"Data Source={_db};Pooling=False");
        connection.Open();
        using var cmd = connection.CreateCommand();
        cmd.CommandText = """
            INSERT INTO suggestions
            (hash,area,title,suggestion,target_file,priority,source,status,notes,created_at,meta)
            VALUES ($hash,$area,$title,$suggestion,$target,$priority,$source,$status,'',$created,$meta)
            """;
        cmd.Parameters.AddWithValue("$hash", $"hash-{source}-{status}-{actionable}-{priority}-{Guid.NewGuid():N}");
        cmd.Parameters.AddWithValue("$area", "cnet-model-compression");
        cmd.Parameters.AddWithValue("$title", "Repair quarantined real-model compression candidate");
        cmd.Parameters.AddWithValue("$suggestion", "repair");
        cmd.Parameters.AddWithValue("$target", "dotnet/CnetControlPlane");
        cmd.Parameters.AddWithValue("$priority", priority);
        cmd.Parameters.AddWithValue("$source", source);
        cmd.Parameters.AddWithValue("$status", status);
        cmd.Parameters.AddWithValue("$created", "2026-07-16T00:00:00Z");
        cmd.Parameters.AddWithValue("$meta", meta);
        cmd.ExecuteNonQuery();
        cmd.CommandText = "SELECT last_insert_rowid()";
        return (long)cmd.ExecuteScalar()!;
    }

    private void WriteReports(bool admitted = true, double qualityDelta = 1.0 / 3, double maxQualityDelta = 0.0, string oldSha = OldSha)
    {
        File.WriteAllText(_recovery, JsonSerializer.Serialize(new
        {
            schema_version = 1,
            root_cause = new { quarantined_sha256 = oldSha },
            replacement = new { sha256 = NewSha },
            acceptance = new
            {
                verdict = admitted ? "PASS_CANDIDATE_ADMITTED" : "PASS_CANDIDATE_QUARANTINED",
                quality_delta = qualityDelta,
                qgkp_byte_identical = true,
                restart_responses_identical = true,
                restart_quality_preserved = true,
            },
            hermes = new { status = "pass", matched = true, returncode = 0, cpu_only = true, loopback_only = true },
            verdict = admitted ? "QWYTHOS_RECOVERY_AND_HERMES_ACCEPTANCE_PASS" : "RECOVERY_FAILED",
        }));
        File.WriteAllText(_acceptance, JsonSerializer.Serialize(new
        {
            schema_version = 2,
            execution = new { cpu_only = true, max_quality_delta = maxQualityDelta },
            artifacts = new
            {
                candidate = new { sha256 = NewSha },
                qgkp = new { round_trip = new { byte_identical = true } },
            },
            admission = new
            {
                admitted,
                selected_role = admitted ? "candidate" : "reference",
                quality_delta = qualityDelta,
                reasons = admitted ? Array.Empty<string>() : new[] { "quality_regression" },
            },
            restart_integrity = new { responses_identical = true, quality_preserved = true },
            overall_pass = true,
            verdict = admitted ? "PASS_CANDIDATE_ADMITTED" : "PASS_CANDIDATE_QUARANTINED",
        }));
    }

    private string RowStatus(long id)
    {
        using var connection = new SqliteConnection($"Data Source={_db};Pooling=False");
        connection.Open();
        using var cmd = connection.CreateCommand();
        cmd.CommandText = "SELECT status FROM suggestions WHERE id=$id";
        cmd.Parameters.AddWithValue("$id", id);
        return (string)cmd.ExecuteScalar()!;
    }

    private string RowMeta(long id)
    {
        using var connection = new SqliteConnection($"Data Source={_db};Pooling=False");
        connection.Open();
        using var cmd = connection.CreateCommand();
        cmd.CommandText = "SELECT meta FROM suggestions WHERE id=$id";
        cmd.Parameters.AddWithValue("$id", id);
        return (string)cmd.ExecuteScalar()!;
    }

    [Fact]
    public void Selects_only_actionable_cnet_rows()
    {
        var wanted = Insert();
        Insert(actionable: false, priority: 4);
        Insert(source: "research", priority: 3);
        var rows = SuggestionActivation.SelectActionable(_db);
        Assert.Equal(new[] { wanted }, rows.Select(r => Convert.ToInt64(r["id"])));
    }

    [Fact]
    public void Dry_run_is_pure()
    {
        var rowId = Insert();
        var result = SuggestionActivation.Activate(_db, _recovery, _acceptance, dryRun: true);
        Assert.Equal("dry_run", result["status"]);
        Assert.Equal("proposed", RowStatus(rowId));
    }

    [Fact]
    public void Valid_evidence_verifies_exactly_one_row()
    {
        var first = Insert(priority: 5);
        var second = Insert(priority: 4);
        var result = SuggestionActivation.Activate(_db, _recovery, _acceptance);
        Assert.Equal("verified", result["status"]);
        Assert.Equal(1, Convert.ToInt32(result["processed"]));
        Assert.Equal("verified", RowStatus(first));
        Assert.Equal("proposed", RowStatus(second));
        Assert.False(JsonDocument.Parse(RowMeta(first)).RootElement.GetProperty("actionable").GetBoolean());
    }

    [Fact]
    public void In_progress_row_resumes_after_crash()
    {
        var rowId = Insert(status: "in_progress");
        var result = SuggestionActivation.Activate(_db, _recovery, _acceptance);
        Assert.True(Convert.ToBoolean(result["resumed"]));
        Assert.Equal("verified", RowStatus(rowId));
    }

    [Fact]
    public void Crash_after_claim_leaves_resumable_state()
    {
        var rowId = Insert();
        Assert.Throws<ActivationInterrupted>(() =>
            SuggestionActivation.Activate(_db, _recovery, _acceptance, crashAfterClaim: true));
        Assert.Equal("in_progress", RowStatus(rowId));
        Assert.Equal("verified", SuggestionActivation.Activate(_db, _recovery, _acceptance)["status"]);
    }

    [Fact]
    public void Policy_weakening_is_archived_not_accepted()
    {
        var rowId = Insert();
        WriteReports(maxQualityDelta: 0.1);
        var result = SuggestionActivation.Activate(_db, _recovery, _acceptance);
        Assert.Equal("archived", result["status"]);
        Assert.Contains("max_quality_delta_not_zero", (List<string>)result["reasons"]!);
        Assert.Equal("archived", RowStatus(rowId));
    }

    [Fact]
    public void Persistent_quality_regression_is_archived()
    {
        var rowId = Insert();
        WriteReports(admitted: false, qualityDelta: -1.0 / 3);
        var result = SuggestionActivation.Activate(_db, _recovery, _acceptance);
        Assert.Equal("archived", result["status"]);
        Assert.Equal("archived", RowStatus(rowId));
        Assert.Empty(SuggestionActivation.SelectActionable(_db));
    }

    [Fact]
    public void Candidate_sha_must_link_task_to_recovery()
    {
        var rowId = Insert(oldSha: new string('c', 64));
        var result = SuggestionActivation.Activate(_db, _recovery, _acceptance);
        Assert.Equal("archived", result["status"]);
        Assert.Contains("quarantined_sha256_mismatch", (List<string>)result["reasons"]!);
        Assert.Equal("archived", RowStatus(rowId));
    }
}
