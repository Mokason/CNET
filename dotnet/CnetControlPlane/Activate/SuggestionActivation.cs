using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Data.Sqlite;

namespace CnetControlPlane.Activate;

public sealed class ActivationInterrupted : Exception
{
    public ActivationInterrupted(string message) : base(message) { }
}

public sealed class EvidenceUnavailable : Exception
{
    public EvidenceUnavailable(string message) : base(message) { }
}

public static class SuggestionActivation
{
    public const string Area = "cnet-model-compression";
    public const string Source = "cnet_real_model_acceptance";
    public const double MaxQualityDelta = 0.0;
    public const string ValidRecoveryVerdict = "QWYTHOS_RECOVERY_AND_HERMES_ACCEPTANCE_PASS";
    public const string ValidAcceptanceVerdict = "PASS_CANDIDATE_ADMITTED";

    private static string Timestamp() =>
        DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss");

    private static SqliteConnection Connect(string db)
    {
        var connection = new SqliteConnection($"Data Source={db};Pooling=False");
        connection.Open();
        return connection;
    }

    private static string ActionableQuery(int limit = 1) => $"""
        SELECT * FROM suggestions
        WHERE area=$area AND source=$source
          AND status IN ('in_progress','proposed')
          AND json_valid(meta)
          AND json_extract(meta, '$.actionable') = 1
        ORDER BY CASE status WHEN 'in_progress' THEN 0 ELSE 1 END,
                 priority DESC, created_at ASC, id ASC
        LIMIT {limit}
        """;

    public static List<Dictionary<string, object?>> SelectActionable(string db, int limit = 1)
    {
        if (limit < 1)
            throw new ArgumentException("limit must be positive");
        using var connection = Connect(db);
        using var cmd = connection.CreateCommand();
        cmd.CommandText = ActionableQuery(limit);
        cmd.Parameters.AddWithValue("$area", Area);
        cmd.Parameters.AddWithValue("$source", Source);
        return ReadRows(cmd);
    }

    private static List<Dictionary<string, object?>> ReadRows(SqliteCommand cmd)
    {
        var rows = new List<Dictionary<string, object?>>();
        using var reader = cmd.ExecuteReader();
        while (reader.Read())
        {
            var row = new Dictionary<string, object?>();
            for (var i = 0; i < reader.FieldCount; i++)
                row[reader.GetName(i)] = reader.IsDBNull(i) ? null : reader.GetValue(i);
            rows.Add(row);
        }
        return rows;
    }

    private static (Dictionary<string, object?>? Row, bool Resumed) ClaimOne(string db)
    {
        using var connection = Connect(db);
        using var begin = connection.CreateCommand();
        begin.CommandText = "BEGIN IMMEDIATE";
        begin.ExecuteNonQuery();

        using var select = connection.CreateCommand();
        select.CommandText = ActionableQuery(1);
        select.Parameters.AddWithValue("$area", Area);
        select.Parameters.AddWithValue("$source", Source);
        var rows = ReadRows(select);
        if (rows.Count == 0)
        {
            using var commitEmpty = connection.CreateCommand();
            commitEmpty.CommandText = "COMMIT";
            commitEmpty.ExecuteNonQuery();
            return (null, false);
        }

        var row = rows[0];
        var resumed = string.Equals(Convert.ToString(row["status"]), "in_progress", StringComparison.Ordinal);
        var meta = JsonNode.Parse(Convert.ToString(row["meta"])!)!.AsObject();
        var activation = meta["activation"] as JsonObject ?? new JsonObject();
        var attempt = activation["attempt_count"]?.GetValue<int>() ?? 0;
        activation["attempt_count"] = attempt + 1;
        activation["state"] = "in_progress";
        activation["claimed_at"] = Timestamp();
        activation["resumed"] = resumed;
        activation["policy"] = new JsonObject { ["max_quality_delta"] = MaxQualityDelta };
        meta["activation"] = activation;

        using var update = connection.CreateCommand();
        update.CommandText = "UPDATE suggestions SET status='in_progress', updated_at=$u, meta=$m WHERE id=$id";
        update.Parameters.AddWithValue("$u", Timestamp());
        update.Parameters.AddWithValue("$m", JsonUtil.CompactCanonical(meta));
        update.Parameters.AddWithValue("$id", Convert.ToInt64(row["id"]));
        update.ExecuteNonQuery();

        using var commit = connection.CreateCommand();
        commit.CommandText = "COMMIT";
        commit.ExecuteNonQuery();

        var claimed = SelectActionable(db, 1);
        return (claimed.Count > 0 ? claimed[0] : null, resumed);
    }

