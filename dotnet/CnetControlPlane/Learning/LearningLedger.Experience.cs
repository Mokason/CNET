using Microsoft.Data.Sqlite;

namespace CnetControlPlane.Learning;

internal sealed record LearningExperience(long Sequence, string RequestId, string Dataset, byte Key, string Origin,
    string Boot, long StartedNanoseconds, string? FinishedBoot, long? FinishedNanoseconds,
    string State, string? SourceSha256, ushort? Expected, ushort? Value, string? ApprovedSourceSha256,
    ushort? ApprovedExpected, string? ApprovedBoot, long? ApprovedNanoseconds, bool ReviewConflict);

internal sealed partial class LearningLedger
{
    internal const int MaximumExperiences = 4096;
    internal const long ExperienceCompletionHeadroom = MaximumExperiences * 1024L + 65536;
    internal static bool IsRequestId(string id) => id is { Length: 32 }
        && id.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f');

    internal LocalTableReference? TaskSource(string dataset)
    {
        var authorized = policy.Datasets.SingleOrDefault(d => d.Id == dataset && d.SymbolVocabularySha256 is null)
            ?? throw new ArgumentException("learning_task_dataset_refused");
        using var data = LearningFiles.Open(Path.Combine(files.FullPath, "data"));
        if (!data.ValidateFile(dataset + ".tsv", allowMissing: true).HasValue) return null;
        var reference = LocalTableReference.Parse(data.Read(dataset + ".tsv", 4096), authorized);
        data.AssertPathIdentity();
        return reference;
    }

    private static LearningExperience ReadExperience(SqliteDataReader row) => new(row.GetInt64(0), row.GetString(1),
        row.GetString(2), checked((byte)row.GetInt64(3)), row.GetString(4), row.GetString(5), row.GetInt64(6),
        row.IsDBNull(7) ? null : row.GetString(7), row.IsDBNull(8) ? null : row.GetInt64(8), row.GetString(9),
        row.IsDBNull(10) ? null : row.GetString(10), row.IsDBNull(11) ? null : checked((ushort)row.GetInt64(11)),
        row.IsDBNull(12) ? null : checked((ushort)row.GetInt64(12)), row.IsDBNull(13) ? null : row.GetString(13),
        row.IsDBNull(14) ? null : checked((ushort)row.GetInt64(14)), row.IsDBNull(15) ? null : row.GetString(15),
        row.IsDBNull(16) ? null : row.GetInt64(16), row.GetInt64(17) != 0);

    private LearningExperience? Experience(string id, SqliteTransaction? tx)
    {
        using var cmd = Command("SELECT * FROM experiences WHERE request_id=$id", tx, ("$id", id));
        using var row = cmd.ExecuteReader();
        return row.Read() ? ReadExperience(row) : null;
    }

    internal IReadOnlyList<LearningExperience> Experiences(long after, int limit)
    {
        if (after < 0 || limit is < 1 or > 100) throw new ArgumentException("learning_task_page_refused");
        files.AssertPathIdentity(); RequireSchemaVersion();
        using var cmd = Command("SELECT * FROM experiences WHERE sequence>$after ORDER BY sequence LIMIT $limit", null,
            ("$after", after), ("$limit", limit));
        using var rows = cmd.ExecuteReader();
        var result = new List<LearningExperience>();
        while (rows.Read()) result.Add(ReadExperience(rows));
        return result.AsReadOnly();
    }

    private void RequireTaskAdmission(LearningInstant now, SqliteTransaction tx)
    {
        policy.RequireEnabled();
        if (Convert.ToInt64(Scalar("SELECT paused FROM configuration WHERE id=1", tx)) != 0)
            throw new InvalidOperationException("learning_paused");
        var run = ReadRun(tx);
        if (run is not null && (run.State != "running" || run.Boot != now.Boot
            || now.Nanoseconds - run.StartNanoseconds >= policy.MaxRunSeconds * 1_000_000_000L
            || now.Nanoseconds - run.LastNanoseconds > policy.MaxProbeGapSeconds * 1_000_000_000L))
            throw new InvalidOperationException("learning_task_run_unavailable");
    }

    internal (LearningExperience Experience, bool Created) BeginExperience(string id, string dataset, byte key, string origin)
    {
        if (!IsRequestId(id) || origin is not ("synthetic" or "unreviewed"))
            throw new ArgumentException("learning_task_identity_refused");
        Dataset(dataset);
        var result = Transaction<(LearningExperience? Experience, bool Created)>((now, tx) =>
        {
            var existing = Experience(id, tx);
            if (existing is not null)
            {
                if (existing.Dataset != dataset || existing.Key != key || existing.Origin != origin)
                    throw new InvalidOperationException("learning_task_identity_conflict");
                return (existing, false); // Pending tombstones never authorize another native attempt.
            }
            RequireTaskAdmission(now, tx);
            if (Convert.ToInt64(Scalar("SELECT count(*) FROM experiences", tx)) >= MaximumExperiences)
                throw new InvalidOperationException("learning_task_capacity");
            // Bounded rows plus reserved headroom leave room to finish admitted observations.
            // This is a logical storage guard, not an OS disk quota.
            _ = LearningStorage.Measure(files.FullPath, policy.MaxStorageMiB * 1024L * 1024 - ExperienceCompletionHeadroom);
            var readable = TryTaskSource(dataset, out var source);
            if (!readable || Scalar("SELECT source FROM task_source_pins WHERE dataset=$d", tx, ("$d", dataset)) is string approved
                && source?.SourceSha256 != approved)
            {
                SetPaused(tx);
                return (null, false); // Commit the pause before reporting refusal; no native call is authorized.
            }
            Execute("""
                INSERT INTO experiences(request_id,dataset,key,origin,boot,started_ns,state,source,expected)
                VALUES($id,$d,$k,$o,$b,$n,'pending',$s,$e)
                """, tx, ("$id", id), ("$d", dataset), ("$k", (int)key), ("$o", origin),
                ("$b", now.Boot), ("$n", now.Nanoseconds), ("$s", (object?)source?.SourceSha256 ?? DBNull.Value),
                ("$e", (object?)source?.ExpectedFor(key) ?? DBNull.Value));
            return (Experience(id, tx)!, true);
        });
        if (result.Experience is null) throw new InvalidOperationException("learning_task_source_conflict");
        return (result.Experience, result.Created);
    }
}
