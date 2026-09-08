namespace CnetControlPlane.Learning;

internal sealed record LearningObservation(LearningExperience Experience, bool Replayed);

internal static class LearningObservationRunner
{
    internal static LearningObservation Observe(LearningLedger ledger, LearningRuntime runtime, LearningPolicy policy,
        string installation, string dataset, byte key, string origin, string requestId)
    {
        runtime.Verify();
        var pending = ledger.BeginExperience(requestId, dataset, key, origin);
        if (!pending.Created) return new(pending.Experience, true);
        LearningAskResult? answer = null;
        try
        {
            var authorized = policy.Datasets.Single(d => d.Id == dataset && d.SymbolVocabularySha256 is null);
            using var client = new LearningAskClient(Path.Combine(installation, "ipc/ask.sock"), policy.WorkerSeconds);
            answer = client.AskAsync(authorized, key, default).GetAwaiter().GetResult();
            runtime.Verify();
        }
        catch (Exception error) when (error is ArgumentException or InvalidOperationException or IOException
            or UnauthorizedAccessException or OperationCanceledException)
        { answer = null; } // Transport/parser/runtime uncertainty is not a learning miss.
        return new(ledger.FinishExperience(requestId, answer), false);
    }
}