    private static JsonElement LoadReport(string path, string label)
    {
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            if (doc.RootElement.ValueKind != JsonValueKind.Object)
                throw new EvidenceUnavailable($"{label}_not_object");
            return doc.RootElement.Clone();
        }
        catch (Exception ex) when (ex is IOException or JsonException)
        {
            throw new EvidenceUnavailable($"{label}_unavailable:{ex.Message}");
        }
    }

    private static double AsFloat(JsonElement el, double defaultValue)
    {
        try
        {
            return el.ValueKind switch
            {
                JsonValueKind.Number => el.GetDouble(),
                JsonValueKind.String when double.TryParse(el.GetString(), out var v) => v,
                _ => defaultValue,
            };
        }
        catch
        {
            return defaultValue;
        }
    }

    public static (List<string> Reasons, Dictionary<string, object?> Evidence) ValidateEvidence(
        Dictionary<string, object?> row,
        string recoveryReport,
        string acceptanceReport)
    {
        var recovery = LoadReport(recoveryReport, "recovery_report");
        var acceptance = LoadReport(acceptanceReport, "acceptance_report");
        var reasons = new List<string>();
        using var metaDoc = JsonDocument.Parse(Convert.ToString(row["meta"])!);
        var meta = metaDoc.RootElement;

        var taskSha = meta.TryGetProperty("candidate_sha256", out var ts) ? ts.GetString() ?? "" : "";
        var recoveredFrom = recovery.TryGetProperty("root_cause", out var rc)
            && rc.TryGetProperty("quarantined_sha256", out var qs)
            ? qs.GetString() ?? "" : "";
        var replacementSha = recovery.TryGetProperty("replacement", out var rep)
            && rep.TryGetProperty("sha256", out var rsha)
            ? rsha.GetString() ?? "" : "";
        var acceptedSha = acceptance.TryGetProperty("artifacts", out var arts)
            && arts.TryGetProperty("candidate", out var cand)
            && cand.TryGetProperty("sha256", out var asha)
            ? asha.GetString() ?? "" : "";

        if (taskSha.Length != 64 || recoveredFrom != taskSha)
            reasons.Add("quarantined_sha256_mismatch");
        if (replacementSha.Length != 64 || acceptedSha != replacementSha)
            reasons.Add("replacement_sha256_mismatch");

        var execution = acceptance.TryGetProperty("execution", out var ex) ? ex : default;
        if (execution.ValueKind != JsonValueKind.Object
            || !execution.TryGetProperty("cpu_only", out var cpu)
            || cpu.ValueKind != JsonValueKind.True)
        {
            reasons.Add("acceptance_not_cpu_only");
        }
        var policyDelta = execution.ValueKind == JsonValueKind.Object
            && execution.TryGetProperty("max_quality_delta", out var mqd)
            ? AsFloat(mqd, double.NaN) : double.NaN;
        if (policyDelta != MaxQualityDelta)
            reasons.Add("max_quality_delta_not_zero");

        var admission = acceptance.TryGetProperty("admission", out var adm) ? adm : default;
        var qualityDelta = admission.ValueKind == JsonValueKind.Object
            && admission.TryGetProperty("quality_delta", out var qd)
            ? AsFloat(qd, double.NegativeInfinity) : double.NegativeInfinity;
        if (admission.ValueKind != JsonValueKind.Object
            || !admission.TryGetProperty("admitted", out var admitted)
            || admitted.ValueKind != JsonValueKind.True)
        {
            reasons.Add("candidate_not_admitted");
        }
        if (admission.ValueKind != JsonValueKind.Object
            || !admission.TryGetProperty("selected_role", out var role)
            || role.GetString() != "candidate")
        {
            reasons.Add("candidate_not_selected");
        }
        if (qualityDelta < -MaxQualityDelta)
            reasons.Add("quality_regression");
        if (admission.ValueKind == JsonValueKind.Object
            && admission.TryGetProperty("reasons", out var reasonsEl)
            && reasonsEl.ValueKind == JsonValueKind.Array
            && reasonsEl.GetArrayLength() > 0)
        {
            reasons.Add("admission_reasons_present");
        }

        var qgkp = acceptance.TryGetProperty("artifacts", out var a2)
            && a2.TryGetProperty("qgkp", out var q)
            && q.TryGetProperty("round_trip", out var rt) ? rt : default;
        if (qgkp.ValueKind != JsonValueKind.Object
            || !qgkp.TryGetProperty("byte_identical", out var bi)
            || bi.ValueKind != JsonValueKind.True)
        {
            reasons.Add("qgkp_round_trip_failed");
        }
        var restart = acceptance.TryGetProperty("restart_integrity", out var ri) ? ri : default;
        if (restart.ValueKind != JsonValueKind.Object
            || !restart.TryGetProperty("responses_identical", out var resp)
            || resp.ValueKind != JsonValueKind.True)
        {
            reasons.Add("restart_responses_changed");
        }
        if (restart.ValueKind != JsonValueKind.Object
            || !restart.TryGetProperty("quality_preserved", out var qp)
            || qp.ValueKind != JsonValueKind.True)
        {
            reasons.Add("restart_quality_regressed");
        }
        if (!acceptance.TryGetProperty("overall_pass", out var op) || op.ValueKind != JsonValueKind.True)
            reasons.Add("acceptance_campaign_failed");
        if (!acceptance.TryGetProperty("verdict", out var av) || av.GetString() != ValidAcceptanceVerdict)
            reasons.Add("acceptance_verdict_invalid");

        var recoveryAcceptance = recovery.TryGetProperty("acceptance", out var ra) ? ra : default;
        if (recoveryAcceptance.ValueKind != JsonValueKind.Object
            || !recoveryAcceptance.TryGetProperty("qgkp_byte_identical", out var rq)
            || rq.ValueKind != JsonValueKind.True)
        {
            reasons.Add("recovery_qgkp_failed");
        }
        if (recoveryAcceptance.ValueKind != JsonValueKind.Object
            || !recoveryAcceptance.TryGetProperty("restart_responses_identical", out var rr)
            || rr.ValueKind != JsonValueKind.True)
        {
            reasons.Add("recovery_restart_changed");
        }
        if (recoveryAcceptance.ValueKind != JsonValueKind.Object
            || !recoveryAcceptance.TryGetProperty("restart_quality_preserved", out var rqp)
            || rqp.ValueKind != JsonValueKind.True)
        {
            reasons.Add("recovery_restart_quality_regressed");
        }
        if (!recovery.TryGetProperty("verdict", out var rv) || rv.GetString() != ValidRecoveryVerdict)
            reasons.Add("recovery_verdict_invalid");

        var hermes = recovery.TryGetProperty("hermes", out var h) ? h : default;
        var hermesOk = hermes.ValueKind == JsonValueKind.Object
            && hermes.TryGetProperty("status", out var hs) && hs.GetString() == "pass"
            && hermes.TryGetProperty("matched", out var hm) && hm.ValueKind == JsonValueKind.True
            && hermes.TryGetProperty("returncode", out var hr) && hr.GetInt32() == 0
            && hermes.TryGetProperty("cpu_only", out var hc) && hc.ValueKind == JsonValueKind.True
            && hermes.TryGetProperty("loopback_only", out var hl) && hl.ValueKind == JsonValueKind.True;
        if (!hermesOk)
            reasons.Add("hermes_acceptance_failed");

        var unique = reasons.Distinct().ToList();
        var evidence = new Dictionary<string, object?>
        {
            ["task_candidate_sha256"] = taskSha,
            ["replacement_sha256"] = replacementSha,
            ["acceptance_quality_delta"] = qualityDelta,
            ["max_quality_delta"] = policyDelta,
            ["recovery_report"] = recoveryReport,
            ["acceptance_report"] = acceptanceReport,
        };
        return (unique, evidence);
    }

    private static void Finalize(
        string db,
        long rowId,
        string status,
        List<string> reasons,
        Dictionary<string, object?> evidence)
    {
        var now = Timestamp();
        using var connection = Connect(db);
        using var begin = connection.CreateCommand();
        begin.CommandText = "BEGIN IMMEDIATE";
        begin.ExecuteNonQuery();

        using var select = connection.CreateCommand();
        select.CommandText = "SELECT meta,notes,status FROM suggestions WHERE id=$id";
        select.Parameters.AddWithValue("$id", rowId);
        using var reader = select.ExecuteReader();
        if (!reader.Read())
            throw new SqliteException($"row {rowId} disappeared", 19);
        var currentStatus = reader.GetString(2);
        if (currentStatus != "in_progress")
            throw new SqliteException($"row {rowId} is not in_progress", 19);
        var meta = JsonNode.Parse(reader.GetString(0))!.AsObject();
        var oldNotes = reader.IsDBNull(1) ? "" : reader.GetString(1);
        reader.Close();

        meta["actionable"] = false;
        var activation = meta["activation"] as JsonObject ?? new JsonObject();
        activation["state"] = status;
        activation["completed_at"] = now;
        activation["reasons"] = JsonSerializer.SerializeToNode(reasons);
        activation["evidence"] = JsonSerializer.SerializeToNode(evidence);
        meta["activation"] = activation;

        var note = status == "verified"
            ? "Bounded CNET activation verified existing recovery evidence."
            : "Bounded CNET activation archived the row because evidence failed: " + string.Join(", ", reasons);
        var notes = $"{oldNotes.Trim()}\n{note}".Trim();

        using var update = connection.CreateCommand();
        update.CommandText = """
            UPDATE suggestions
               SET status=$status, updated_at=$u, verified_at=$v, notes=$notes, meta=$meta
             WHERE id=$id
            """;
        update.Parameters.AddWithValue("$status", status);
        update.Parameters.AddWithValue("$u", now);
        update.Parameters.AddWithValue("$v", status == "verified" ? now : DBNull.Value);
        update.Parameters.AddWithValue("$notes", notes);
        update.Parameters.AddWithValue("$meta", JsonUtil.CompactCanonical(meta));
        update.Parameters.AddWithValue("$id", rowId);
        update.ExecuteNonQuery();

        using var commit = connection.CreateCommand();
        commit.CommandText = "COMMIT";
        commit.ExecuteNonQuery();
    }

    public static Dictionary<string, object?> Activate(
        string db,
        string recoveryReport,
        string acceptanceReport,
        bool dryRun = false,
        bool crashAfterClaim = false)
    {
        var preview = SelectActionable(db, 1);
        if (preview.Count == 0)
            return new Dictionary<string, object?> { ["status"] = "idle", ["processed"] = 0 };
        if (dryRun)
        {
            var row = preview[0];
            return new Dictionary<string, object?>
            {
                ["status"] = "dry_run",
                ["processed"] = 0,
                ["would_process"] = Convert.ToInt64(row["id"]),
                ["resumable"] = Convert.ToString(row["status"]) == "in_progress",
            };
        }

        var (claimed, resumed) = ClaimOne(db);
        if (claimed is null)
            return new Dictionary<string, object?> { ["status"] = "idle", ["processed"] = 0 };
        if (crashAfterClaim)
            throw new ActivationInterrupted($"interrupted after durable claim of row {claimed["id"]}");

        try
        {
            var (reasons, evidence) = ValidateEvidence(claimed, recoveryReport, acceptanceReport);
            var finalStatus = reasons.Count > 0 ? "archived" : "verified";
            Finalize(db, Convert.ToInt64(claimed["id"]), finalStatus, reasons, evidence);
            return new Dictionary<string, object?>
            {
                ["status"] = finalStatus,
                ["processed"] = 1,
                ["row_id"] = Convert.ToInt64(claimed["id"]),
                ["resumed"] = resumed,
                ["reasons"] = reasons,
                ["evidence"] = evidence,
            };
        }
        catch (EvidenceUnavailable ex)
        {
            return new Dictionary<string, object?>
            {
                ["status"] = "evidence_unavailable",
                ["processed"] = 0,
                ["row_id"] = Convert.ToInt64(claimed["id"]),
                ["resumed"] = resumed,
                ["reason"] = ex.Message,
            };
        }
    }

    public static int RunCli(string[] args)
    {
        string? db = null, recovery = null, acceptance = null;
        var dryRun = false;
        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--db": db = args[++i]; break;
                case "--recovery-report": recovery = args[++i]; break;
                case "--acceptance-report": acceptance = args[++i]; break;
                case "--dry-run": dryRun = true; break;
                default: throw new ArgumentException($"unknown argument: {args[i]}");
            }
        }
        if (db is null || recovery is null || acceptance is null)
            throw new ArgumentException("--db, --recovery-report, and --acceptance-report are required");

        Dictionary<string, object?> result;
        try
        {
            result = Activate(db, recovery, acceptance, dryRun);
        }
        catch (Exception ex) when (ex is IOException or ArgumentException or SqliteException)
        {
            result = new Dictionary<string, object?> { ["status"] = "error", ["reason"] = ex.Message };
            Console.WriteLine(JsonSerializer.Serialize(result));
            return 2;
        }
        Console.WriteLine(JsonSerializer.Serialize(result));
        var status = Convert.ToString(result["status"]);
        if (status is "verified" or "idle" or "dry_run")
            return 0;
        if (status == "evidence_unavailable")
            return 2;
        return 1;
    }
}
