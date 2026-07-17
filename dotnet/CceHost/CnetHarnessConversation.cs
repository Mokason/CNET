using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace CNET.CceHost;

/// <summary>
/// Owns bounded, per-conversation short-term chat history for an
/// <see cref="IChatClient"/>. The underlying client remains stateless: this
/// class is the explicit authority for retained turns.
/// </summary>
public sealed class CnetHarnessConversation : IDisposable
{
    private sealed record Turn(string User, string Assistant);

    private readonly IChatClient _client;
    private readonly string _systemPrompt;
    private readonly int _maxRetainedTurns;
    private readonly int _maxRetainedCharacters;
    private readonly bool _ownsClient;
    private readonly List<Turn> _turns = new();
    private readonly object _stateGate = new();
    private readonly SemaphoreSlim _serial = new(1, 1);
    private bool _disposed;
    private int _retainedCharacters;

    public CnetHarnessConversation(IChatClient client, string systemPrompt,
        int maxRetainedTurns = 8, int maxRetainedCharacters = 32768,
        bool ownsClient = false)
    {
        ArgumentNullException.ThrowIfNull(client);
        ArgumentNullException.ThrowIfNull(systemPrompt);
        if (maxRetainedTurns <= 0)
            throw new ArgumentOutOfRangeException(nameof(maxRetainedTurns));
        if (maxRetainedCharacters <= systemPrompt.Length)
            throw new ArgumentOutOfRangeException(nameof(maxRetainedCharacters));

        _client = client;
        _systemPrompt = systemPrompt;
        _maxRetainedTurns = maxRetainedTurns;
        _maxRetainedCharacters = maxRetainedCharacters;
        _ownsClient = ownsClient;
        _retainedCharacters = systemPrompt.Length;
    }

    public int RetainedTurnCount
    {
        get { lock (_stateGate) return _turns.Count; }
    }

    public int RetainedCharacterCount
    {
        get { lock (_stateGate) return _retainedCharacters; }
    }

    public async Task<string> SendAsync(string user)
    {
        ArgumentException.ThrowIfNullOrEmpty(user);
        if (_systemPrompt.Length + user.Length > _maxRetainedCharacters)
        {
            throw new ArgumentException(
                "User message exceeds the retained conversation character budget.",
                nameof(user));
        }

        await _serial.WaitAsync().ConfigureAwait(false);
        try
        {
            (string Role, string Content)[] request;
            lock (_stateGate)
            {
                ObjectDisposedException.ThrowIf(_disposed, this);
                request = BuildBoundedRequest(user);
            }

            string assistant = await _client.ChatAsync(request).ConfigureAwait(false);
            assistant ??= string.Empty;

            lock (_stateGate)
            {
                ObjectDisposedException.ThrowIf(_disposed, this);
                AddRetainedTurn(user, assistant);
            }
            return assistant;
        }
        finally
        {
            _serial.Release();
        }
    }

    public IReadOnlyList<(string Role, string Content)> Snapshot()
    {
        lock (_stateGate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            var snapshot = new (string Role, string Content)[1 + _turns.Count * 2];
            snapshot[0] = ("system", _systemPrompt);
            int index = 1;
            foreach (Turn turn in _turns)
            {
                snapshot[index++] = ("user", turn.User);
                snapshot[index++] = ("assistant", turn.Assistant);
            }
            return snapshot;
        }
    }

    public void Dispose()
    {
        _serial.Wait();
        try
        {
            lock (_stateGate)
            {
                if (_disposed) return;
                _disposed = true;
                _turns.Clear();
                _retainedCharacters = 0;
            }
            if (_ownsClient && _client is IDisposable disposable)
                disposable.Dispose();
        }
        finally
        {
            _serial.Release();
        }
    }

    private (string Role, string Content)[] BuildBoundedRequest(string user)
    {
        int firstTurn = _turns.Count;
        int selectedTurns = 0;
        int selectedCharacters = _systemPrompt.Length + user.Length;
        for (int i = _turns.Count - 1; i >= 0; --i)
        {
            if (selectedTurns >= _maxRetainedTurns - 1) break;
            Turn candidate = _turns[i];
            int candidateCharacters = candidate.User.Length
                + candidate.Assistant.Length;
            if (candidateCharacters >
                _maxRetainedCharacters - selectedCharacters) break;
            selectedCharacters += candidateCharacters;
            selectedTurns++;
            firstTurn = i;
        }

        var request = new (string Role, string Content)[2 + selectedTurns * 2];
        request[0] = ("system", _systemPrompt);
        int index = 1;
        for (int i = firstTurn; i < _turns.Count; ++i)
        {
            Turn turn = _turns[i];
            request[index++] = ("user", turn.User);
            request[index++] = ("assistant", turn.Assistant);
        }
        request[index] = ("user", user);
        return request;
    }

    private void AddRetainedTurn(string user, string assistant)
    {
        while (_turns.Count > 0 &&
               (_turns.Count >= _maxRetainedTurns ||
                user.Length > _maxRetainedCharacters - _retainedCharacters ||
                assistant.Length > _maxRetainedCharacters
                    - _retainedCharacters - user.Length))
        {
            RemoveOldestTurn();
        }

        int assistantBudget = _maxRetainedCharacters
            - _retainedCharacters - user.Length;
        string retainedAssistant = assistant[..Math.Min(
            assistant.Length, assistantBudget)];
        _turns.Add(new Turn(user, retainedAssistant));
        _retainedCharacters += user.Length + retainedAssistant.Length;
    }

    private void RemoveOldestTurn()
    {
        Turn oldest = _turns[0];
        _turns.RemoveAt(0);
        _retainedCharacters -= oldest.User.Length + oldest.Assistant.Length;
    }
}
