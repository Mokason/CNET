using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace CNET.CceHost;

public interface IAsyncContextSource
{
    ValueTask<string> CollectAsync(
        string draft,
        int maxCharacters,
        CancellationToken cancellationToken);
}

public sealed record AsyncContextSnapshot(
    long Version,
    string Draft,
    string HistoryContext,
    string ConnectionContext,
    string CombinedContext,
    bool PrefillCompleted);

public sealed class AsyncContextPipelineOptions
{
    public TimeSpan TypingDebounce { get; init; } = TimeSpan.FromMilliseconds(120);
    public int MaxDraftCharacters { get; init; } = 16 * 1024;
    public int MaxHistoryCharacters { get; init; } = 2048;
    public int MaxConnectionCharacters { get; init; } = 1024;
    public int MaxCombinedCharacters { get; init; } = 3072;
}

/// <summary>
/// Runs bounded CPU context enrichment concurrently while keeping speculative
/// and final model work on the conversation's single serialized model lane.
/// </summary>
public sealed class AsyncContextPipeline : IAsyncDisposable
{
    private readonly CnetHarnessConversation _conversation;
    private readonly IAsyncContextSource _historySource;
    private readonly IAsyncContextSource _connectionSource;
    private readonly AsyncContextPipelineOptions _options;
    private readonly bool _ownsConversation;
    private readonly object _gate = new();
    private readonly CancellationTokenSource _lifetime = new();

    private CancellationTokenSource? _activeCancellation;
    private Task<AsyncContextSnapshot>? _activeTask;
    private CancellationTokenSource? _finalCancellation;
    private Task<string>? _finalTask;
    private AsyncContextSnapshot? _current;
    private long _version;
    private bool _disposed;

    public AsyncContextPipeline(
        CnetHarnessConversation conversation,
        IAsyncContextSource historySource,
        IAsyncContextSource connectionSource,
        AsyncContextPipelineOptions? options = null,
        bool ownsConversation = false)
    {
        ArgumentNullException.ThrowIfNull(conversation);
        ArgumentNullException.ThrowIfNull(historySource);
        ArgumentNullException.ThrowIfNull(connectionSource);
        options ??= new AsyncContextPipelineOptions();
        if (options.TypingDebounce < TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(options));
        if (options.MaxDraftCharacters <= 0 ||
            options.MaxHistoryCharacters <= 0 ||
            options.MaxConnectionCharacters <= 0 ||
            options.MaxCombinedCharacters <= 0)
            throw new ArgumentOutOfRangeException(nameof(options));

        _conversation = conversation;
        _historySource = historySource;
        _connectionSource = connectionSource;
        _options = options;
        _ownsConversation = ownsConversation;
    }

    public AsyncContextSnapshot? Current
    {
        get { lock (_gate) return _current; }
    }

