using System.Text.Json;

namespace CnetControlPlane.Learning;

internal static class LearningTaskCommand
{
    internal static int Execute(LearningLedger ledger, LearningRuntime native, LearningPolicy policy,
        string root, string origin, string requestId, string text, string correlation)
    {
        var proposal = LearningTaskParser.Propose(text);
        if (proposal.Status == "ready" && !policy.Datasets.Any(d => d.Id == proposal.Dataset && d.SymbolVocabularySha256 is null))
            proposal = new("abstain", "dataset_not_authorized");
        LearningObservation? observation = proposal.Status == "ready"
            ? LearningObservationRunner.Observe(ledger, native, policy, root, proposal.Dataset!, proposal.Key!.Value, origin, requestId)
            : null;
        Console.WriteLine(JsonSerializer.Serialize(new { @event = "learning_task", correlation_id = correlation,
            proposal, replayed = observation?.Replayed ?? false, experience = observation?.Experience }));
        return observation?.Experience.State is "unknown" or "conflict" ? 2 : 0;
    }
}
