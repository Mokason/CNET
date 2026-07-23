using CNET.Cce.CnetHarness;
using CNET.Cce.Llm.Memory;

namespace CNET.Cce.Llm.Orchestration;

/// <summary>Outcome of one sleep cycle, for the journal.</summary>
public sealed record SleepReport(
    int SessionsSummarized, int SummariesRejected, int Surprises, int LinesCompacted);

/// <summary>
/// The rest phase: deep housekeeping that live ticks cannot afford, run only
/// when the system is genuinely idle. Biological framing intended — this is
/// consolidation-during-sleep: episodic sessions get distilled into compact
/// summary blobs, the whole store is re-scanned for surprises (not just the
/// watermark tail), and the janitor sweeps regardless of thresholds.
/// </summary>
/// <remarks>
/// Idleness is structural, not scheduled: sleep requires the store's WRITER
/// LOCK, so a live session anywhere means the system is awake and sleep
/// yields untouched. Summaries are stored under "summary:&lt;sid&gt;" session
/// ids with role "summary" — recallable material, never recent turns, and
/// each is screened by the judge before storage (a bad summary is worse than
/// no summary; taste can veto its own team's homework even though it can
/// never veto the user's).
/// </remarks>
public sealed class SleepPhase(string storePath, Func<ICnetInferenceSession> sessionFactory)
{
    /// <summary>Sessions shorter than this are not worth summarizing.</summary>
    public int MinSessionBlobs { get; init; } = 6;

    /// <summary>Most sessions summarized per sleep — sleep is a batch, not a marathon.</summary>
    public int MaxSummariesPerSleep { get; init; } = 4;

    /// <summary>Optional judge: screens generated summaries before storage.</summary>
    public Judgment.AdaptiveJudge? Judge { get; set; }

    /// <summary>Transcript budget per summarized session (characters).</summary>
    public int TranscriptCap { get; init; } = 6000;

    /// <summary>
    /// Conversation sessions that have enough substance and no summary yet.
    /// Pure function of the view — testable without a model.
    /// </summary>
    public List<string> SessionsNeedingSummary(IMemoryView view)
    {
        var all = view.All();
        var summarized = all.Where(b => b.SessionId.StartsWith("summary:", StringComparison.Ordinal))
                            .Select(b => b.SessionId["summary:".Length..])
                            .ToHashSet(StringComparer.Ordinal);
        return all
            .Where(b => !b.SessionId.StartsWith("doc:", StringComparison.Ordinal) &&
                        !b.SessionId.StartsWith("summary:", StringComparison.Ordinal))
            .GroupBy(b => b.SessionId)
            .Where(g => g.Count() >= MinSessionBlobs && !summarized.Contains(g.Key))
            .OrderBy(g => g.Min(b => b.Id))          // oldest debts first
            .Select(g => g.Key)
            .ToList();
    }

    /// <summary>
    /// One sleep cycle. Returns null when the system is awake (store locked) —
    /// the caller journals the yield and retries on a later tick.
    /// </summary>
    public SleepReport? Run(string? wordsPath, string? recordsDir, string? inboxPath)
    {
        BlobStore store;
        try
        {
            store = BlobStore.Open(storePath);       // the idleness test itself
        }
        catch (IOException)
        {
            return null;                             // awake: a session holds the store
        }

        int summarized = 0, rejected = 0, surprises = 0;
        using (store)
        {
            List<string> due = SessionsNeedingSummary(store).Take(MaxSummariesPerSleep).ToList();
            if (due.Count > 0)
            {
                using ICnetInferenceSession session = sessionFactory();
                foreach (string sid in due)
                {
                    string transcript = BuildTranscript(store, sid);
                    CnetHarnessGenerationResult result = session.Generate(new CnetHarnessGenerateOptions
                    {
                        // Third-person archive framing, learned live: handed a
                        // transcript ending on an unanswered correction, the
                        // model REPLIED to it ("You're right, my apologies!")
                        // instead of describing it.
                        System = "You are writing an archive note ABOUT a finished conversation " +
                                 "between a user and an assistant. You are not a participant: " +
                                 "do not reply to it, do not apologize, do not address anyone. " +
                                 "Describe in third person, 3-6 sentences: durable facts, " +
                                 "decisions, corrections, and outcomes, with verbatim names and " +
                                 "numbers. No speculation, no filler.",
                        User = "--- transcript begins ---\n" + transcript +
                               "\n--- transcript ends ---\nWrite the archive note now.",
                        Role = "sleep",
                        MaxTokens = 320,
                        Sampling = CnetHarnessSamplingMode.Focused,
                    });

                    string summary = result.Text.Trim();
                    if (summary.Length == 0 ||
                        Judge?.Judge(summary).Value == Judgment.Verdict.Bad)
                    {
                        rejected++;
                        continue;                    // a bad summary is worse than none
                    }
                    store.Append("summary:" + sid, 0, "summary",
                        $"[summary of session {sid}]\n{summary}",
                        summary.Length / 4 + 1);
                    summarized++;
                }
            }

            // Full-store surprise rescan — the tail-watermark scan misses
            // cross-session patterns; sleep can afford the whole sweep.
            if (wordsPath is not null && recordsDir is not null && inboxPath is not null &&
                File.Exists(wordsPath))
            {
                var scanner = new SurpriseScanner(wordsPath, recordsDir);
                var found = scanner.Scan(store, sinceBlobId: 0);
                surprises = found.Count;
                if (found.Count > 0 &&
                    scanner.BuildObservationRecord(store, found) is { } rec)
                {
                    Directory.CreateDirectory(recordsDir);
                    File.WriteAllText(Path.Combine(recordsDir, $"skill_{rec.Name}.txt"),
                                      rec.Record);
                    GhostConsolidator.NoteRecordSkill(inboxPath, rec.Name, rec.Record);
                }
            }
        }   // release the lock before compacting (Compact takes it itself)

        int compacted = Math.Max(0, BlobStore.Compact(storePath));
        return new SleepReport(summarized, rejected, surprises, compacted);
    }

    private static string BuildTranscript(IMemoryView view, string sid)
    {
        var sb = new System.Text.StringBuilder();
        foreach (MemoryBlob b in view.All().Where(b => b.SessionId == sid).OrderBy(b => b.Id))
        {
            sb.Append(b.Role).Append(": ").AppendLine(b.Text);
            if (sb.Length > 6000) break;
        }
        return sb.ToString();
    }
}
