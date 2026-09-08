namespace CnetControlPlane.Learning;

internal sealed record LearningGap(string Dataset, byte Key, string Origin, string? SourceSha256,
    int Requests, int PendingApprovals, int ApprovedRequests, long FirstSequence, long LastSequence,
    string LatestState, IReadOnlyDictionary<string, int> States, int ObservedRequestSamples, double? MeanRequestMilliseconds);
internal sealed record LearningGapReport(long SnapshotSequence, int Observations, int TotalGroups, bool Truncated,
    bool TrainingEligible, int WorkerBudgetSeconds, double? EstimatedLearningSeconds, string CostStatus,
    IReadOnlyList<LearningGap> Groups);

internal sealed partial class LearningLedger
{
    internal LearningGapReport Gaps(int limit)
    {
        if (limit is < 1 or > 32) throw new ArgumentException("learning_gap_limit");
        files.AssertPathIdentity(); RequireSchemaVersion();
        var experiences = new List<LearningExperience>();
        // One SQLite read snapshot, including rows whose approval changed since
        // insertion. Never page the source and combine inconsistent snapshots.
        using (var cmd = Command("SELECT * FROM experiences ORDER BY sequence LIMIT 4097", null))
        using (var rows = cmd.ExecuteReader())
            while (rows.Read()) experiences.Add(ReadExperience(rows));
        if (experiences.Count > MaximumExperiences) throw new InvalidOperationException("learning_task_capacity");
        files.AssertPathIdentity();
        var groups = experiences.GroupBy(row => (row.Dataset, row.Key, row.Origin, row.SourceSha256))
            .Select(group =>
            {
                var rows = group.ToArray();
                var elapsed = rows.Where(row => row.State != "pending" && row.FinishedBoot == row.Boot
                    && row.FinishedNanoseconds >= row.StartedNanoseconds)
                    .Select(row => (row.FinishedNanoseconds!.Value - row.StartedNanoseconds) / 1_000_000.0).ToArray();
                return new LearningGap(group.Key.Dataset, group.Key.Key, group.Key.Origin, group.Key.SourceSha256,
                    rows.Length, rows.Count(row => row.State is "miss" or "awaiting_evidence"
                        && row.ApprovedSourceSha256 is null && !row.ReviewConflict),
                    rows.Count(row => row.ApprovedSourceSha256 is not null), rows[0].Sequence, rows[^1].Sequence,
                    rows[^1].State, rows.GroupBy(row => row.State).ToDictionary(state => state.Key, state => state.Count()),
                    elapsed.Length, elapsed.Length == 0 ? null : elapsed.Average());
            }).OrderByDescending(group => group.PendingApprovals).ThenByDescending(group => group.Requests)
            .ThenBy(group => group.FirstSequence).ToArray();
        // Frequency is an inspection order, not a learned scheduler or an
        // acquisition-cost estimate. WorkerSeconds is a per-worker policy cap.
        return new(experiences.Count == 0 ? 0 : experiences[^1].Sequence, experiences.Count, groups.Length,
            groups.Length > limit, false, policy.WorkerSeconds, null, "acquisition_cost_not_measured",
            Array.AsReadOnly(groups.Take(limit).ToArray()));
    }
}
