namespace CnetControlPlane.Learning;

internal sealed record LearningLiveResult(string SourceSha256, string ActiveSha256, ulong Revision,
    string Boot, long StartNanoseconds, long EndNanoseconds,
    int CorrectAnswers, int CorrectAbstentions, int MissingAnswers, int WrongAnswers,
    int SymbolKeys = 0, int CorrectSymbolAnswers = 0, int CorrectSymbolAbstentions = 0,
    int MissingSymbolAnswers = 0, int WrongSymbolAnswers = 0)
{
    public bool Passed => MissingAnswers == 0 && WrongAnswers == 0
        && MissingSymbolAnswers == 0 && WrongSymbolAnswers == 0;
}

/// <summary>
/// An observation, not promotion authority or a run certificate. Probe traffic
/// never becomes demand or labels. Endpoint identity checks reject a mixed
/// native generation; they do not authenticate source truth or hostile owners.
/// </summary>
internal static class LearningLiveVerification
{
    internal static async Task<LearningLiveResult> ObserveAsync(LearningDataset dataset,
        Func<LocalTableReference> readSource, Func<CancellationToken, Task<ControlStatus>> status,
        Func<byte, CancellationToken, Task<LearningAskResult>> ask, int seconds, ILearningClock clock,
        CancellationToken cancellation = default,
        Func<string, CancellationToken, Task<LearningAskResult>>? symbolAsk = null,
        bool stopOnMismatch = false)
    {
        if (seconds is < 1 or > 120) throw new ArgumentException("learning_live_verification_budget");
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        stop.CancelAfter(TimeSpan.FromSeconds(seconds));
        var start = clock.Now;
        if (string.IsNullOrEmpty(start.Boot) || start.Nanoseconds < 0)
            throw new InvalidOperationException("learning_live_verification_clock");
        var last = start.Nanoseconds;
        void CheckTime()
        {
            stop.Token.ThrowIfCancellationRequested();
            var now = clock.Now;
            if (now.Boot != start.Boot || now.Nanoseconds < last
                || now.Nanoseconds - start.Nanoseconds >= seconds * 1_000_000_000L)
                throw new InvalidOperationException("learning_live_verification_clock");
            last = now.Nanoseconds;
        }
        async Task<T> Exchange<T>(Func<CancellationToken, Task<T>> operation)
        {
            CheckTime();
            var pending = operation(stop.Token);
            try
            {
                // Runtime timers need not include suspend. Poll the one global
                // boot deadline even while a later client exchange is pending.
                while (!pending.IsCompleted)
                {
                    CheckTime();
                    await Task.WhenAny(pending, Task.Delay(20, stop.Token)).ConfigureAwait(false);
                }
                var value = await pending.ConfigureAwait(false);
                CheckTime();
                return value;
            }
            catch
            {
                stop.Cancel();
                // The fixed clients honor cancellation and reap/close their
                // child/socket. Do not detach their unfinished cleanup.
                try { await pending.ConfigureAwait(false); }
                catch (Exception error) when (error is OperationCanceledException or InvalidOperationException
                    or ArgumentException or IOException or UnauthorizedAccessException) { }
                throw;
            }
        }
        CheckTime();
        var reference = readSource();
        if (reference.Dataset != dataset.Id || reference.Authority != dataset.Authority)
            throw new InvalidOperationException("learning_live_verification_source");
        if (reference.Symbols is not null && (symbolAsk is null
            || reference.Symbols.VocabularySha256 != dataset.SymbolVocabularySha256)
            || reference.Symbols is null && dataset.SymbolVocabularySha256 is not null)
            throw new InvalidOperationException("learning_live_verification_symbol_source");
        CheckTime();
        var before = await Exchange(status).ConfigureAwait(false);
        if (!before.Ok || !before.Durable || before.Reason != ControlReason.Ok || before.Active is null || before.Staged is not null)
            throw new InvalidOperationException("learning_live_verification_native");
        var correct = 0; var abstentions = 0; var missing = 0; var wrong = 0;
        for (var key = 0; key < 256; key++)
        {
            CheckTime();
            var answer = await Exchange(token => ask((byte)key, token)).ConfigureAwait(false);
            CheckTime();
            var expected = reference.ExpectedFor((byte)key);
            if (answer.Verified)
            {
                if (expected.HasValue && answer.Value == expected) correct++;
                else wrong++;
            }
            else if (expected.HasValue) missing++;
            else abstentions++;
            // Probation must regain control synchronously on the first known
            // failure, before another await can cancel or obscure it. Ordinary
            // read-only verification instead retains the complete counts.
            if (stopOnMismatch && (missing != 0 || wrong != 0))
                throw new InvalidOperationException("learning_live_verification_mismatch");
        }
        var symbolCorrect = 0; var symbolAbstentions = 0; var symbolMissing = 0; var symbolWrong = 0;
        if (reference.Symbols is { } symbols)
        {
            for (var row = 0; row < symbols.Keys.Count; row++)
            {
                var answer = await Exchange(token => symbolAsk!(symbols.Keys[row], token)).ConfigureAwait(false);
                if (!answer.Verified) symbolMissing++;
                else if (answer.Text == symbols.Labels[row]) symbolCorrect++;
                else symbolWrong++;
                if (stopOnMismatch && (symbolMissing != 0 || symbolWrong != 0))
                    throw new InvalidOperationException("learning_live_verification_mismatch");
            }
            var unknown = await Exchange(token => symbolAsk!(symbols.UnknownToken, token)).ConfigureAwait(false);
            if (unknown.Verified) symbolWrong++; else symbolAbstentions++;
            if (stopOnMismatch && symbolWrong != 0)
                throw new InvalidOperationException("learning_live_verification_mismatch");
        }
        var after = await Exchange(status).ConfigureAwait(false);
        CheckTime();
        if (before != after) throw new InvalidOperationException("learning_live_verification_native_changed");
        var finalSource = readSource();
        if (finalSource.SourceSha256 != reference.SourceSha256 || finalSource.Dataset != dataset.Id || finalSource.Authority != dataset.Authority)
            throw new InvalidOperationException("learning_live_verification_source_changed");
        CheckTime();
        return new(reference.SourceSha256, before.Active, before.Revision, start.Boot, start.Nanoseconds, last,
            correct, abstentions, missing, wrong, reference.Symbols?.Keys.Count ?? 0,
            symbolCorrect, symbolAbstentions, symbolMissing, symbolWrong);
    }
}
