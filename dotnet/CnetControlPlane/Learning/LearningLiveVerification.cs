namespace CnetControlPlane.Learning;

internal sealed record LearningLiveResult(string SourceSha256, string ActiveSha256, ulong Revision,
    string Boot, long StartNanoseconds, long EndNanoseconds,
    int CorrectAnswers, int CorrectAbstentions, int MissingAnswers, int WrongAnswers)
{
    public bool Passed => MissingAnswers == 0 && WrongAnswers == 0;
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
        CancellationToken cancellation = default)
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
        }
        var after = await Exchange(status).ConfigureAwait(false);
        CheckTime();
        if (before != after) throw new InvalidOperationException("learning_live_verification_native_changed");
        var finalSource = readSource();
        if (finalSource.SourceSha256 != reference.SourceSha256 || finalSource.Dataset != dataset.Id || finalSource.Authority != dataset.Authority)
            throw new InvalidOperationException("learning_live_verification_source_changed");
        CheckTime();
        return new(reference.SourceSha256, before.Active, before.Revision, start.Boot, start.Nanoseconds, last,
            correct, abstentions, missing, wrong);
    }
}
