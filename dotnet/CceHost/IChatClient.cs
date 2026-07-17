using System.Threading;
using System.Threading.Tasks;

namespace CNET.CceHost;

/// <summary>Small chat-client abstraction so agents can pick a backend.</summary>
public interface IChatClient
{
    /// <summary>Send a role/content transcript, return the assistant's text.</summary>
    Task<string> ChatAsync((string Role, string Content)[] messages);
}

/// <summary>
/// Optional capability for warming a client's exact prompt state without
/// committing generated text to conversation history.
/// </summary>
public interface IPrefillChatClient
{
    Task PrefillAsync(
        (string Role, string Content)[] messages,
        CancellationToken cancellationToken);
}
