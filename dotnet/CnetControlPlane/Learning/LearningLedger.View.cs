namespace CnetControlPlane.Learning;

internal sealed record LearningOutstanding(LearningJob Job, string State);

internal sealed partial class LearningLedger
{
    internal bool IsPaused
    {
        get { files.AssertPathIdentity(); return Convert.ToInt64(Scalar("SELECT paused FROM configuration WHERE id=1")) != 0; }
    }
    internal ControlStatus? NativeStatus
    {
        get { files.AssertPathIdentity(); return Scalar("SELECT frame FROM native_binding WHERE id=1") is string text ? ParseFrame(text) : null; }
    }
    internal LearningOutstanding? Outstanding
    {
        get
        {
            files.AssertPathIdentity();
            using var command = Command("SELECT id,dataset,source,state FROM jobs WHERE state IN('reserved','staged','evaluated','probation') LIMIT 2", null);
            using var reader = command.ExecuteReader();
            if (!reader.Read()) return null;
            var result = new LearningOutstanding(new(reader.GetInt64(0), reader.GetString(1), reader.GetString(2)), reader.GetString(3));
            if (reader.Read()) throw new InvalidOperationException("learning_supervisor_multiple_jobs");
            Dataset(result.Job.Dataset);
            if (!IsHash(result.Job.SourceSha256)) throw new InvalidOperationException("learning_supervisor_job_integrity");
            return result;
        }
    }
    internal IReadOnlyList<byte> DemandedKeys(string dataset)
    {
        Dataset(dataset); files.AssertPathIdentity();
        using var command = Command("SELECT key FROM demand WHERE dataset=$d AND misses>0 ORDER BY key LIMIT 257", null, ("$d", dataset));
        using var reader = command.ExecuteReader();
        var keys = new List<byte>();
        while (reader.Read())
        {
            var key = reader.GetInt64(0);
            if (keys.Count == 256 || key is < 0 or > 255) throw new InvalidOperationException("learning_supervisor_demand_integrity");
            keys.Add((byte)key);
        }
        return keys.AsReadOnly();
    }
}