    public Task<AsyncContextSnapshot> UpdateDraftAsync(
        string draft,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(draft);
        ValidateDraftLength(draft);

        CancellationTokenSource? previousCancellation;
        Task<AsyncContextSnapshot>? previousTask;
        CancellationTokenSource linked;
        Task<AsyncContextSnapshot> task;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            previousCancellation = _activeCancellation;
            previousTask = _activeTask;

            linked = CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken, _lifetime.Token);
            long version = ++_version;
            _activeCancellation = linked;
            task = ProcessDraftAsync(version, draft, linked.Token);
            _activeTask = task;
        }

        CancelAndDisposeWhenComplete(previousCancellation, previousTask);
        ReleaseCurrentDraftWhenComplete(linked, task);
        return task;
    }

    public Task<string> SendAsync(
        string user,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrEmpty(user);
        ValidateDraftLength(user);

        CancellationTokenSource? draftCancellation;
        Task<AsyncContextSnapshot>? draftTask;
        CancellationTokenSource? completedFinalCancellation;
        Task<string>? completedFinalTask;
        AsyncContextSnapshot? snapshot;
        CancellationTokenSource finalCancellation;
        Task<string> finalTask;
        long finalVersion;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (_finalTask is { IsCompleted: false })
            {
                throw new InvalidOperationException(
                    "A final send is already active for this pipeline.");
            }

            finalVersion = ++_version;
            draftCancellation = _activeCancellation;
            draftTask = _activeTask;
            _activeCancellation = null;
            _activeTask = null;
            snapshot = string.Equals(_current?.Draft, user,
                StringComparison.Ordinal) ? _current : null;

            completedFinalCancellation = _finalCancellation;
            completedFinalTask = _finalTask;
            finalCancellation = CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken, _lifetime.Token);
            finalTask = SendCoreAsync(
                finalVersion, user, snapshot, draftTask,
                finalCancellation.Token);
            _finalCancellation = finalCancellation;
            _finalTask = finalTask;
        }

        CancelAndDisposeWhenComplete(draftCancellation, draftTask);
        CancelAndDisposeWhenComplete(
            completedFinalCancellation, completedFinalTask);
        return finalTask;
    }

    private async Task<string> SendCoreAsync(
        long finalVersion,
        string user,
        AsyncContextSnapshot? snapshot,
        Task<AsyncContextSnapshot>? draftTask,
        CancellationToken cancellationToken)
    {
        await Task.Yield();
        if (draftTask is not null)
        {
            try { await draftTask.ConfigureAwait(false); }
            catch (OperationCanceledException) { }
            catch { /* Speculation must never block final generation. */ }
        }

        cancellationToken.ThrowIfCancellationRequested();
        if (snapshot is null)
        {
            (string history, string connections) = await CollectSourcesAsync(
                user, cancellationToken).ConfigureAwait(false);
            snapshot = CreateSnapshot(
                finalVersion, user, history, connections,
                prefillCompleted: false);
        }
        else
        {
            snapshot = snapshot with { Version = finalVersion };
        }

        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (finalVersion != _version)
                throw new OperationCanceledException();
            _current = snapshot;
        }

        return await _conversation.SendAsync(
            user, snapshot.CombinedContext, cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask DisposeAsync()
    {
        CancellationTokenSource? activeCancellation;
        Task<AsyncContextSnapshot>? activeTask;
        CancellationTokenSource? finalCancellation;
        Task<string>? finalTask;
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
            ++_version;
            _current = null;
            activeCancellation = _activeCancellation;
            activeTask = _activeTask;
            finalCancellation = _finalCancellation;
            finalTask = _finalTask;
            _activeCancellation = null;
            _activeTask = null;
            _finalCancellation = null;
            _finalTask = null;
        }

        _lifetime.Cancel();
        activeCancellation?.Cancel();
        finalCancellation?.Cancel();
        if (activeTask is not null)
        {
            try { await activeTask.ConfigureAwait(false); }
            catch (OperationCanceledException) { }
            catch { /* Disposing background speculation is best-effort. */ }
        }
        if (finalTask is not null)
        {
            try { await finalTask.ConfigureAwait(false); }
            catch (OperationCanceledException) { }
            catch { /* The caller observes final-generation failure. */ }
        }
        activeCancellation?.Dispose();
        finalCancellation?.Dispose();
        _lifetime.Dispose();
        if (_ownsConversation)
            _conversation.Dispose();
    }

    private void ValidateDraftLength(string draft)
    {
        if (draft.Length > _options.MaxDraftCharacters)
        {
            throw new ArgumentException(
                $"Draft length {draft.Length} exceeds the configured maximum "
                + $"of {_options.MaxDraftCharacters} characters.",
                nameof(draft));
        }
    }

    private async Task<AsyncContextSnapshot> ProcessDraftAsync(
        long version,
        string draft,
        CancellationToken cancellationToken)
    {
        await Task.Yield();
        if (_options.TypingDebounce > TimeSpan.Zero)
            await Task.Delay(_options.TypingDebounce, cancellationToken)
                .ConfigureAwait(false);

        (string history, string connections) = await CollectSourcesAsync(
            draft, cancellationToken).ConfigureAwait(false);
        cancellationToken.ThrowIfCancellationRequested();

        AsyncContextSnapshot snapshot = CreateSnapshot(
            version, draft, history, connections, prefillCompleted: false);
        Publish(version, snapshot);

        if (draft.Length == 0)
            return snapshot;

        try
        {
            bool completed = await _conversation.PrefillAsync(
                draft, snapshot.CombinedContext, cancellationToken)
                .ConfigureAwait(false);
            cancellationToken.ThrowIfCancellationRequested();
            if (completed)
            {
                snapshot = snapshot with { PrefillCompleted = true };
                Publish(version, snapshot);
            }
        }
        catch (OperationCanceledException) { throw; }
        catch
        {
            // Prefill is an optimization. Its failure cannot remove usable CPU
            // context or make final generation unavailable.
        }
        return snapshot;
    }

    private async Task<(string History, string Connections)> CollectSourcesAsync(
        string draft,
        CancellationToken cancellationToken)
    {
        Task<string> historyTask = CollectSourceAsync(
            _historySource, draft, _options.MaxHistoryCharacters,
            cancellationToken);
        Task<string> connectionTask = CollectSourceAsync(
            _connectionSource, draft, _options.MaxConnectionCharacters,
            cancellationToken);
        await Task.WhenAll(historyTask, connectionTask).ConfigureAwait(false);
        return (await historyTask.ConfigureAwait(false),
            await connectionTask.ConfigureAwait(false));
    }

    private static async Task<string> CollectSourceAsync(
        IAsyncContextSource source,
        string draft,
        int maxCharacters,
        CancellationToken cancellationToken)
    {
        try
        {
            string value = await source.CollectAsync(
                draft, maxCharacters, cancellationToken).ConfigureAwait(false);
            cancellationToken.ThrowIfCancellationRequested();
            return Clip(value, maxCharacters);
        }
        catch (OperationCanceledException) { throw; }
        catch { return string.Empty; }
    }

    private AsyncContextSnapshot CreateSnapshot(
        long version,
        string draft,
        string history,
        string connections,
        bool prefillCompleted)
    {
        var combined = new StringBuilder();
        AppendSection(combined, "Relevant history", history);
        AppendSection(combined, "Word connections", connections);
        return new AsyncContextSnapshot(
            version,
            draft,
            history,
            connections,
            Clip(combined.ToString(), _options.MaxCombinedCharacters),
            prefillCompleted);
    }

    private void Publish(
        long version,
        AsyncContextSnapshot snapshot)
    {
        lock (_gate)
        {
            if (_disposed || version != _version)
                throw new OperationCanceledException();
            _current = snapshot;
        }
    }

    private static void AppendSection(
        StringBuilder destination, string title, string value)
    {
        if (string.IsNullOrEmpty(value)) return;
        if (destination.Length > 0) destination.Append('\n');
        destination.Append('[').Append(title).Append("]\n").Append(value);
    }

    private static string Clip(string? value, int maxCharacters)
    {
        if (string.IsNullOrEmpty(value)) return string.Empty;
        return value.Length <= maxCharacters
            ? value
            : value[..maxCharacters];
    }

    private static void CancelAndDisposeWhenComplete(
        CancellationTokenSource? cancellation,
        Task? task)
    {
        if (cancellation is null) return;
        cancellation.Cancel();
        if (task is null || task.IsCompleted)
        {
            cancellation.Dispose();
            return;
        }
        _ = task.ContinueWith(
            _ => cancellation.Dispose(),
            CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
    }

    private void ReleaseCurrentDraftWhenComplete(
        CancellationTokenSource cancellation,
        Task<AsyncContextSnapshot> task)
    {
        _ = task.ContinueWith(
            _ =>
            {
                bool release;
                lock (_gate)
                {
                    release = ReferenceEquals(_activeCancellation, cancellation)
                        && ReferenceEquals(_activeTask, task);
                    if (release)
                    {
                        _activeCancellation = null;
                        _activeTask = null;
                    }
                }
                if (release)
                    cancellation.Dispose();
            },
            CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
    }
}

public sealed class RelevantHistoryContextSource : IAsyncContextSource
{
    private readonly Func<IReadOnlyList<(string Role, string Content)>> _snapshot;

    public RelevantHistoryContextSource(
        Func<IReadOnlyList<(string Role, string Content)>> snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        _snapshot = snapshot;
    }

    public ValueTask<string> CollectAsync(
        string draft,
        int maxCharacters,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        List<ContextTurn> turns = ContextTerms.ReadCompleteTurns(_snapshot());
        if (turns.Count == 0)
            return ValueTask.FromResult(string.Empty);

        HashSet<string> query = ContextTerms.Tokenize(draft).ToHashSet(
            StringComparer.Ordinal);
        var ranked = turns
            .Select((turn, index) => new
            {
                Turn = turn,
                Index = index,
                Score = ContextTerms.Tokenize(turn.User + " " + turn.Assistant)
                    .Distinct(StringComparer.Ordinal)
                    .Count(query.Contains),
            })
            .OrderByDescending(item => item.Score)
            .ThenByDescending(item => item.Index)
            .ToList();
        if (ranked[0].Score == 0)
            ranked = ranked.Take(1).ToList();
        else
            ranked = ranked.Where(item => item.Score > 0).ToList();

        var output = new StringBuilder();
        foreach (var item in ranked)
        {
            cancellationToken.ThrowIfCancellationRequested();
            string text = $"User: {item.Turn.User}\nAssistant: {item.Turn.Assistant}";
            int separator = output.Length == 0 ? 0 : 2;
            if (text.Length + separator > maxCharacters - output.Length)
                continue;
            if (separator != 0) output.Append("\n\n");
            output.Append(text);
        }
        return ValueTask.FromResult(output.ToString());
    }
}

public sealed class WordConnectionContextSource : IAsyncContextSource
{
    private const int NeighborWindow = 3;
    private const int NeighborsPerTerm = 5;
    private readonly Func<IReadOnlyList<(string Role, string Content)>> _snapshot;

    public WordConnectionContextSource(
        Func<IReadOnlyList<(string Role, string Content)>> snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        _snapshot = snapshot;
    }

    public ValueTask<string> CollectAsync(
        string draft,
        int maxCharacters,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        HashSet<string> query = ContextTerms.Tokenize(draft)
            .Where(term => term.Length > 1)
            .ToHashSet(StringComparer.Ordinal);
        if (query.Count == 0)
            return ValueTask.FromResult(string.Empty);

        var counts = new Dictionary<string, Dictionary<string, int>>(
            StringComparer.Ordinal);
        foreach (ContextTurn turn in ContextTerms.ReadCompleteTurns(_snapshot()))
        {
            cancellationToken.ThrowIfCancellationRequested();
            List<string> words = ContextTerms.Tokenize(
                turn.User + " " + turn.Assistant).ToList();
            for (int i = 0; i < words.Count; ++i)
            {
                string source = words[i];
                if (!query.Contains(source)) continue;
                if (!counts.TryGetValue(source, out var neighbors))
                {
                    neighbors = new Dictionary<string, int>(StringComparer.Ordinal);
                    counts[source] = neighbors;
                }
                int start = Math.Max(0, i - NeighborWindow);
                int end = Math.Min(words.Count - 1, i + NeighborWindow);
                for (int j = start; j <= end; ++j)
                {
                    string neighbor = words[j];
                    if (j == i || neighbor == source || neighbor.Length <= 1)
                        continue;
                    neighbors[neighbor] = neighbors.GetValueOrDefault(neighbor) + 1;
                }
            }
        }

        var output = new StringBuilder();
        foreach (var pair in counts.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            cancellationToken.ThrowIfCancellationRequested();
            string[] neighbors = pair.Value
                .OrderByDescending(item => item.Value)
                .ThenBy(item => item.Key, StringComparer.Ordinal)
                .Take(NeighborsPerTerm)
                .Select(item => item.Key)
                .ToArray();
            if (neighbors.Length == 0) continue;
            string line = $"{pair.Key} -> {string.Join(", ", neighbors)}";
            int separator = output.Length == 0 ? 0 : 1;
            if (line.Length + separator > maxCharacters - output.Length)
                break;
            if (separator != 0) output.Append('\n');
            output.Append(line);
        }
        return ValueTask.FromResult(output.ToString());
    }
}

internal sealed record ContextTurn(string User, string Assistant);

internal static partial class ContextTerms
{
    [GeneratedRegex("[\\p{L}\\p{N}_']+", RegexOptions.CultureInvariant)]
    private static partial Regex TermRegex();

    internal static IEnumerable<string> Tokenize(string value)
    {
        foreach (Match match in TermRegex().Matches(value ?? string.Empty))
            yield return match.Value.ToLowerInvariant();
    }

    internal static List<ContextTurn> ReadCompleteTurns(
        IReadOnlyList<(string Role, string Content)> messages)
    {
        var turns = new List<ContextTurn>();
        for (int i = 0; i + 1 < messages.Count; ++i)
        {
            if (!string.Equals(messages[i].Role, "user",
                    StringComparison.OrdinalIgnoreCase) ||
                !string.Equals(messages[i + 1].Role, "assistant",
                    StringComparison.OrdinalIgnoreCase))
                continue;
            turns.Add(new ContextTurn(messages[i].Content, messages[i + 1].Content));
            ++i;
        }
        return turns;
    }
}
