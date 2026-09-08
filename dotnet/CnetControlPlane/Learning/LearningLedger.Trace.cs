namespace CnetControlPlane.Learning;

internal sealed record LearningTraceMatch(string RequestId, LearningExperience? Experience);

internal sealed partial class LearningLedger
{
    internal IReadOnlyList<LearningTraceMatch> TraceExperiences(IReadOnlyList<string> ids)
    {
        if (ids.Count is < 1 or > 10 || ids.Any(id => !IsRequestId(id)) || ids.Distinct().Count() != ids.Count)
            throw new ArgumentException("learning_task_trace_refused");
        files.AssertPathIdentity(); RequireSchemaVersion();
        // One read snapshot. Parameters are identifiers, never SQL fragments.
        var parameters = ids.Select((id, index) => ("$id" + index, (object)id)).ToArray();
        var found = new Dictionary<string, LearningExperience>(StringComparer.Ordinal);
        using (var cmd = Command("SELECT * FROM experiences WHERE request_id IN ("
                   + string.Join(',', parameters.Select(parameter => parameter.Item1)) + ")", null, parameters))
        using (var rows = cmd.ExecuteReader())
            while (rows.Read())
            {
                var row = ReadExperience(rows);
                found.Add(row.RequestId, row);
            }
        files.AssertPathIdentity();
        return Array.AsReadOnly(ids.Select(id => new LearningTraceMatch(id, found.GetValueOrDefault(id))).ToArray());
    }
}
